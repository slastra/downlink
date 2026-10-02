#include "announce.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"

#include "audio_out.h"
#include "ogg_sniff.h"

static const char *TAG = "announce";

static_assert(CONFIG_DL_ANNOUNCE_MAX_KB <= CONFIG_DL_ANNOUNCE_BUDGET_KB,
              "DL_ANNOUNCE_MAX_KB must fit in DL_ANNOUNCE_BUDGET_KB");

#define RATE         48000
#define CHUNK        (16 * 1024)              /* fixed-size PSRAM pieces: no realloc, no fragmentation */
#define MAX_BYTES    ((size_t)CONFIG_DL_ANNOUNCE_MAX_KB * 1024)
#define BUDGET_BYTES ((size_t)CONFIG_DL_ANNOUNCE_BUDGET_KB * 1024)
#define MAX_CHUNKS   ((MAX_BYTES + CHUNK - 1) / CHUNK)
#define JOBS         CONFIG_DL_ANNOUNCE_QUEUE
#define URL_MAX      512
#define OUT_FRAMES   5760                     /* 120 ms, the largest Opus frame */
#define FETCH_DEADLINE_US (10LL * 1000 * 1000)
#define MAX_REDIRECTS 5

/* ---- jobs ------------------------------------------------------------
 *
 * One writer per phase: the fetch task takes a job QUEUED -> FETCHING ->
 * READY (or back to FREE on failure); the player task takes it READY ->
 * PLAYING -> FREE. The lock guards state changes only and is never held
 * across HTTP or allocation.
 */
enum job_state { J_FREE, J_QUEUED, J_FETCHING, J_READY, J_PLAYING };

struct job_t {
    job_state state;
    uint32_t  seq;
    char      id[33];
    char      url[URL_MAX];
    int       volume, duck;
    uint8_t  *chunks[MAX_CHUNKS];
    size_t    nchunks, len;
    size_t    reserved;             /* budget held, bytes */
    uint16_t  preskip;
    uint32_t  duration_ms;
};

static job_t            *s_jobs;        /* in PSRAM: URLs and chunk lists, rarely touched */
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_fetch_q;     /* job slots, in arrival order */
static QueueHandle_t     s_events;      /* announce_event_t */
static uint32_t          s_seq;
static size_t            s_budget_used;
static uint32_t          s_played, s_failed;     /* under the lock */
static uint32_t          s_events_dropped;       /* a hint for logs only */
static volatile int      s_playing_slot = -1;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void emit(const char *id, announce_state_t state, uint32_t duration_ms, const char *error)
{
    announce_event_t e = {};
    strlcpy(e.id, id, sizeof e.id);
    e.state = state;
    e.duration_ms = duration_ms;
    if (error) strlcpy(e.error, error, sizeof e.error);
    if (xQueueSend(s_events, &e, 0) != pdTRUE) s_events_dropped++;
}

static void free_chunks(job_t *j)
{
    for (size_t i = 0; i < j->nchunks; i++) heap_caps_free(j->chunks[i]);
    j->nchunks = 0;
    j->len = 0;
}

/* The oldest active job, by arrival. Caller holds the lock. */
static int oldest_active(int skip)
{
    int best = -1;
    for (int i = 0; i < JOBS; i++) {
        if (i == skip || s_jobs[i].state == J_FREE) continue;
        if (best < 0 || (int32_t)(s_jobs[i].seq - s_jobs[best].seq) < 0) best = i;
    }
    return best;
}

int announce_enqueue(const char *id, const char *url, int volume, int duck_db)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (duck_db > 0) duck_db = 0;
    if (duck_db < -40) duck_db = -40;

    LOCK();
    int slot = -1, active = 0;
    for (int i = 0; i < JOBS; i++) {
        if (s_jobs[i].state == J_FREE) { if (slot < 0) slot = i; }
        else active++;
    }
    if (slot < 0) {
        UNLOCK();
        return 0;
    }
    job_t *j = &s_jobs[slot];
    j->state = J_QUEUED;
    j->seq = ++s_seq;
    strlcpy(j->id, id, sizeof j->id);
    strlcpy(j->url, url, sizeof j->url);
    j->volume = volume;
    j->duck = duck_db;
    j->nchunks = j->len = j->reserved = 0;
    j->preskip = 0;
    j->duration_ms = 0;
    UNLOCK();

    xQueueSend(s_fetch_q, &slot, 0);   /* sized to JOBS: cannot be full */
    ESP_LOGI(TAG, "%s queued at %d: volume %d, duck %d dB", id, active + 1, volume, duck_db);
    return active + 1;
}

