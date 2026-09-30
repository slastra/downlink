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
#include "stream.h"

static const char *TAG = "player";

#define IN_SIZE      4096
#define OUT_FRAMES   5760                       /* 120 ms at 48 kHz, the largest Opus frame */
#define OUT_BYTES    (OUT_FRAMES * 2 * sizeof(int16_t))
#define PREBUFFER    (CONFIG_DL_PREBUFFER_KB * 1024)
#define SILENT_US    (10LL * 1000 * 1000)       /* data flowing, nothing decoded: reconnect */

enum play_state { BUFFERING, PLAYING, SKIPPING };
static const char *const STATE_NAMES[] = { "buffering", "playing", "skipping" };

static volatile play_state s_state = BUFFERING;
static int32_t  s_gain_q15;
static uint32_t s_underruns, s_resets, s_errors;

static void apply_volume(int16_t *pcm, size_t n)
{
    if (s_gain_q15 >= 32768) return;
    for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((pcm[i] * s_gain_q15) >> 15);
}

static void player_task(void *arg)
{
    StreamBufferHandle_t ring = stream_ring();
    /* Stereo out regardless of the stream: a mono source is upmixed by the
     * decoder, so the I2S side never reconfigures slots. */
    auto *dec = new micro_opus::OggOpusDecoder(false, micro_opus::OPUS_DEFAULT_SAMPLE_RATE, 2);
    auto *in  = (uint8_t *)heap_caps_malloc(IN_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto *out = (int16_t *)heap_caps_malloc(OUT_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(dec && in && out);

    size_t   in_len = 0, in_pos = 0;
    uint64_t in_base = 0;        /* stream offset of in[0] */
    uint64_t rd = 0;             /* stream offset of the next byte to receive */
    int64_t  last_audio_us = 0;
    int      stuck = 0;
    bool     announced = false;

    for (;;) {
        if (s_state == BUFFERING) {
            if (xStreamBufferBytesAvailable(ring) + (in_len - in_pos) < PREBUFFER) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            ESP_LOGI(TAG, "prebuffered %u KB, playing", (unsigned)(xStreamBufferBytesAvailable(ring) / 1024));
            s_state = PLAYING;
            last_audio_us = esp_timer_get_time();
        }

        if (in_pos == in_len) {
            size_t n = xStreamBufferReceive(ring, in, IN_SIZE, pdMS_TO_TICKS(100));
            if (n == 0) {
                if (s_state == PLAYING) {
                    s_underruns++;
                    ESP_LOGW(TAG, "underrun #%" PRIu32 ", rebuffering", s_underruns);
                    s_state = BUFFERING;
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
        uint64_t pos = in_base + in_pos, b;
        size_t avail = in_len - in_pos;
        while (stream_boundary_peek(&b) && b < pos) stream_boundary_pop();
        if (stream_boundary_peek(&b)) {
            if (b == pos) {
                stream_boundary_pop();
                dec->reset();
                s_resets++;
                announced = false;
                stuck = 0;
                if (s_state == SKIPPING) s_state = PLAYING;
                last_audio_us = esp_timer_get_time();
                ESP_LOGI(TAG, "new Ogg stream at offset %" PRIu64, pos);
                continue;
            }
            if (b < pos + avail) avail = (size_t)(b - pos);
        }

        if (s_state == SKIPPING) {
            in_pos += avail;   /* discard up to the next boundary */
            continue;
        }

        size_t used = 0, frames = 0;
        micro_opus::OggOpusResult r = dec->decode(in + in_pos, avail, (uint8_t *)out, OUT_BYTES, used, frames);
        in_pos += used;
        int64_t now = esp_timer_get_time();

        if (r == micro_opus::OGG_OPUS_OK) {
            if (frames) {
                if (!announced) {
                    ESP_LOGI(TAG, "decoding Opus: %" PRIu32 " Hz, %u ch, pre-skip %u",
                             dec->get_sample_rate(), dec->get_channels(), dec->get_pre_skip());
                    audio_out_set_rate(dec->get_sample_rate());
                    announced = true;
                }
                apply_volume(out, frames * 2);
                audio_out_write(out, frames);
                last_audio_us = now;
                stuck = 0;
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

extern "C" void player_start(int volume_percent)
{
    /* 0.5 dB per step below 100; 0 is mute. */
    s_gain_q15 = volume_percent <= 0 ? 0
               : (int32_t)lroundf(32768.0f * powf(10.0f, (volume_percent - 100) * 0.5f / 20.0f));
    ESP_LOGI(TAG, "volume %d%% (%.1f dB)", volume_percent, volume_percent ? (volume_percent - 100) * 0.5 : -INFINITY);
    audio_out_init(micro_opus::OPUS_DEFAULT_SAMPLE_RATE);
    /* micro-opus keeps its working memory in a PSRAM pseudostack, so the
     * task stack itself stays small. */
    xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 7, NULL, 1);
}

extern "C" void player_status(char *out, size_t len)
{
    snprintf(out, len, "player=%s underruns=%" PRIu32 " resets=%" PRIu32 " errors=%" PRIu32,
             STATE_NAMES[s_state], s_underruns, s_resets, s_errors);
}
