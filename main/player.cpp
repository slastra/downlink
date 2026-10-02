#include "player.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"

#include "announce.h"
#include "audio_out.h"
#include "drift.h"
#include "mixer.h"
#include "ogg_sniff.h"
#include "stream.h"

static const char *TAG = "player";

#define RATE          48000
#define BLOCK         480                        /* frames per output block: 10 ms, one DMA descriptor */
#define IN_SIZE       4096
#define OUT_FRAMES    5760                       /* 120 ms at 48 kHz, the largest Opus frame */
#define OUT_BYTES     (OUT_FRAMES * 2 * sizeof(int16_t))
#define FRAMES_PER_MS 48
#define PREBUFFER     ((int64_t)CONFIG_DL_PREBUFFER_MS * FRAMES_PER_MS)
#define TARGET        ((int64_t)CONFIG_DL_BUFFER_MS * FRAMES_PER_MS)
#define CATCHUP_ABOVE ((int64_t)CONFIG_DL_BUFFER_MAX_MS * FRAMES_PER_MS)
/* Start regardless once the ring is half full: a stream whose pages carry
 * no usable granules must still play. */
#define START_BYTES   (CONFIG_DL_RING_KB * 1024 / 2)
#define SILENT_US     (10LL * 1000 * 1000)       /* data flowing, nothing decoded: reconnect */
#define STARVED_BLOCKS 10                        /* 100 ms with nothing to play is an underrun */
#define VOLUME_RAMP   (50 * FRAMES_PER_MS)       /* a volume change glides over 50 ms */
/* Decode-and-discard (catch-up, reconnect dedupe) may use this much of each
 * 10 ms block; the rest is the output path's -- less while an announcement
 * is decoding alongside. */
#define DISCARD_BUDGET_US          8000
#define DISCARD_BUDGET_ANNOUNCE_US 5000
#define STATS_WINDOW_US   (30LL * 1000 * 1000)

#define BUFFERING PLAYER_BUFFERING
#define PLAYING   PLAYER_PLAYING
#define SKIPPING  PLAYER_SKIPPING
static const char *const STATE_NAMES[] = { "buffering", "playing", "skipping" };

static volatile player_state_t s_state = BUFFERING;
static volatile int32_t  s_volume_q15;           /* set by player_set_volume, picked up per block */
static volatile int      s_volume;
static volatile bool     s_hold;                 /* player_set_hold: faded out */
static uint32_t s_underruns, s_resets, s_errors;
/* Counters other tasks read are 32-bit, so a read never tears. */
static volatile uint32_t s_played_s;             /* music seconds that reached the DAC */
static uint32_t          s_played_rem;
static volatile uint32_t s_progress;             /* frames played or deliberately discarded */
static volatile uint32_t s_block_max_us, s_block_avg_us;
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

/* ---- the music source ----------------------------------------------
 *
 * Pull model: the output loop asks for a block and gets what is ready, the
 * rest it pads with silence. All the stream logic -- boundaries, depth,
 * reconnect dedupe, catch-up, drift -- lives behind music_read(), so the
 * output keeps its 10 ms rhythm whatever the stream is doing.
 */
static struct {
    micro_opus::OggOpusDecoder *dec;
    StreamBufferHandle_t ring;
    uint8_t *in;
    size_t   in_len, in_pos;
    uint64_t in_base;            /* stream offset of in[0] */
    uint64_t rd;                 /* stream offset of the next byte to receive */
    uint64_t walk_pos;           /* stream offset the walker has seen up to */
    int16_t *fifo;               /* the last decoded packet, after drift correction */
    size_t   fifo_len, fifo_pos;
    int64_t  last_audio_us;
    int      stuck;
    int      starved_blocks;
    bool     announced;
    /* Reconnect overlap: Icecast's burst repeats audio already played. While
     * set, pages of `dedupe_serial` ending at or before `dedupe_until` are
     * decoded (the decoder needs them) and discarded. */
    bool     dedupe;
    uint32_t dedupe_serial;
    int64_t  dedupe_until;
    uint64_t deduped;
    bool     was_catching_up;
    float    us_per_frame;       /* decode cost, for budgeting discards */
    size_t   last_packet;        /* frames in the last decoded packet */
} m;

enum step_t { STEP_PCM, STEP_DISCARDED, STEP_MOVED, STEP_IDLE };

static void count_progress(size_t frames)
{
    s_progress += (uint32_t)frames;
}

/* One step of the old decode loop: receive, honour a boundary, skip, or
 * decode one packet into the FIFO (or discard it). */