/* ---- the fetch task -------------------------------------------------- */

static void on_last_page(uint64_t off, uint32_t serial, int64_t granule, uint8_t flags, void *ctx)
{
    if (granule > 0) *(int64_t *)ctx = granule;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Open `url` and read the headers. Returns the client (caller cleans up)
 * or NULL with `err` filled. */
static esp_http_client_handle_t open_url(const char *url, int64_t *length, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "downlink/0.1 (ESP32-S3)";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, errlen, "client init failed");
        return NULL;
    }
    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            snprintf(err, errlen, "connect failed: %s", esp_err_to_name(e));
            esp_http_client_cleanup(c);
            return NULL;
        }
        *length = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (!is_redirect(status)) break;
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    if (status != 200) {
        snprintf(err, errlen, "HTTP %d", status);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

/* Download job `j` into chunks; NULL on success, else the reason. */
static const char *fetch(job_t *j, const char *url, char *err, size_t errlen)
{
    int64_t length = 0;
    esp_http_client_handle_t c = NULL;

    /* Reserve budget for what is coming before downloading it. If it does
     * not fit yet, drop the connection and wait for clips ahead to play
     * out (a held-open connection would only time out). */
    for (;;) {
        c = open_url(url, &length, err, errlen);
        if (!c) return err;
        if (length > (int64_t)MAX_BYTES) {
            esp_http_client_cleanup(c);
            return "too large";
        }
        size_t want = length > 0 ? (size_t)length : MAX_BYTES;
        size_t reserve = (want + CHUNK - 1) / CHUNK * CHUNK;
        LOCK();
        bool fits = s_budget_used + reserve <= BUDGET_BYTES;
        if (fits) {
            s_budget_used += reserve;
            j->reserved = reserve;
        }
        UNLOCK();
        if (fits) break;
        esp_http_client_cleanup(c);
        ESP_LOGI(TAG, "%s waits for clip memory", j->id);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(100));
            LOCK();
            bool now = s_budget_used + reserve <= BUDGET_BYTES;
            UNLOCK();
            if (now) break;
        }
    }

    const char *fail = NULL;
    int64_t deadline = esp_timer_get_time() + FETCH_DEADLINE_US;
    size_t fill = CHUNK;   /* bytes in the last chunk; CHUNK means "start a new one" */
    for (;;) {
        if (esp_timer_get_time() > deadline) { fail = "timeout"; break; }
        if (length > 0 && (int64_t)j->len >= length) break;   /* all of it, even on a chunk boundary */
        if (fill == CHUNK) {
            if (j->len >= MAX_BYTES) { fail = "too large"; break; }
            if ((j->nchunks + 1) * CHUNK > j->reserved) { fail = "longer than its Content-Length"; break; }
            uint8_t *p = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
            if (!p) { fail = "no memory"; break; }
            j->chunks[j->nchunks++] = p;
            fill = 0;
        }
        int n = esp_http_client_read(c, (char *)j->chunks[j->nchunks - 1] + fill, (int)(CHUNK - fill));
        if (n > 0) {
            fill += n;
            j->len += n;
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        } else if (n == 0) {
            break;   /* end of body */
        } else {
            fail = "download failed";
            break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (fail) return fail;
    if (length > 0 && (int64_t)j->len != length) return "download incomplete";

    /* Ogg Opus, or nothing reaches the DAC. */
    uint8_t channels = 0;
    if (j->len == 0 || !opus_head_parse(j->chunks[0], j->len < CHUNK ? j->len : CHUNK, &j->preskip, &channels))
        return "not Ogg Opus";

    /* Duration: the last page's granule, less the pre-skip. */
    int64_t last = 0;
    ogg_sniff_t walk = {};
    walk.on_page = on_last_page;
    walk.ctx = &last;
    ogg_sniff_reset(&walk);
    for (size_t i = 0; i < j->nchunks; i++) {
        size_t n = i + 1 < j->nchunks ? CHUNK : j->len - i * CHUNK;
        ogg_sniff_feed(&walk, j->chunks[i], n, (uint64_t)i * CHUNK);
    }
    j->duration_ms = last > j->preskip ? (uint32_t)((last - j->preskip) / (RATE / 1000)) : 0;

    /* Hand back what the reservation over-estimated. */
    LOCK();
    size_t used = j->nchunks * CHUNK;
    s_budget_used -= j->reserved - used;
    j->reserved = used;
    UNLOCK();
    ESP_LOGI(TAG, "%s: %u bytes, %" PRIu32 " ms, %u ch", j->id, (unsigned)j->len, j->duration_ms, channels);
    return NULL;
}

static void fetch_task(void *arg)
{
    char url[URL_MAX], id[33], err[48];
    for (;;) {
        int slot;
        xQueueReceive(s_fetch_q, &slot, portMAX_DELAY);
        job_t *j = &s_jobs[slot];
        LOCK();
        j->state = J_FETCHING;
        strlcpy(url, j->url, sizeof url);
        strlcpy(id, j->id, sizeof id);
        UNLOCK();
        emit(id, ANN_DOWNLOADING, 0, NULL);

        err[0] = 0;
        const char *fail = fetch(j, url, err, sizeof err);
        if (!fail) {
            LOCK();
            j->state = J_READY;
            UNLOCK();
            continue;
        }
        ESP_LOGW(TAG, "%s failed: %s", id, fail);
        free_chunks(j);
        LOCK();
        s_budget_used -= j->reserved;
        j->reserved = 0;
        j->state = J_FREE;
        s_failed++;
        UNLOCK();
        emit(id, ANN_FAILED, 0, fail);
    }
}

/* ---- playback (player task only) -------------------------------------- */

#define BLOCK_MAX 480   /* the player's block */

static micro_opus::OggOpusDecoder *s_dec;
static duck_t      s_duck;
static gain_ramp_t s_gain;
static volatile bool s_sounding;
static uint32_t s_clip_avg_us;            /* clip decode per block, while sounding; 32-bit reads never tear */

static struct {
    int      slot;          /* the job playing, or -1 */
    bool     started;       /* past the pre-roll: the clip is sounding */
    size_t   chunk, off, consumed;
    int      stuck;
    int16_t *fifo;          /* the last decoded packet (PSRAM) */
    size_t   fifo_len, fifo_pos;
    uint64_t frames_out;
    uint64_t written;       /* frames written to the DAC before this block */
    int16_t  block[BLOCK_MAX * 2];
} P;

/* Events wait here until their sample has left the DAC. */
#define PENDING 6
static struct { uint64_t due; announce_event_t ev; } s_pend[PENDING];
static int s_npend;

static void schedule(uint64_t due, const char *id, announce_state_t state, uint32_t duration_ms, const char *error)
{
    if (s_npend == PENDING) {   /* cannot happen with a 4-deep queue; never lose one */
        xQueueSend(s_events, &s_pend[0].ev, 0);
        memmove(s_pend, s_pend + 1, sizeof s_pend[0] * (PENDING - 1));
        s_npend--;
    }
    announce_event_t &e = s_pend[s_npend].ev;
    e = {};
    strlcpy(e.id, id, sizeof e.id);
    e.state = state;
    e.duration_ms = duration_ms;
    if (error) strlcpy(e.error, error, sizeof e.error);
    s_pend[s_npend++].due = due;
}

void announce_written(uint64_t frames_written)
{
    P.written = frames_written;
    while (s_npend && s_pend[0].due <= frames_written) {
        if (xQueueSend(s_events, &s_pend[0].ev, 0) != pdTRUE) s_events_dropped++;
        memmove(s_pend, s_pend + 1, sizeof s_pend[0] * (s_npend - 1));
        s_npend--;
    }
}

/* Decode up to `want` frames of the playing clip into dst. Returns the
 * frame count; sets *ended when the clip has nothing more to give. */
static size_t clip_read(job_t *j, int16_t *dst, size_t want, bool *ended)
{
    size_t got = 0;
    while (got < want) {
        if (P.fifo_pos < P.fifo_len) {
            size_t n = P.fifo_len - P.fifo_pos;
            if (n > want - got) n = want - got;
            memcpy(dst + got * 2, P.fifo + P.fifo_pos * 2, n * 2 * sizeof(int16_t));
            P.fifo_pos += n;
            got += n;
            continue;
        }
        /* Past the last byte, keep calling with no input: the demuxer may
         * still hold packets of a page that crossed a chunk boundary. */
        bool drained = P.consumed >= j->len;
        size_t avail = drained ? 0 : CHUNK - P.off;
        if (avail > j->len - P.consumed) avail = j->len - P.consumed;
        const uint8_t *in = drained ? j->chunks[0] : j->chunks[P.chunk] + P.off;
        size_t used = 0, frames = 0;
        micro_opus::OggOpusResult r = s_dec->decode(in, avail,
                                                    (uint8_t *)P.fifo, OUT_FRAMES * 2 * sizeof(int16_t),
                                                    used, frames);
        P.consumed += used;
        P.off += used;
        if (P.off == CHUNK) { P.chunk++; P.off = 0; }
        if (r == micro_opus::OGG_OPUS_OK) {
            if (frames) {
                P.fifo_len = frames;
                P.fifo_pos = 0;
                P.stuck = 0;
            } else if (drained || (used == 0 && ++P.stuck > 3)) {
                *ended = true;   /* all played; or truncated, the rest never completes a page */
                break;
            }
        } else if (r == micro_opus::OGG_OPUS_DECODE_ERROR && used > 0) {
            continue;            /* one bad packet: skip it */
        } else {
            *ended = true;       /* the stream is broken from here: stop cleanly */
            break;
        }
    }
    return got;
}

static void finish_clip(job_t *j, size_t last_frames_this_block)
{
    bool ok = P.frames_out > 0;
    if (ok) {
        schedule(P.written + last_frames_this_block + AUDIO_OUT_DMA_FRAMES, j->id, ANN_DONE, 0, NULL);
        ESP_LOGI(TAG, "%s done: %.1f s", j->id, P.frames_out / (double)RATE);
    } else {
        schedule(P.written, j->id, ANN_FAILED, 0, "no audio");
        ESP_LOGW(TAG, "%s decoded no audio", j->id);
    }
    free_chunks(j);
    LOCK();
    if (ok) s_played++; else s_failed++;
    s_budget_used -= j->reserved;
    j->reserved = 0;
    j->state = J_FREE;
    int next = oldest_active(-1);
    bool another_ready = next >= 0 && s_jobs[next].state == J_READY;
    UNLOCK();
    duck_clip_ended(&s_duck, another_ready);
    P.slot = -1;
    s_playing_slot = -1;
    s_sounding = false;
}

const int16_t *announce_process(int16_t *music, size_t frames, gain_ramp_t **gain)
{
    *gain = &s_gain;
    if (frames > BLOCK_MAX) frames = BLOCK_MAX;

    /* Take the oldest job once it is ready; a later one never jumps it. */
    if (P.slot < 0) {
        LOCK();
        int next = oldest_active(-1);
        if (next >= 0 && s_jobs[next].state == J_READY) s_jobs[next].state = J_PLAYING;
        else next = -1;
        UNLOCK();
        if (next >= 0) {
            job_t *j = &s_jobs[next];
            P.slot = next;
            P.started = false;
            P.chunk = P.off = P.consumed = 0;
            P.stuck = 0;
            P.fifo_len = P.fifo_pos = 0;
            P.frames_out = 0;
            s_dec->reset();
            /* The clip's gain sits under the master volume: relative. */
            gain_ramp_set(&s_gain, mixer_volume_q15(j->volume), 0);
            duck_start(&s_duck, (float)j->duck);
            s_playing_slot = next;
            ESP_LOGI(TAG, "%s: ducking to %d dB", j->id, j->duck);
        }
    }

    const int16_t *clip = NULL;
    if (P.slot >= 0) {
        job_t *j = &s_jobs[P.slot];
        /* Pre-roll: the first word waits until the music is down. */
        if (!P.started && duck_ready(&s_duck)) {
            P.started = true;
            s_sounding = true;
            schedule(P.written + AUDIO_OUT_DMA_FRAMES, j->id, ANN_PLAYING, j->duration_ms, NULL);
        }
        if (P.started) {
            bool ended = false;
            int64_t t0 = esp_timer_get_time();
            size_t got = clip_read(j, P.block, frames, &ended);
            int32_t us = (int32_t)(esp_timer_get_time() - t0);
            s_clip_avg_us += (us - (int32_t)s_clip_avg_us) / 32;
            if (got < frames) memset(P.block + got * 2, 0, (frames - got) * 2 * sizeof(int16_t));
            P.frames_out += got;
            clip = P.block;
            if (ended) finish_clip(j, got);
        }
    }

    duck_block(&s_duck, frames);
    duck_apply(&s_duck, music, frames);
    return clip;
}

bool announce_sounding(void)
{
    return s_sounding;
}

uint32_t announce_clip_avg_us(void)
{
    return s_clip_avg_us;
}

/* ---- the rest -------------------------------------------------------- */

bool announce_wait_event(announce_event_t *e, TickType_t wait)
{
    return xQueueReceive(s_events, e, wait) == pdTRUE;
}

void announce_get_stats(announce_stats_t *out)
{
    *out = {};
    LOCK();
    out->played = s_played;
    out->failed = s_failed;
    int slot = s_playing_slot;
    if (slot >= 0 && s_jobs[slot].state == J_PLAYING) strlcpy(out->playing_id, s_jobs[slot].id, sizeof out->playing_id);
    for (int i = 0; i < JOBS; i++) if (s_jobs[i].state != J_FREE) out->queued++;
    UNLOCK();
}

const char *announce_state_name(announce_state_t s)
{
    switch (s) {
    case ANN_DOWNLOADING: return "downloading";
    case ANN_PLAYING:     return "playing";
    case ANN_DONE:        return "done";
    case ANN_FAILED:      return "failed";
    }
    return "?";
}

void announce_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_fetch_q = xQueueCreate(JOBS, sizeof(int));
    s_events = xQueueCreateWithCaps(16, sizeof(announce_event_t), MALLOC_CAP_SPIRAM);
    s_jobs = (job_t *)heap_caps_calloc(JOBS, sizeof(job_t), MALLOC_CAP_SPIRAM);
    P.slot = -1;
    P.fifo = (int16_t *)heap_caps_malloc((OUT_FRAMES + 1) * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    /* No Ogg CRC: the clip arrived whole over TCP, which checksums every
     * segment, and opus_head_parse already turned away anything that isn't
     * Ogg Opus. Stereo out: a mono clip is upmixed by the decoder. */
    s_dec = new micro_opus::OggOpusDecoder(false, RATE, 2);
    assert(s_lock && s_fetch_q && s_events && s_jobs && P.fifo && s_dec);
    duck_init(&s_duck, CONFIG_DL_DUCK_ATTACK_MS, CONFIG_DL_DUCK_TAIL_MS, CONFIG_DL_DUCK_RELEASE_MS, RATE);
    gain_ramp_init(&s_gain, 0);
    /* A 10 KB internal stack: esp_http_client crashes on a PSRAM one, and
     * an HTTPS handshake with certificate-bundle verification peaked at
     * 6.8 KB on it (stackFree.fetch). Core 0, beside the network. */
    xTaskCreatePinnedToCore(fetch_task, "fetch", 10240, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "ready: %d jobs, %u KB budget, %u KB per clip", JOBS,
             (unsigned)(BUDGET_BYTES / 1024), (unsigned)(MAX_BYTES / 1024));
}
