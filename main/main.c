/*
 * downlink: an Icecast Ogg Opus player for the ESP32-S3 into a PCM5102A.
 *
 *   core 0: WiFi (netlink), HTTP stream task -> 512 KB ring in PSRAM
 *   core 1: player task, ring -> OggOpusDecoder -> drift correction
 *           -> volume -> I2S
 *
 * WiFi and the captive portal come from tspl-station unchanged in
 * behaviour: an NVS credential table seeded from menuconfig, and an open
 * AP "downlink-<id>" when no known network is seen for 90 s or BOOT is held
 * for 3 s. The portal also takes the stream URL.
 *
 * Remotely, the board reports health on MQTT (retained status every 30 s
 * and on every state change) and takes commands: status, url, volume,
 * reboot. See README "Remote management". A supervisor here reboots it,
 * with the reason recorded, if it wedges.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "led.h"
#include "netlink.h"
#include "player.h"
#include "provision.h"
#include "settings.h"
#include "stream.h"
#include "uplink.h"

static const char *TAG = "main";

#define BOOT_GPIO       0
#define HEARTBEAT_S     30
#define TICK_MS         250

/* Supervisor limits: what "wedged" means for a board nobody is watching. */
#define STALL_REBOOT_S  60              /* data waiting, nothing played */
#define LOW_HEAP_BYTES  (16 * 1024)     /* internal RAM */
#define LOW_HEAP_S      10

/* Why we rebooted ourselves, kept across the software reset in RTC memory
 * (not cleared by it, unlike .bss) and reported in the next status. */
#define REBOOT_MAGIC 0x444c5242u        /* "DLRB" */
static RTC_NOINIT_ATTR uint32_t s_reboot_magic;
static RTC_NOINIT_ATTR char     s_reboot_why[32];
static char s_last_reboot[32];          /* this boot's cause, if we caused it */

/* The first state that applies wins; the LED only changes on a transition. */
enum { ST_PORTAL_CLIENT, ST_PORTAL, ST_NO_WIFI, ST_WRONG_CODEC, ST_CONNECTING, ST_BUFFERING, ST_PLAYING };

static int current_status(void)
{
    if (provision_active())      return provision_clients() > 0 ? ST_PORTAL_CLIENT : ST_PORTAL;
    if (!netlink_is_up())        return ST_NO_WIFI;
    if (stream_codec_rejected()) return ST_WRONG_CODEC;
    if (!stream_connected())     return ST_CONNECTING;
    return player_state() == PLAYER_PLAYING ? ST_PLAYING : ST_BUFFERING;
}

static int update_led(void)
{
    static int shown = -1;
    int st = current_status();
    if (st == shown) return st;
    shown = st;
    switch (st) {
    case ST_PORTAL_CLIENT: LED_PROVISION_CLIENT(); break;
    case ST_PORTAL:        LED_PROVISION();        break;
    case ST_NO_WIFI:       LED_NO_WIFI();          break;
    case ST_WRONG_CODEC:   LED_WRONG_CODEC();      break;
    case ST_CONNECTING:    LED_CONNECTING();       break;
    case ST_BUFFERING:     LED_BUFFERING();        break;
    case ST_PLAYING:       LED_PLAYING();          break;
    }
    return st;
}

static const char *const STATUS_NAMES[] = {
    [ST_PORTAL_CLIENT] = "portal_client", [ST_PORTAL] = "portal", [ST_NO_WIFI] = "no_wifi",
    [ST_WRONG_CODEC] = "wrong_codec", [ST_CONNECTING] = "connecting",
    [ST_BUFFERING] = "buffering", [ST_PLAYING] = "playing",
};

/* ---- remote health / commands ------------------------------------- */

static volatile bool s_publish_now;

