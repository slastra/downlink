#include "player.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"

#include "audio_out.h"
#include "drift.h"
#include "ogg_sniff.h"
#include "stream.h"

static const char *TAG = "player";

#define IN_SIZE      4096
#define OUT_FRAMES   5760                       /* 120 ms at 48 kHz, the largest Opus frame */
#define OUT_BYTES    (OUT_FRAMES * 2 * sizeof(int16_t))
#define FRAMES_PER_MS 48
#define PREBUFFER     ((int64_t)CONFIG_DL_PREBUFFER_MS * FRAMES_PER_MS)
#define TARGET        ((int64_t)CONFIG_DL_BUFFER_MS * FRAMES_PER_MS)
#define CATCHUP_ABOVE ((int64_t)CONFIG_DL_BUFFER_MAX_MS * FRAMES_PER_MS)
/* Start regardless once the ring is half full: a stream whose pages carry
 * no usable granules must still play. */
#define START_BYTES   (CONFIG_DL_RING_KB * 1024 / 2)
#define SILENT_US     (10LL * 1000 * 1000)       /* data flowing, nothing decoded: reconnect */

#define BUFFERING PLAYER_BUFFERING
#define PLAYING   PLAYER_PLAYING
#define SKIPPING  PLAYER_SKIPPING
static const char *const STATE_NAMES[] = { "buffering", "playing", "skipping" };

static volatile player_state_t s_state = BUFFERING;
static volatile int32_t s_gain_q15;
static volatile int     s_volume;
static uint32_t s_underruns, s_resets, s_errors;
static uint64_t s_frames_played;
static drift_t  s_drift;
static volatile int32_t s_depth_ms = -1;

/* ---- where the decoder is, in stream time --------------------------
 *
 * The buffer is measured in audio, not bytes (drift.h says why). The
 * stream task reports the granule of the newest page written; here the
 * player walks the same pages it feeds the decoder and queues each audio
 * page's start offset and granule. Once the decoder has consumed past a
 * page's start, that page's granule is the play position. In the same
 * connection and logical stream, newest minus play position is the audio
 * buffered, to within a page (~100 ms), at any bitrate.
 */
struct page_mark { uint64_t off; uint32_t serial; int64_t granule; };
#define MARKS 256   /* the walker runs at most IN_SIZE bytes ahead: far fewer pages */
static page_mark s_marks[MARKS];
static uint32_t  s_mark_head, s_mark_tail;

static struct {
    uint32_t epoch, serial;
    int64_t  granule;
    bool     valid;
} s_pos;

static ogg_sniff_t s_walk;

static void on_walk_page(uint64_t off, uint32_t serial, int64_t granule, uint8_t flags, void *ctx)
{
    if (granule <= 0) return;   /* header page, or no packet ends here */
    if (s_mark_tail - s_mark_head == MARKS) s_mark_head++;
    s_marks[s_mark_tail++ % MARKS] = { off, serial, granule };
}

static void advance_pos(uint64_t consumed)
{
    while (s_mark_head != s_mark_tail && s_marks[s_mark_head % MARKS].off < consumed) {
        const page_mark &m = s_marks[s_mark_head++ % MARKS];
        s_pos.serial = m.serial;
        s_pos.granule = m.granule;
        s_pos.valid = true;
    }
}

/* Buffered audio, in frames, when both ends are in the same connection and
 * logical stream; unknown across a boundary still in the ring. */
static bool depth(int64_t *out)
{
    stream_pos_t w;
    stream_write_pos(&w);
    if (!w.valid || !s_pos.valid || w.epoch != s_pos.epoch || w.serial != s_pos.serial) return false;
    *out = w.granule > s_pos.granule ? w.granule - s_pos.granule : 0;
    return true;
}

/* For the prebuffer, with nothing played yet in this stream (boot, or a
 * new connection after an underrun) there is no play position; but the
 * ring was empty when buffering began, so everything written in this
 * stream so far is what is buffered. */
static bool depth_for_prebuffer(int64_t *out)
{
    if (depth(out)) return true;
    stream_pos_t w;
    stream_write_pos(&w);
    if (!w.valid) return false;
    *out = w.granule - w.first;
    return true;
}

static void apply_volume(int16_t *pcm, size_t n)
{
    int32_t g = s_gain_q15;
    if (g >= 32768) return;
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
}