static step_t decode_step(void)
{
    if (m.in_pos == m.in_len) {
        size_t n = xStreamBufferReceive(m.ring, m.in, IN_SIZE, 0);
        if (n == 0) return STEP_IDLE;
        m.in_base = m.rd;
        m.rd += n;
        m.in_len = n;
        m.in_pos = 0;
    }

    /* Never feed the decoder across a boundary: stop short of it, and
     * reset when the read position lands on it. */
    uint64_t pos = m.in_base + m.in_pos;
    size_t avail = m.in_len - m.in_pos;
    stream_boundary_t b;
    while (stream_boundary_peek(&b) && b.offset < pos) stream_boundary_pop();
    if (stream_boundary_peek(&b)) {
        if (b.offset == pos) {
            stream_boundary_pop();
            m.dec->reset();
            s_resets++;
            m.announced = false;
            m.stuck = 0;
            if (s_state == SKIPPING) s_state = PLAYING;
            m.last_audio_us = esp_timer_get_time();
            /* The walker and the play position start over with the
             * decoder; a new connection also means a new depth. */
            ogg_sniff_reset(&s_walk);
            m.walk_pos = pos;
            s_mark_head = s_mark_tail;
            bool was_valid = s_pos.valid;
            s_pos.valid = false;
            if (b.epoch != s_pos.epoch) {
                /* Same logical stream on the new connection: whatever of
                 * its burst we already played gets dropped below. */
                m.dedupe = was_valid;
                m.dedupe_serial = s_pos.serial;
                m.dedupe_until = s_pos.granule;
                s_pos.epoch = b.epoch;
                drift_reset(&s_drift);
            }
            ESP_LOGI(TAG, "new Ogg stream at offset %" PRIu64 " (connection %" PRIu32 ")", pos, b.epoch);
            return STEP_MOVED;
        }
        if (b.offset < pos + avail) avail = (size_t)(b.offset - pos);
    }

    if (s_state == SKIPPING) {
        m.in_pos += avail;   /* discard up to the next boundary */
        return STEP_MOVED;
    }

    /* The walker sees exactly the bytes the decoder is offered, once. */
    if (m.walk_pos < pos) m.walk_pos = pos;
    if (m.walk_pos < pos + avail) {
        ogg_sniff_feed(&s_walk, m.in + (m.walk_pos - m.in_base), (size_t)(pos + avail - m.walk_pos), m.walk_pos);
        m.walk_pos = pos + avail;
    }

    size_t used = 0, frames = 0;
    int64_t t0 = esp_timer_get_time();
    micro_opus::OggOpusResult r = m.dec->decode(m.in + m.in_pos, avail, (uint8_t *)m.fifo, OUT_BYTES, used, frames);
    int64_t now = esp_timer_get_time();
    m.in_pos += used;
    advance_pos(m.in_base + m.in_pos);

    if (r == micro_opus::OGG_OPUS_OK) {
        if (frames == 0) {
            if (used == 0 && ++m.stuck > 3) {
                m.in_pos++;   /* the decoder made no progress on this byte; step over it */
                m.stuck = 0;
            }
            return STEP_MOVED;
        }
        if (!m.announced) {
            ESP_LOGI(TAG, "decoding Opus: %" PRIu32 " Hz, %u ch, pre-skip %u",
                     m.dec->get_sample_rate(), m.dec->get_channels(), m.dec->get_pre_skip());
            m.announced = true;
        }
        m.us_per_frame += ((float)(now - t0) / frames - m.us_per_frame) / 16;
        m.last_packet = frames;
        m.last_audio_us = now;
        m.stuck = 0;
        int64_t d = 0;
        bool known = depth(&d);
        s_depth_ms = known ? (int32_t)(d / FRAMES_PER_MS) : -1;

        if (m.dedupe && s_pos.valid) {
            if (s_pos.serial == m.dedupe_serial && s_pos.granule <= m.dedupe_until) {
                m.deduped += frames;
                count_progress(frames);
                return STEP_DISCARDED;   /* already played on the last connection */
            }
            if (m.deduped) ESP_LOGI(TAG, "reconnect overlap: %.1f s already played, dropped", m.deduped / 48000.0);
            m.dedupe = false;
            m.deduped = 0;
        }

        bool catching_up = known ? drift_catching_up(&s_drift, d) : s_drift.catching_up;
        if (catching_up != m.was_catching_up) {
            if (catching_up) ESP_LOGW(TAG, "%lld s buffered; catching up to %d s", (long long)(d / RATE),
                                      CONFIG_DL_BUFFER_MS / 1000);
            else ESP_LOGI(TAG, "caught up (%.1f s skipped so far)", s_drift.skipped / 48000.0);
            m.was_catching_up = catching_up;
        }
        if (catching_up) {
            drift_skipped(&s_drift, frames);
            count_progress(frames);
            return STEP_DISCARDED;
        }
        if (known) drift_update(&s_drift, d, frames);
        m.fifo_len = drift_apply(&s_drift, m.fifo, frames, OUT_FRAMES + 1);
        m.fifo_pos = 0;
        return STEP_PCM;
    }
    if (r == micro_opus::OGG_OPUS_DECODE_ERROR) {
        /* One bad packet: Opus conceals it, the stream carries on. */
        if (++s_errors % 50 == 1) ESP_LOGW(TAG, "decode error (%" PRIu32 " so far)", s_errors);
        if (used == 0) m.in_pos++;
        return STEP_MOVED;
    }
    /* Structural: lost sync, EOS without a following BOS, a header out of
     * place. The decoder only restarts on a BOS page, so skip to the next
     * boundary and ask for a fresh connection to make one arrive soon. */
    s_errors++;
    ESP_LOGW(TAG, "stream invalid (%d); skipping to the next stream start", (int)r);
    s_state = SKIPPING;
    stream_reconnect("decoder lost the Ogg stream");
    return STEP_MOVED;
}