static void publish_health(int st)
{
    stream_stats_t ss;
    player_stats_t ps;
    netlink_info_t ni;
    stream_get_stats(&ss);
    player_get_stats(&ps);
    netlink_get_info(&ni);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", STATUS_NAMES[st]);
    cJSON_AddStringToObject(o, "title", ss.title);
    cJSON_AddStringToObject(o, "url", ss.url);
    cJSON_AddStringToObject(o, "board", CONFIG_DL_BOARD);
    if (s_last_reboot[0]) cJSON_AddStringToObject(o, "rebootCause", s_last_reboot);

    cJSON *j = cJSON_AddObjectToObject(o, "stream");
    cJSON_AddBoolToObject(j, "connected", ss.connected);
    cJSON_AddNumberToObject(j, "kbps", ss.kbps);
    cJSON_AddNumberToObject(j, "ringKB", (double)(ss.ring_used / 1024));
    cJSON_AddNumberToObject(j, "ringPct", (double)(ss.ring_used * 100 / ss.ring_size));
    cJSON_AddNumberToObject(j, "connects", ss.connects);
    cJSON_AddNumberToObject(j, "drops", ss.drops);
    cJSON_AddNumberToObject(j, "http", ss.last_http);
    if (ss.last_error[0]) cJSON_AddStringToObject(j, "lastError", ss.last_error);

    j = cJSON_AddObjectToObject(o, "player");
    cJSON_AddNumberToObject(j, "underruns", ps.underruns);
    cJSON_AddNumberToObject(j, "resets", ps.resets);
    cJSON_AddNumberToObject(j, "errors", ps.errors);
    cJSON_AddNumberToObject(j, "playedS", ps.played_s);
    cJSON_AddNumberToObject(j, "volume", ps.volume);
    if (ps.depth_ms >= 0) cJSON_AddNumberToObject(j, "bufferMs", ps.depth_ms);
    cJSON *aj = cJSON_AddObjectToObject(o, "audio");
    cJSON_AddNumberToObject(aj, "blockMaxUs", ps.block_max_us);
    cJSON_AddNumberToObject(aj, "blockAvgUs", ps.block_avg_us);
    cJSON *dj = cJSON_AddObjectToObject(j, "drift");
    cJSON_AddStringToObject(dj, "mode", ps.drift_mode == 2 ? "catching_up" : ps.drift_mode > 0 ? "dropping"
                                        : ps.drift_mode < 0 ? "repeating" : "idle");
    cJSON_AddNumberToObject(dj, "targetMs", CONFIG_DL_BUFFER_MS);
    cJSON_AddNumberToObject(dj, "dropped", ps.drift_dropped);
    cJSON_AddNumberToObject(dj, "repeated", ps.drift_repeated);
    cJSON_AddNumberToObject(dj, "catchups", ps.catchups);
    cJSON_AddNumberToObject(dj, "skippedS", ps.skipped_s);

    /* Stack headroom (bytes never touched) per task: a number that trends
     * toward zero is a crash that has not happened yet. */
    j = cJSON_AddObjectToObject(o, "stackFree");
    static const char *const TASKS[] = { "main", "stream", "player", "mqtt_pub", "mqtt_task", "wifi_join" };
    for (size_t i = 0; i < sizeof TASKS / sizeof *TASKS; i++) {
        TaskHandle_t h = xTaskGetHandle(TASKS[i]);
        if (h) cJSON_AddNumberToObject(j, TASKS[i], uxTaskGetStackHighWaterMark(h));
    }

    j = cJSON_AddObjectToObject(o, "wifi");
    if (ni.ssid[0]) {
        cJSON_AddStringToObject(j, "ssid", ni.ssid);
        cJSON_AddNumberToObject(j, "rssi", ni.rssi);
        cJSON_AddStringToObject(j, "ip", ni.ip);
    }
    cJSON_AddNumberToObject(j, "drops", ni.drops);
    cJSON_AddNumberToObject(j, "roams", ni.roams);

    uplink_publish_status(o);
}

static void reboot_for(const char *why);

/* On the esp-mqtt event task: quick work only. */
static void on_cmd(cJSON *root)
{
    const cJSON *c = cJSON_GetObjectItem(root, "cmd");
    const char *cmd = cJSON_IsString(c) ? c->valuestring : "";

    if (!strcmp(cmd, "status")) {
        s_publish_now = true;
        uplink_publish_cmd_result(cmd, true, NULL);
    } else if (!strcmp(cmd, "url")) {
        const cJSON *u = cJSON_GetObjectItem(root, "url");
        const char *url = cJSON_IsString(u) ? u->valuestring : "";
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
            uplink_publish_cmd_result(cmd, false, "url must start with http:// or https://");
        } else if (!settings_set_url(url)) {
            uplink_publish_cmd_result(cmd, false, "could not save (too long?)");
        } else {
            stream_set_url(url);
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "url", url);
            uplink_publish_cmd_result_json(cmd, true, d);
            s_publish_now = true;
        }
    } else if (!strcmp(cmd, "volume")) {
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        int vol = cJSON_IsNumber(v) ? (int)v->valuedouble : -1;
        if (vol < 0 || vol > 100) {
            uplink_publish_cmd_result(cmd, false, "value must be 0-100");
        } else {
            player_set_volume(vol);
            settings_set_volume(vol);
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "volume", vol);
            uplink_publish_cmd_result_json(cmd, true, d);
            s_publish_now = true;
        }
    } else if (!strcmp(cmd, "reboot")) {
        uplink_publish_cmd_result(cmd, true, NULL);
        reboot_for("mqtt command");
    } else {
        uplink_publish_cmd_result(cmd, false, "unknown command; try status, url, volume, reboot");
    }
    cJSON_Delete(root);
}

static void on_mqtt_up(void) { s_publish_now = true; }

static void on_net_up(void)   { stream_set_link(true); }
static void on_net_down(void) { stream_set_link(false); }