static void player_task(void *arg)
{
    StreamBufferHandle_t ring = stream_ring();
    /* Stereo out regardless of the stream: a mono source is upmixed by the
     * decoder, so the I2S side never reconfigures slots. */
    auto *dec = new micro_opus::OggOpusDecoder(false, micro_opus::OPUS_DEFAULT_SAMPLE_RATE, 2);
    auto *in  = (uint8_t *)heap_caps_malloc(IN_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* One spare frame: drift correction may repeat one. */
    auto *out = (int16_t *)heap_caps_malloc(OUT_BYTES + 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(dec && in && out);

    size_t   in_len = 0, in_pos = 0;
    uint64_t in_base = 0;        /* stream offset of in[0] */
    uint64_t rd = 0;             /* stream offset of the next byte to receive */
    int64_t  last_audio_us = 0;
    int      stuck = 0;
    bool     announced = false;
    uint64_t walk_pos = 0;       /* stream offset the walker has seen up to */
    /* Reconnect overlap: Icecast's burst repeats audio already played. While
     * set, pages of `dedupe_serial` ending at or before `dedupe_until` are
     * decoded (the decoder needs them) and discarded. */
    bool     dedupe = false;
    uint32_t dedupe_serial = 0;
    int64_t  dedupe_until = 0;
    uint64_t deduped = 0;
    uint32_t ff_packets = 0;
    bool     was_catching_up = false;
    s_walk.on_page = on_walk_page;   /* renumber stays off: the stream task did it */
    ogg_sniff_reset(&s_walk);

    for (;;) {
        if (s_state == BUFFERING) {
            int64_t d = 0;
            bool known = depth_for_prebuffer(&d);
            size_t bytes = xStreamBufferBytesAvailable(ring) + (in_len - in_pos);
            if (!(known && d >= PREBUFFER) && bytes < START_BYTES) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            ESP_LOGI(TAG, "prebuffered %lld ms (%u KB), playing", known ? (long long)(d / FRAMES_PER_MS) : -1LL,
                     (unsigned)(bytes / 1024));
            s_state = PLAYING;
            drift_reset(&s_drift);   /* the depth starts over */
            last_audio_us = esp_timer_get_time();
        }

        if (in_pos == in_len) {
            size_t n = xStreamBufferReceive(ring, in, IN_SIZE, pdMS_TO_TICKS(100));
            if (n == 0) {
                if (s_state == PLAYING) {
                    s_underruns++;
                    ESP_LOGW(TAG, "underrun #%" PRIu32 ", rebuffering", s_underruns);
                    s_state = BUFFERING;
                    drift_reset(&s_drift);
                }
                continue;   /* SKIPPING stays put until its boundary arrives */
            }
            in_base = rd;
            rd += n;
            in_len = n;
            in_pos = 0;
        }

        /* Never feed the decoder across a boundary: stop short of it, and
         * reset when the read position lands on it. */
        uint64_t pos = in_base + in_pos;
        size_t avail = in_len - in_pos;
        stream_boundary_t b;
        while (stream_boundary_peek(&b) && b.offset < pos) stream_boundary_pop();
        if (stream_boundary_peek(&b)) {
            if (b.offset == pos) {
                stream_boundary_pop();
                dec->reset();
                s_resets++;
                announced = false;
                stuck = 0;
                if (s_state == SKIPPING) s_state = PLAYING;
                last_audio_us = esp_timer_get_time();
                /* The walker and the play position start over with the
                 * decoder; a new connection also means a new depth. */
                ogg_sniff_reset(&s_walk);
                walk_pos = pos;
                s_mark_head = s_mark_tail;
                bool was_valid = s_pos.valid;
                s_pos.valid = false;
                if (b.epoch != s_pos.epoch) {
                    /* Same logical stream on the new connection: whatever of
                     * its burst we already played gets dropped below. */
                    dedupe = was_valid;
                    dedupe_serial = s_pos.serial;
                    dedupe_until = s_pos.granule;
                    s_pos.epoch = b.epoch;
                    drift_reset(&s_drift);
                }
                ESP_LOGI(TAG, "new Ogg stream at offset %" PRIu64 " (connection %" PRIu32 ")", pos, b.epoch);
                continue;
            }
            if (b.offset < pos + avail) avail = (size_t)(b.offset - pos);
        }

        if (s_state == SKIPPING) {
            in_pos += avail;   /* discard up to the next boundary */
            continue;
        }

        /* The walker sees exactly the bytes the decoder is offered, once. */
        if (walk_pos < pos) walk_pos = pos;
        if (walk_pos < pos + avail) {
            ogg_sniff_feed(&s_walk, in + (walk_pos - in_base), (size_t)(pos + avail - walk_pos), walk_pos);
            walk_pos = pos + avail;
        }

        size_t used = 0, frames = 0;
        micro_opus::OggOpusResult r = dec->decode(in + in_pos, avail, (uint8_t *)out, OUT_BYTES, used, frames);
        in_pos += used;
        advance_pos(in_base + in_pos);
        int64_t now = esp_timer_get_time();

        if (r == micro_opus::OGG_OPUS_OK) {
            if (frames) {
                if (!announced) {
                    ESP_LOGI(TAG, "decoding Opus: %" PRIu32 " Hz, %u ch, pre-skip %u",
                             dec->get_sample_rate(), dec->get_channels(), dec->get_pre_skip());
                    audio_out_set_rate(dec->get_sample_rate());
                    announced = true;
                }
                last_audio_us = now;
                stuck = 0;
                int64_t d = 0;
                bool known = depth(&d);
                s_depth_ms = known ? (int32_t)(d / FRAMES_PER_MS) : -1;

                if (dedupe && s_pos.valid) {
                    if (s_pos.serial == dedupe_serial && s_pos.granule <= dedupe_until) {
                        deduped += frames;
                        continue;   /* already played on the last connection */
                    }
                    if (deduped) ESP_LOGI(TAG, "reconnect overlap: %.1f s already played, dropped", deduped / 48000.0);
                    dedupe = false;
                    deduped = 0;
                }

                bool catching_up = known ? drift_catching_up(&s_drift, d) : s_drift.catching_up;
                if (catching_up != was_catching_up) {
                    if (catching_up) ESP_LOGW(TAG, "%lld s buffered; catching up to %d s", (long long)(d / 48000),
                                              CONFIG_DL_BUFFER_MS / 1000);
                    else ESP_LOGI(TAG, "caught up (%.1f s skipped so far)", s_drift.skipped / 48000.0);
                    was_catching_up = catching_up;
                }
                if (catching_up) {
                    /* Decode and discard, faster than real time. Yield now and
                     * then: the idle task on this core feeds the watchdog. */
                    drift_skipped(&s_drift, frames);
                    if ((++ff_packets & 7) == 0) vTaskDelay(1);
                } else {
                    if (known) drift_update(&s_drift, d, frames);
                    frames = drift_apply(&s_drift, out, frames, OUT_FRAMES + 1);
                    apply_volume(out, frames * 2);
                    audio_out_write(out, frames);
                    s_frames_played += frames;
                }
            } else if (used == 0 && ++stuck > 3) {
                in_pos++;   /* the decoder made no progress on this byte; step over it */
                stuck = 0;
            }
        } else if (r == micro_opus::OGG_OPUS_DECODE_ERROR) {
            /* One bad packet: Opus conceals it, the stream carries on. */
            if (++s_errors % 50 == 1) ESP_LOGW(TAG, "decode error (%" PRIu32 " so far)", s_errors);
            if (used == 0) in_pos++;
        } else {
            /* Structural: lost sync, EOS without a following BOS, a header
             * out of place. The decoder only restarts on a BOS page, so skip
             * to the next boundary and ask for a fresh connection to make
             * one arrive soon. */
            s_errors++;
            ESP_LOGW(TAG, "stream invalid (%d); skipping to the next stream start", (int)r);
            s_state = SKIPPING;
            stream_reconnect("decoder lost the Ogg stream");
        }

        if (s_state == PLAYING && now - last_audio_us > SILENT_US) {
            ESP_LOGW(TAG, "no audio decoded for %lld s", SILENT_US / 1000000);
            last_audio_us = now;
            s_state = SKIPPING;
            stream_reconnect("no decodable audio");
        }
    }
}

extern "C" void player_set_volume(int volume_percent)
{
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    /* 0.5 dB per step below 100; 0 is mute. */
    s_gain_q15 = volume_percent == 0 ? 0
               : (int32_t)lroundf(32768.0f * powf(10.0f, (volume_percent - 100) * 0.5f / 20.0f));
    s_volume = volume_percent;
    ESP_LOGI(TAG, "volume %d%% (%.1f dB)", volume_percent, volume_percent ? (volume_percent - 100) * 0.5 : -INFINITY);
}

extern "C" void player_get_stats(player_stats_t *out)
{
    out->state = s_state;
    out->underruns = s_underruns;
    out->resets = s_resets;
    out->errors = s_errors;
    out->played_s = (uint32_t)(s_frames_played / micro_opus::OPUS_DEFAULT_SAMPLE_RATE);
    out->volume = s_volume;
    out->depth_ms = s_depth_ms;
    out->drift_mode = s_drift.catching_up ? 2 : s_drift.mode;
    out->drift_dropped = s_drift.dropped;
    out->drift_repeated = s_drift.repeated;
    out->catchups = s_drift.catchups;
    out->skipped_s = (uint32_t)(s_drift.skipped / micro_opus::OPUS_DEFAULT_SAMPLE_RATE);
}

extern "C" void player_start(int volume_percent)
{
    drift_init(&s_drift, TARGET, CATCHUP_ABOVE);
    player_set_volume(volume_percent);
    audio_out_init(micro_opus::OPUS_DEFAULT_SAMPLE_RATE);
    /* micro-opus keeps its working memory in a PSRAM pseudostack, so the
     * task stack itself stays small. */
    xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 7, NULL, 1);
}

extern "C" player_state_t player_state(void) { return s_state; }

extern "C" void player_status(char *out, size_t len)
{
    snprintf(out, len, "player=%s depth=%ldms underruns=%" PRIu32 " resets=%" PRIu32 " errors=%" PRIu32
             " drift=-%" PRIu32 "/+%" PRIu32 " catchups=%" PRIu32,
             STATE_NAMES[s_state], (long)s_depth_ms, s_underruns, s_resets, s_errors,
             s_drift.dropped, s_drift.repeated, s_drift.catchups);
}
