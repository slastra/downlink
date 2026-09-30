#include "stream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ogg_sniff.h"

static const char *TAG = "stream";

#define LINK_UP        BIT0
#define READ_CHUNK     4096
#define MAX_REDIRECTS  5
#define GOOD_RUN_US    (30LL * 1000 * 1000)   /* a connection this long resets the backoff */

static EventGroupHandle_t   s_ev;
static StreamBufferHandle_t s_ring;
static QueueHandle_t        s_bounds;
static char                 s_url[256];

/* Written only by the net task; the heartbeat's reads may tear, which only
 * costs a stats line. */
static uint64_t      s_written;
static uint64_t      s_last_bound = UINT64_MAX;
static volatile bool s_reconnect;
static volatile bool s_connected;
static uint32_t      s_connects, s_drops;
static ogg_sniff_t   s_sniff;
static char          s_title[160];

/* Response headers of the current attempt, filled by the event handler. */
static int  s_metaint;
static char s_ctype[48], s_icy_name[64], s_icy_br[8];

/* ICY demux state. */
enum { ICY_AUDIO, ICY_LEN, ICY_META };
static int    s_icy_state;
static size_t s_icy_left;
static char   s_meta[16 * 255 + 1];
static size_t s_meta_len;

/* ------------------------------------------------------------------ */

StreamBufferHandle_t stream_ring(void) { return s_ring; }

bool stream_boundary_peek(uint64_t *offset)
{
    return xQueuePeek(s_bounds, offset, 0) == pdTRUE;
}

void stream_boundary_pop(void)
{
    uint64_t discard;
    xQueueReceive(s_bounds, &discard, 0);
}

static void push_boundary(uint64_t off)
{
    if (off == s_last_bound) return;   /* connection start and its BOS page coincide */
    s_last_bound = off;
    if (xQueueSend(s_bounds, &off, 0) != pdTRUE)
        ESP_LOGW(TAG, "boundary queue full; decoder will resync the slow way");
}

void stream_reconnect(const char *why)
{
    ESP_LOGW(TAG, "reconnect requested: %s", why);
    s_reconnect = true;
}

void stream_set_link(bool up)
{
    if (!s_ev) return;
    if (up) xEventGroupSetBits(s_ev, LINK_UP);
    else    xEventGroupClearBits(s_ev, LINK_UP);
}

static bool link_up(void)
{
    return xEventGroupGetBits(s_ev) & LINK_UP;
}

/* ------------------------------------------------------------------ */
/* metadata                                                            */

static void set_title(const char *t, const char *source)
{
    if (strcmp(t, s_title) == 0) return;
    strlcpy(s_title, t, sizeof s_title);
    ESP_LOGI(TAG, "now playing (%s): %s", source, t[0] ? t : "(untitled)");
}

static void on_bos(uint64_t off, void *ctx)
{
    push_boundary(off);
}

static volatile bool s_wrong_codec;

static void on_codec(const uint8_t magic[8], void *ctx)
{
    if (memcmp(magic, "OpusHead", 8) == 0) return;
    bool vorbis = memcmp(magic, "\x01vorbis", 7) == 0;
    ESP_LOGE(TAG, "stream is %s, not Opus; this build decodes Ogg Opus only (set RUMP's codec to Opus)",
             vorbis ? "Ogg Vorbis" : "an unknown Ogg codec");
    s_wrong_codec = true;
    s_reconnect = true;
}

static void on_tags(const char *vendor, const char *title, const char *artist, void *ctx)
{
    char t[sizeof s_title];
    if (artist[0] && title[0]) snprintf(t, sizeof t, "%s - %s", artist, title);
    else strlcpy(t, title[0] ? title : artist, sizeof t);
    ESP_LOGI(TAG, "OpusTags: vendor=\"%s\"", vendor);
    set_title(t, "OpusTags");
}

/* StreamTitle='Artist - Song';StreamUrl='';  -- the value may contain
 * apostrophes, so the terminator is "';", not "'". */