/*
 * Fill up to `want` frames of `dst`; returns how many. Discarding (catch-up
 * or dedupe) stops once `budget_us` of this call is spent, and not at all
 * when the output is `behind`, so it can never starve the DAC.
 */
static size_t music_read(int16_t *dst, size_t want, int64_t budget_us, bool behind)
{
    int64_t t0 = esp_timer_get_time();

    if (s_state == BUFFERING) {
        int64_t d = 0;
        bool known = depth_for_prebuffer(&d);
        size_t bytes = xStreamBufferBytesAvailable(m.ring) + (m.in_len - m.in_pos);
        if (!(known && d >= PREBUFFER) && bytes < START_BYTES) return 0;
        ESP_LOGI(TAG, "prebuffered %lld ms (%u KB), playing", known ? (long long)(d / FRAMES_PER_MS) : -1LL,
                 (unsigned)(bytes / 1024));
        s_state = PLAYING;
        drift_reset(&s_drift);   /* the depth starts over */
        m.last_audio_us = t0;
        m.starved_blocks = 0;
    }

    size_t got = 0;
    bool starved = false;
    for (int steps = 0; got < want && steps < 64; steps++) {
        if (m.fifo_pos < m.fifo_len) {
            size_t n = m.fifo_len - m.fifo_pos;
            if (n > want - got) n = want - got;
            memcpy(dst + got * 2, m.fifo + m.fifo_pos * 2, n * 2 * sizeof(int16_t));
            m.fifo_pos += n;
            got += n;
            continue;
        }
        if (m.dedupe || s_drift.catching_up) {
            if (behind) break;
            float est = m.us_per_frame * (m.last_packet ? m.last_packet : 960);
            if (esp_timer_get_time() - t0 + (int64_t)est > budget_us) break;
        }
        if (decode_step() == STEP_IDLE) {
            starved = true;
            break;
        }
    }

    if (s_state == PLAYING) {
        int64_t now = esp_timer_get_time();
        if (got < want && starved) {
            if (++m.starved_blocks == STARVED_BLOCKS) {
                s_underruns++;
                ESP_LOGW(TAG, "underrun #%" PRIu32 ", rebuffering", s_underruns);
                s_state = BUFFERING;
                drift_reset(&s_drift);
            }
        } else {
            m.starved_blocks = 0;
        }
        if (s_state == PLAYING && now - m.last_audio_us > SILENT_US) {
            ESP_LOGW(TAG, "no audio decoded for %lld s", SILENT_US / 1000000);
            m.last_audio_us = now;
            s_state = SKIPPING;
            stream_reconnect("no decodable audio");
        }
    }
    return got;
}

/* ---- the output loop -----------------------------------------------
 *
 * One block every 10 ms; the blocking I2S write is the clock. Sources hand
 * over what they have and the rest is silence, so nothing upstream -- a
 * rebuffer, a reconnect, a catch-up -- can stall the output.
 */
