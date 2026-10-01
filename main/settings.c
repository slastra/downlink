#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NS "downlink"

static char s_id[32]   = CONFIG_DL_DEVICE_ID;
static char s_url[256] = CONFIG_DL_STREAM_URL;
static int  s_volume   = CONFIG_DL_VOLUME;

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof s_id;
    if (nvs_get_str(h, "id", s_id, &n) != ESP_OK) strlcpy(s_id, CONFIG_DL_DEVICE_ID, sizeof s_id);
    n = sizeof s_url;
    if (nvs_get_str(h, "url", s_url, &n) != ESP_OK) {
        /* Seed once. The placeholder is not worth persisting: a board built
         * with it should pick up a real URL from a later build. */
        strlcpy(s_url, CONFIG_DL_STREAM_URL, sizeof s_url);
        if (!strstr(s_url, "icecast.example")) {
            nvs_set_str(h, "url", s_url);
            nvs_commit(h);
            ESP_LOGI(TAG, "seeded stream URL from build config");
        }
    }
    uint8_t v;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s_volume = v;
    nvs_close(h);
}

const char *settings_id(void)  { return s_id; }
const char *settings_url(void) { return s_url; }
int settings_volume(void)      { return s_volume; }

bool settings_set_volume(int volume)
{
    if (volume < 0 || volume > 100) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "vol", (uint8_t)volume);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_volume = volume;
    return err == ESP_OK;
}

static bool set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = val[0] ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "%s -> \"%s\" %s", key, val[0] ? val : "(default)", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK;
}

bool settings_set_id(const char *id)
{
    if (strlen(id) >= sizeof s_id) return false;
    return set_str("id", id);
}

bool settings_set_url(const char *url)
{
    if (strlen(url) >= sizeof s_url) return false;
    if (!set_str("url", url)) return false;
    strlcpy(s_url, url[0] ? url : CONFIG_DL_STREAM_URL, sizeof s_url);
    return true;
}
