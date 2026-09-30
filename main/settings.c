#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NS "downlink"

static char s_id[32]   = CONFIG_DL_DEVICE_ID;
static char s_url[256] = CONFIG_DL_STREAM_URL;

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_id;
    if (nvs_get_str(h, "id", s_id, &n) != ESP_OK) strlcpy(s_id, CONFIG_DL_DEVICE_ID, sizeof s_id);
    n = sizeof s_url;
    if (nvs_get_str(h, "url", s_url, &n) != ESP_OK) strlcpy(s_url, CONFIG_DL_STREAM_URL, sizeof s_url);
    nvs_close(h);
}

const char *settings_id(void)  { return s_id; }
const char *settings_url(void) { return s_url; }

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
    return set_str("url", url);
}