static void player_task(void *arg)
{
    static int16_t block[BLOCK * 2];
    gain_ramp_t master;
    gain_ramp_init(&master, s_volume_q15);
    int32_t master_set = s_volume_q15;

    bool behind = false;
    int64_t window_start = esp_timer_get_time();
    uint32_t window_max = 0;
    uint64_t written = 0;

    for (;;) {
        int64_t t0 = esp_timer_get_time();

        size_t got = music_read(block, BLOCK,
                                announce_sounding() ? DISCARD_BUDGET_ANNOUNCE_US : DISCARD_BUDGET_US, behind);
        if (got < BLOCK) memset(block + got * 2, 0, (BLOCK - got) * 2 * sizeof(int16_t));

        /* Duck the music under an announcement and get the clip's block,
         * then one mix: master * (music + clip), clamped once. */
        gain_ramp_t *ann_gain;
        const int16_t *clip = announce_process(block, BLOCK, &ann_gain);

        int32_t v = s_hold ? 0 : s_volume_q15;
        if (v != master_set) {
            bool fade = !v || !master_set;   /* into or out of a hold */
            gain_ramp_set(&master, v, fade ? PLAYER_HOLD_FADE_MS * FRAMES_PER_MS : VOLUME_RAMP);
            master_set = v;
        }
        mix_block(block, clip, BLOCK, ann_gain, &master);

        int64_t t1 = esp_timer_get_time();
        audio_out_write(block, BLOCK);
        int64_t t2 = esp_timer_get_time();
        written += BLOCK;
        announce_written(written);

        /* A write that did not wait means the DMA had room to spare: the
         * loop is running late, so the next block does no discarding. */
        behind = t2 - t1 < 2000;

        if (got) {
            s_played_rem += got;
            if (s_played_rem >= RATE) {
                s_played_s += s_played_rem / RATE;
                s_played_rem %= RATE;
            }
            count_progress(got);
        }

        uint32_t us = (uint32_t)(t1 - t0);
        if (us > window_max) window_max = us;
        s_block_avg_us += ((int32_t)us - (int32_t)s_block_avg_us) / 64;
        if (t2 - window_start >= STATS_WINDOW_US) {
            s_block_max_us = window_max;
            window_max = 0;
            window_start = t2;
        }
    }
}

extern "C" void player_set_hold(bool hold)
{
    if (hold != s_hold) ESP_LOGI(TAG, "%s", hold ? "holding: fading out" : "resuming: fading in");
    s_hold = hold;
}

extern "C" void player_set_volume(int volume_percent)
{
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    s_volume_q15 = mixer_volume_q15(volume_percent);
    s_volume = volume_percent;
    ESP_LOGI(TAG, "volume %d%% (%.1f dB)", volume_percent, volume_percent ? (volume_percent - 100) * 0.5 : -INFINITY);
}

extern "C" void player_get_stats(player_stats_t *out)
{
    out->state = s_state;
    out->underruns = s_underruns;
    out->resets = s_resets;
    out->errors = s_errors;
    out->played_s = s_played_s;
    out->progress = s_progress;
    out->volume = s_volume;
    out->depth_ms = s_depth_ms;
    out->drift_mode = s_drift.catching_up ? 2 : s_drift.mode;
    out->drift_dropped = s_drift.dropped;
    out->drift_repeated = s_drift.repeated;
    out->catchups = s_drift.catchups;
    out->skipped_s = (uint32_t)(s_drift.skipped / RATE);
    out->block_max_us = s_block_max_us;
    out->block_avg_us = s_block_avg_us;
}

extern "C" void player_start(int volume_percent)
{
    drift_init(&s_drift, TARGET, CATCHUP_ABOVE);
    player_set_volume(volume_percent);
    audio_out_init(RATE);

    m.ring = stream_ring();
    /* Stereo out regardless of the stream: a mono source is upmixed by the
     * decoder, so the I2S side never reconfigures slots. */
    m.dec  = new micro_opus::OggOpusDecoder(false, RATE, 2);
    m.in   = (uint8_t *)heap_caps_malloc(IN_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* One spare frame: drift correction may repeat one. */
    m.fifo = (int16_t *)heap_caps_malloc(OUT_BYTES + 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(m.dec && m.in && m.fifo);
    m.us_per_frame = 3.0f;
    s_walk.on_page = on_walk_page;   /* renumber stays off: the stream task did it */
    ogg_sniff_reset(&s_walk);

    /* micro-opus keeps its working memory in a PSRAM pseudostack, so the
     * task stack itself stays small. */
    xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 7, NULL, 1);
}

extern "C" player_state_t player_state(void) { return s_state; }

extern "C" void player_status(char *out, size_t len)
{
    snprintf(out, len, "player=%s depth=%ldms underruns=%" PRIu32 " resets=%" PRIu32 " errors=%" PRIu32
             " drift=-%" PRIu32 "/+%" PRIu32 " catchups=%" PRIu32 " block=%" PRIu32 "/%" PRIu32 "us",
             STATE_NAMES[s_state], (long)s_depth_ms, s_underruns, s_resets, s_errors,
             s_drift.dropped, s_drift.repeated, s_drift.catchups, s_block_avg_us, s_block_max_us);
}
