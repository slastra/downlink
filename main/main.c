/*
 * downlink: an Icecast Ogg Opus player for the ESP32-S3 into a PCM5102A.
 *
 *   core 0: WiFi (netlink), HTTP stream task -> 512 KB ring in PSRAM
 *   core 1: player task, ring -> OggOpusDecoder -> volume -> I2S
 *
 * WiFi and the captive portal come from tspl-station unchanged in
 * behaviour: an NVS credential table seeded from menuconfig, and an open
 * AP "downlink-<id>" when no known network is seen for 90 s or BOOT is held
 * for 3 s. The portal also takes the stream URL.
 */
#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
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

static const char *TAG = "main";

#define BOOT_GPIO       0
#define HEARTBEAT_S     30
#define TICK_MS         250

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

static void update_led(void)
{
    static int shown = -1;
    int st = current_status();
    if (st == shown) return;
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
}

static void on_net_up(void)   { stream_set_link(true); }
static void on_net_down(void) { stream_set_link(false); }

static void on_portal_save(const char *id, const char *url)
{
    if (id)  settings_set_id(id);
    if (url) settings_set_url(url);
}

/* Give the "Saved" page time to reach the phone before the AP vanishes. */
static void restart_cb(void *arg) { esp_restart(); }
static void reboot_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "reboot" };
    if (!t && esp_timer_create(&ta, &t) != ESP_OK) esp_restart();
    esp_timer_start_once(t, 1500 * 1000);
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
    settings_load();
    led_init();
    ESP_LOGI(TAG, "downlink %s -> %s", settings_id(), settings_url());

    stream_start(settings_url());
    player_start(CONFIG_DL_VOLUME);

    netlink_callbacks_t ncb = { .on_up = on_net_up, .on_down = on_net_down };
    netlink_start(settings_id(), &ncb);

    provision_init(settings_id(), settings_url());
    provision_set_save_cb(on_portal_save);
    provision_set_reboot_cb(reboot_soon);
    xTaskCreate(button_task, "button", 2048, NULL, 1, NULL);

    char line[200];
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        update_led();
        if (tick % (1000 / TICK_MS)) continue;
        provision_poll();
        if (tick % (HEARTBEAT_S * 1000 / TICK_MS)) continue;
        netlink_status(line, sizeof line);
        ESP_LOGI(TAG, "%s", line);
        stream_status(line, sizeof line);
        ESP_LOGI(TAG, "%s", line);
        player_status(line, sizeof line);
        ESP_LOGI(TAG, "%s heap=%uK psram=%uK", line,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