static void on_portal_save(const char *id, const char *url)
{
    if (id)  settings_set_id(id);
    if (url) settings_set_url(url);
}

/* Give the "Saved" page (or the MQTT reply) time to leave before the
 * reboot. */
static void restart_cb(void *arg) { esp_restart(); }
static void reboot_for(const char *why)
{
    ESP_LOGW(TAG, "rebooting: %s", why);
    strlcpy(s_reboot_why, why, sizeof s_reboot_why);
    s_reboot_magic = REBOOT_MAGIC;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "reboot" };
    if (!t && esp_timer_create(&ta, &t) != ESP_OK) esp_restart();
    esp_timer_start_once(t, 1500 * 1000);
}

static void reboot_after_portal_save(void) { reboot_for("portal save"); }

static void take_reboot_cause(void)
{
    if (s_reboot_magic == REBOOT_MAGIC && esp_reset_reason() == ESP_RST_SW) {
        s_reboot_why[sizeof s_reboot_why - 1] = 0;
        strlcpy(s_last_reboot, s_reboot_why, sizeof s_last_reboot);
        ESP_LOGW(TAG, "last reboot was ours: %s", s_last_reboot);
    }
    s_reboot_magic = 0;
}

/*
 * Last line of defence for a board out of reach: reboot when it is wedged
 * in a way nothing else recovers from. Not for a stream that is simply
 * offline -- the stream task already retries that forever.
 */
static void supervise(void)
{
    static uint32_t last_progress;
    static int stalled_s, low_heap_s;

    stream_stats_t ss;
    player_stats_t ps;
    stream_get_stats(&ss);
    player_get_stats(&ps);

    /* Audio waiting in the ring but none reaching the DAC: the player or
     * I2S is stuck (the player's own checks would have reconnected). A
     * stuck player lets the ring fill, so bytes are the right test here. */
    bool fed = ss.connected && ss.ring_used >= 64 * 1024;
    /* Progress counts frames played or deliberately discarded (catch-up,
     * reconnect dedupe), so a long catch-up is not mistaken for a hang. */
    if (fed && ps.progress == last_progress) {
        if (++stalled_s == STALL_REBOOT_S) reboot_for("player stall");
    } else {
        stalled_s = 0;
    }
    last_progress = ps.progress;

    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < LOW_HEAP_BYTES) {
        if (++low_heap_s == LOW_HEAP_S) reboot_for("low memory");
    } else {
        low_heap_s = 0;
    }
}

static void button_task(void *arg)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(BOOT_GPIO) == 0) {
            if (++held == 30) {
                ESP_LOGW(TAG, "BOOT held 3 s");
                if (provision_active()) provision_stop();
                else provision_start();
            }
        } else {
            held = 0;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    take_reboot_cause();
    settings_load();
    led_init();
    ESP_LOGI(TAG, "downlink %s -> %s", settings_id(), settings_url());

    stream_start(settings_url());
    player_start(settings_volume());

    netlink_callbacks_t ncb = { .on_up = on_net_up, .on_down = on_net_down };
    netlink_start(settings_id(), &ncb);

    provision_init(settings_id(), settings_url());
    provision_set_save_cb(on_portal_save);
    provision_set_reboot_cb(reboot_after_portal_save);

    uplink_callbacks_t ucb = { .on_cmd = on_cmd, .on_up = on_mqtt_up };
    uplink_start(settings_id(), &ucb);
    xTaskCreate(button_task, "button", 2048, NULL, 1, NULL);

    char line[200];
    int last_st = -1;
    uint32_t last_err_seq = 0;
    int64_t last_pub_us = 0;
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int st = update_led();

        /* Health: on every state change or new stream error (at most once a
         * second), on request, and every heartbeat regardless. */
        int64_t now = esp_timer_get_time();
        uint32_t err_seq = stream_error_seq();
        bool changed = st != last_st || err_seq != last_err_seq;
        bool due = s_publish_now || now - last_pub_us >= (int64_t)HEARTBEAT_S * 1000000
                || (changed && now - last_pub_us >= 1000000);
        if (due && uplink_is_up()) {
            s_publish_now = false;
            last_st = st;
            last_err_seq = err_seq;
            last_pub_us = now;
            publish_health(st);
        }

        if (tick % (1000 / TICK_MS)) continue;
        provision_poll();
        supervise();
        if (tick % (HEARTBEAT_S * 1000 / TICK_MS)) continue;
        netlink_status(line, sizeof line);
        ESP_LOGI(TAG, "%s", line);
        stream_status(line, sizeof line);
        ESP_LOGI(TAG, "%s", line);
        uplink_status(line, sizeof line);
        ESP_LOGI(TAG, "%s", line);
        player_status(line, sizeof line);
        ESP_LOGI(TAG, "%s heap=%uK psram=%uK", line,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