static void parse_icy(const char *m)
{
    const char *p = strstr(m, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    char t[sizeof s_title];
    size_t n = (size_t)(e - p) < sizeof t - 1 ? (size_t)(e - p) : sizeof t - 1;
    memcpy(t, p, n);
    t[n] = 0;
    set_title(t, "ICY");
}

/* ------------------------------------------------------------------ */
/* ring                                                                */

/* Blocks while the ring is full (that is the backpressure that keeps a
 * faster-than-realtime connect burst from running away). Gives up if a
 * reconnect is requested or the link drops. */
static bool ring_write(uint8_t *p, size_t n)
{
    ogg_sniff_feed(&s_sniff, p, n, s_written);
    if (s_wrong_codec) return false;
    while (n) {
        size_t sent = xStreamBufferSend(s_ring, p, n, pdMS_TO_TICKS(500));
        s_written += sent;
        p += sent;
        n -= sent;
        if (n && (s_reconnect || !link_up())) return false;
    }
    return true;
}

static bool feed(uint8_t *buf, size_t n)
{
    if (!s_metaint) return ring_write(buf, n);
    for (size_t i = 0; i < n; ) {
        switch (s_icy_state) {
        case ICY_AUDIO: {
            size_t take = n - i < s_icy_left ? n - i : s_icy_left;
            if (!ring_write(buf + i, take)) return false;
            i += take;
            s_icy_left -= take;
            if (s_icy_left == 0) s_icy_state = ICY_LEN;
            break;
        }
        case ICY_LEN:
            s_icy_left = buf[i++] * 16;
            s_meta_len = 0;
            if (s_icy_left) {
                s_icy_state = ICY_META;
            } else {
                s_icy_state = ICY_AUDIO;
                s_icy_left = s_metaint;
            }
            break;
        case ICY_META: {
            size_t take = n - i < s_icy_left ? n - i : s_icy_left;
            memcpy(s_meta + s_meta_len, buf + i, take);
            s_meta_len += take;
            i += take;
            s_icy_left -= take;
            if (s_icy_left == 0) {
                s_meta[s_meta_len] = 0;
                parse_icy(s_meta);
                s_icy_state = ICY_AUDIO;
                s_icy_left = s_metaint;
            }
            break;
        }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    const char *k = e->header_key, *v = e->header_value;
    if      (!strcasecmp(k, "icy-metaint"))  s_metaint = atoi(v);
    else if (!strcasecmp(k, "content-type")) strlcpy(s_ctype, v, sizeof s_ctype);
    else if (!strcasecmp(k, "icy-name"))     strlcpy(s_icy_name, v, sizeof s_icy_name);
    else if (!strcasecmp(k, "icy-br"))       strlcpy(s_icy_br, v, sizeof s_icy_br);
    return ESP_OK;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* One connection, start to finish. Returns true if it streamed long enough
 * to count as healthy. */
static bool run_connection(uint8_t *buf)
{
    esp_http_client_config_t cfg = {
        .url = s_url,
        .event_handler = on_http,
        .timeout_ms = 2000,
        .buffer_size = READ_CHUNK,
        .buffer_size_tx = 1024,
        .user_agent = "downlink/0.1 (ESP32-S3)",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "http client init failed"); return false; }
    esp_http_client_set_header(c, "Icy-MetaData", "1");

    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        s_metaint = 0;
        s_ctype[0] = s_icy_name[0] = s_icy_br[0] = 0;
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(c);
            return false;
        }
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (!is_redirect(status)) break;
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
        ESP_LOGI(TAG, "HTTP %d, following redirect", status);
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d from %s", status, s_url);
        esp_http_client_cleanup(c);
        return false;
    }
    ESP_LOGI(TAG, "connected: type=\"%s\" name=\"%s\" br=%s metaint=%d",
             s_ctype, s_icy_name, s_icy_br[0] ? s_icy_br : "?", s_metaint);
    if (!strcasestr(s_ctype, "ogg") && !strcasestr(s_ctype, "opus")) {
        /* An MP3 mount would sync the Ogg walker on nothing and the
         * decoder on garbage; refuse it outright rather than hiss. */
        ESP_LOGE(TAG, "\"%s\" is not Ogg; this build decodes Ogg Opus only (set RUMP's codec to Opus)", s_ctype);
        esp_http_client_cleanup(c);
        return false;
    }

    s_connects++;
    s_connected = true;
    s_wrong_codec = false;
    ogg_sniff_reset(&s_sniff);
    push_boundary(s_written);
    s_icy_state = ICY_AUDIO;
    s_icy_left = s_metaint;

    int64_t t0 = esp_timer_get_time(), last_data = t0;
    for (;;) {
        if (s_reconnect) break;
        if (!link_up()) { ESP_LOGW(TAG, "link down, dropping the connection"); break; }
        int n = esp_http_client_read(c, (char *)buf, READ_CHUNK);
        int64_t now = esp_timer_get_time();
        if (n > 0) {
            last_data = now;
            if (!feed(buf, n)) break;
        } else if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (now - last_data > (int64_t)CONFIG_DL_STALL_S * 1000000) {
                ESP_LOGW(TAG, "no data for %d s", CONFIG_DL_STALL_S);
                break;
            }
        } else {
            ESP_LOGW(TAG, "connection closed by server (%d)", n);
            break;
        }
    }
    s_connected = false;
    s_drops++;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    int64_t ran = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "disconnected after %" PRId64 " s", ran / 1000000);
    return ran >= GOOD_RUN_US && !s_wrong_codec;
}

static void net_task(void *arg)
{
    uint8_t *buf = malloc(READ_CHUNK);
    assert(buf);
    uint32_t backoff_ms = 1000;
    for (;;) {
        xEventGroupWaitBits(s_ev, LINK_UP, pdFALSE, pdTRUE, portMAX_DELAY);
        s_reconnect = false;
        ESP_LOGI(TAG, "opening %s", s_url);
        if (run_connection(buf)) backoff_ms = 1000;
        if (s_wrong_codec) backoff_ms = CONFIG_DL_MAX_BACKOFF_S * 1000;
        ESP_LOGI(TAG, "retry in %" PRIu32 " ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > CONFIG_DL_MAX_BACKOFF_S * 1000) backoff_ms = CONFIG_DL_MAX_BACKOFF_S * 1000;
    }
}

void stream_start(const char *url)
{
    strlcpy(s_url, url, sizeof s_url);
    s_ev = xEventGroupCreate();
    s_bounds = xQueueCreate(16, sizeof(uint64_t));
    s_ring = xStreamBufferCreateWithCaps(CONFIG_DL_RING_KB * 1024, 1, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG, "no PSRAM for a %d KB ring", CONFIG_DL_RING_KB);
        abort();
    }
    s_sniff.on_bos = on_bos;
    s_sniff.on_tags = on_tags;
    s_sniff.on_codec = on_codec;
    ogg_sniff_reset(&s_sniff);
    /* Core 0 with the WiFi driver; decode owns core 1. */
    xTaskCreatePinnedToCore(net_task, "stream", 6144, NULL, 6, NULL, 0);
}

bool stream_connected(void)      { return s_connected; }
bool stream_codec_rejected(void) { return s_wrong_codec; }

void stream_status(char *out, size_t len)
{
    static uint64_t last_bytes;
    static int64_t  last_us;
    int64_t now = esp_timer_get_time();
    uint64_t w = s_written;
    unsigned kbps = last_us ? (unsigned)((w - last_bytes) * 8000 / (uint64_t)(now - last_us)) : 0;
    last_bytes = w;
    last_us = now;
    size_t fill = s_ring ? xStreamBufferBytesAvailable(s_ring) : 0;
    snprintf(out, len, "stream=%s in=%ukbps ring=%u/%uKB connects=%" PRIu32 " drops=%" PRIu32 " title=\"%s\"",
             s_connected ? "up" : "down", kbps, (unsigned)(fill / 1024), (unsigned)CONFIG_DL_RING_KB,
             s_connects, s_drops, s_title);
}
