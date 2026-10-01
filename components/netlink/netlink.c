#include "netlink.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "netlink";

#define SCAN_MAX_AP     32
#define ROAM_MARGIN_DB  8
#define ROAM_SCAN_MIN_INTERVAL_US  (30 * 1000 * 1000)
#define QUICK_RETRY_MAX  4
#define QUICK_RETRY_MS   250
#define MAX_CREDS        CONFIG_NETLINK_MAX_CREDS

#define NVS_NS   "wifi"

/* ------------------------------------------------------------------ */
/* credential table                                                    */

static netlink_cred_t     s_creds[MAX_CREDS];
static int                s_ncreds;
static SemaphoreHandle_t  s_cred_lock;

static void creds_load(void)
{
    nvs_handle_t h;
    s_ncreds = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0;
    nvs_get_u8(h, "count", &n);
    if (n > MAX_CREDS) n = MAX_CREDS;
    for (uint8_t i = 0; i < n; i++) {
        char key[8];
        netlink_cred_t *c = &s_creds[s_ncreds];
        size_t len = sizeof c->ssid;
        snprintf(key, sizeof key, "ssid%u", i);
        if (nvs_get_str(h, key, c->ssid, &len) != ESP_OK || c->ssid[0] == 0) continue;
        len = sizeof c->pass;
        snprintf(key, sizeof key, "pass%u", i);
        if (nvs_get_str(h, key, c->pass, &len) != ESP_OK) c->pass[0] = 0;
        snprintf(key, sizeof key, "prio%u", i);
        c->prio = 0;
        nvs_get_u8(h, key, &c->prio);
        s_ncreds++;
    }
    nvs_close(h);
}

static bool creds_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    /* Rewrite the whole table; it is tiny and this is rare. */
    for (int i = 0; i < MAX_CREDS; i++) {
        char key[8];
        snprintf(key, sizeof key, "ssid%u", i); nvs_erase_key(h, key);
        snprintf(key, sizeof key, "pass%u", i); nvs_erase_key(h, key);
        snprintf(key, sizeof key, "prio%u", i); nvs_erase_key(h, key);
    }
    for (int i = 0; i < s_ncreds; i++) {
        char key[8];
        snprintf(key, sizeof key, "ssid%u", i); nvs_set_str(h, key, s_creds[i].ssid);
        snprintf(key, sizeof key, "pass%u", i); nvs_set_str(h, key, s_creds[i].pass);
        snprintf(key, sizeof key, "prio%u", i); nvs_set_u8(h, key, s_creds[i].prio);
    }
    nvs_set_u8(h, "count", (uint8_t)s_ncreds);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static int cred_find(const char *ssid)
{
    for (int i = 0; i < s_ncreds; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* state                                                               */

static bool               s_enabled;
static volatile bool      s_up;
static uint32_t           s_drops, s_fails, s_roams;
static uint32_t           s_backoff_ms = 1000;
static TaskHandle_t       s_join_task;
static int64_t            s_last_roam_scan_us;
static netlink_callbacks_t s_cb;
static char               s_ip[16];

/* Target chosen by the last scan: the AP and which credential it matched. */
static wifi_ap_record_t   s_target;
static int                s_target_cred = -1;
static bool               s_have_target;
static uint32_t           s_quick_retries;
static uint8_t            s_last_reason;

/* Credentials that failed this cycle (by index). Cleared when the cycle
 * exhausts the table, so a site whose AP was merely rebooting gets another
 * chance without anyone touching the station. */
static uint32_t           s_failed_mask;

/* Join watchdog: associated but no IP within the timeout counts as a
 * failure of that credential. */
static esp_timer_handle_t s_join_timer;
static volatile bool      s_paused;
static int64_t            s_last_known_seen_us;

/* ------------------------------------------------------------------ */
/* remembered AP                                                       */

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    char    ssid[33];
} saved_ap_t;

static void save_ap(const wifi_ap_record_t *ap)
{
    saved_ap_t rec = { 0 };
    memcpy(rec.bssid, ap->bssid, 6);
    rec.channel = ap->primary;
    strlcpy(rec.ssid, (const char *)ap->ssid, sizeof rec.ssid);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    saved_ap_t old;
    size_t len = sizeof old;
    if (nvs_get_blob(h, "last", &old, &len) != ESP_OK || len != sizeof old ||
        memcmp(&old, &rec, sizeof rec) != 0) {
        nvs_set_blob(h, "last", &rec, sizeof rec);
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool load_ap(wifi_ap_record_t *out, int *cred)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    saved_ap_t rec;
    size_t len = sizeof rec;
    esp_err_t err = nvs_get_blob(h, "last", &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof rec) return false;
    int ci = cred_find(rec.ssid);
    if (ci < 0) return false;   /* that network was removed since */
    memset(out, 0, sizeof *out);
    memcpy(out->bssid, rec.bssid, 6);
    out->primary = rec.channel;
    strlcpy((char *)out->ssid, rec.ssid, sizeof out->ssid);
    *cred = ci;
    return true;
}

/* ------------------------------------------------------------------ */
/* diagnosis (relo-bridge)                                             */

static bool worth_reknocking(uint8_t reason)
{
    switch (reason) {
    case 0:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}

static void bump_backoff(void)
{
    s_fails++;
    s_backoff_ms *= 2;
    if (s_backoff_ms > CONFIG_NETLINK_MAX_BACKOFF_MS) s_backoff_ms = CONFIG_NETLINK_MAX_BACKOFF_MS;
}

static const char *reason_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return s_have_target ? "that AP did not answer -- moved, off, or a stale cache"
                             : "SSID not visible -- out of range, or 5 GHz only (this radio is 2.4 GHz)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return s_quick_retries <= 1 ? "declined once (band steering does this); knocking again"
                                    : "auth keeps failing -- check the PSK";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "lost the AP -- too far, or it went away";
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* AP selection                                                        */

/*
 * Scan everything and pick the best AP among the credentials we hold:
 * highest priority first, then strongest signal, skipping credentials that
 * failed this cycle. `same_ssid` restricts to one SSID (roam check).
 */
static bool pick_best_ap(wifi_ap_record_t *best, int *cred, const char *same_ssid)
{
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)same_ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 120, .max = 300 } },
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return false;
    }
    uint16_t n = SCAN_MAX_AP;
    static wifi_ap_record_t recs[SCAN_MAX_AP];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK || n == 0) {
        ESP_LOGW(TAG, "scan found no APs");
        esp_wifi_clear_ap_list();
        return false;
    }
    int found = -1, found_cred = -1;
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n; i++) {
        int ci = cred_find((const char *)recs[i].ssid);
        if (ci < 0) continue;
        s_last_known_seen_us = esp_timer_get_time();
        bool failed = (s_failed_mask >> ci) & 1;
        ESP_LOGI(TAG, "  candidate \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch=%2u rssi=%d prio=%u%s",
                 recs[i].ssid, recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                 recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                 recs[i].primary, recs[i].rssi, s_creds[ci].prio, failed ? " (failed this cycle)" : "");
        if (failed) continue;
        if (found < 0 ||
            s_creds[ci].prio > s_creds[found_cred].prio ||
            (s_creds[ci].prio == s_creds[found_cred].prio && recs[i].rssi > recs[found].rssi)) {
            found = i;
            found_cred = ci;
        }
    }
    xSemaphoreGive(s_cred_lock);
    if (found < 0) return false;
    *best = recs[found];
    *cred = found_cred;
    return true;
}

/*
 * Install the credential and pin the AP, then connect.
 *
 * relo-bridge found that setting the PSK and connecting in one go loses the
 * first attempt to a 10 ms auth failure; with one SSID it installed the
 * PSK at start and never touched it again. A table means the PSK can
 * change per attempt. The re-knock path absorbs that first rejection at a
 * cost of 250 ms, which is cheaper than any cleverness here.
 */
static void connect_to(const wifi_ap_record_t *ap, int cred)
{
    wifi_config_t cfg = { 0 };
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    strlcpy((char *)cfg.sta.ssid, s_creds[cred].ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, s_creds[cred].pass, sizeof cfg.sta.password);
    xSemaphoreGive(s_cred_lock);
    cfg.sta.bssid_set = true;
    memcpy(cfg.sta.bssid, ap->bssid, 6);
    cfg.sta.channel = ap->primary;
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s; retrying", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "joining \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d",
             cfg.sta.ssid, ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5], ap->primary, ap->rssi);
    esp_timer_stop(s_join_timer);
    esp_timer_start_once(s_join_timer, (uint64_t)CONFIG_NETLINK_JOIN_TIMEOUT_MS * 1000);
    err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
}

static void join_timeout(void *arg)
{
    if (s_up) return;
    ESP_LOGW(TAG, "no IP within %d ms; giving up on this AP", CONFIG_NETLINK_JOIN_TIMEOUT_MS);
    s_quick_retries = QUICK_RETRY_MAX;   /* force the rescan path */
    esp_wifi_disconnect();               /* DISCONNECTED event drives the join task */
}

/* ------------------------------------------------------------------ */
/* join / roam task                                                    */

#define NOTE_JOIN  1
#define NOTE_ROAM  2

static void join_task(void *arg)
{
    for (;;) {
        uint32_t note = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (note == NOTE_ROAM) {
            int64_t now = esp_timer_get_time();
            if (!s_up || now - s_last_roam_scan_us < ROAM_SCAN_MIN_INTERVAL_US) continue;
            s_last_roam_scan_us = now;
            wifi_ap_record_t cur;
            if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) continue;
            ESP_LOGI(TAG, "rssi low (%d), looking for something better", cur.rssi);
            wifi_ap_record_t best; int cred;
            if (!pick_best_ap(&best, &cred, (const char *)cur.ssid)) continue;
            if (memcmp(best.bssid, cur.bssid, 6) == 0 || best.rssi < cur.rssi + ROAM_MARGIN_DB) {
                ESP_LOGI(TAG, "staying put (best alternative %d dBm)", best.rssi);
                continue;
            }
            ESP_LOGI(TAG, "roaming %d -> %d dBm", cur.rssi, best.rssi);
            s_roams++;
            s_target = best; s_target_cred = cred; s_have_target = true;
            s_quick_retries = 0; s_last_reason = 0;
            esp_wifi_disconnect();
            continue;
        }

        if (s_ncreds == 0 || s_paused) {
            /* Nothing to join, or the portal has the radio. Sleep until a
             * credential arrives or the portal resumes us. */
            continue;
        }

        /* Re-knock on the chosen AP before spending a scan. */
        if (s_have_target && s_quick_retries < QUICK_RETRY_MAX && worth_reknocking(s_last_reason)) {
            if (s_quick_retries > 0) vTaskDelay(pdMS_TO_TICKS(QUICK_RETRY_MS));
            s_quick_retries++;
            connect_to(&s_target, s_target_cred);
            continue;
        }
        if (s_have_target) {
            ESP_LOGW(TAG, "\"%s\" unreachable after %" PRIu32 " attempt(s); trying the next network",
                     s_target.ssid, s_quick_retries);
            if (s_target_cred >= 0) s_failed_mask |= 1u << s_target_cred;
            s_have_target = false;
            bump_backoff();
        }

        wifi_ap_record_t best; int cred;
        if (pick_best_ap(&best, &cred, NULL)) {
            s_target = best; s_target_cred = cred; s_have_target = true;
            s_quick_retries = 0; s_last_reason = 0;
            connect_to(&best, cred);
            continue;
        }

        /* Nothing usable in range. If we excluded anything, forgive it and
         * try the table again after the backoff; otherwise just back off. */
        if (s_failed_mask) {
            ESP_LOGW(TAG, "table exhausted; clearing failures and starting over");
            s_failed_mask = 0;
        } else {
            ESP_LOGW(TAG, "none of %d known network(s) in range", s_ncreds);
        }
        bump_backoff();
        ESP_LOGI(TAG, "waiting %" PRIu32 " ms before retry", s_backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
        xTaskNotify(s_join_task, NOTE_JOIN, eSetValueWithOverwrite);
    }
}

/* ------------------------------------------------------------------ */
/* events                                                              */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = data;
        ESP_LOGI(TAG, "associated on channel %u", e->channel);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        esp_timer_stop(s_join_timer);
        if (s_up) {
            s_up = false;
            s_drops++;
            s_ip[0] = 0;
            if (s_cb.on_down) s_cb.on_down();
            /* Keep the target: a drop is usually the AP, not the choice. */
        }
        s_last_reason = e->reason;
        const char *hint = reason_hint(e->reason);
        if (hint) ESP_LOGW(TAG, "disconnected: reason %u (%s)", e->reason, hint);
        else      ESP_LOGW(TAG, "disconnected: reason %u", e->reason);
        xTaskNotify(s_join_task, NOTE_JOIN, eSetValueWithOverwrite);
        break;
    }
    case WIFI_EVENT_STA_BSS_RSSI_LOW:
        xTaskNotify(s_join_task, NOTE_ROAM, eSetValueWithOverwrite);
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *e = data;
    esp_timer_stop(s_join_timer);
    s_up = true;
    s_last_known_seen_us = esp_timer_get_time();
    s_backoff_ms = 1000;
    s_quick_retries = 0;
    s_failed_mask = 0;
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) save_ap(&ap);
    esp_wifi_set_rssi_threshold(CONFIG_NETLINK_ROAM_RSSI);
    ESP_LOGI(TAG, "up: %s (drops %" PRIu32 ", roams %" PRIu32 ")", s_ip, s_drops, s_roams);
    if (s_cb.on_up) s_cb.on_up();
}

/* ------------------------------------------------------------------ */

void netlink_start(const char *hostname, const netlink_callbacks_t *cb)
{
    if (cb) s_cb = *cb;
    s_cred_lock = xSemaphoreCreateMutex();
    creds_load();
    if (s_ncreds == 0 && strlen(CONFIG_NETLINK_SSID0) > 0) {
        /* Seed from the build once; after that NVS is the truth. */
        strlcpy(s_creds[0].ssid, CONFIG_NETLINK_SSID0, sizeof s_creds[0].ssid);
        strlcpy(s_creds[0].pass, CONFIG_NETLINK_PASS0, sizeof s_creds[0].pass);
        s_creds[0].prio = 0;
        s_ncreds = 1;
        creds_save();
        ESP_LOGI(TAG, "seeded credential table from build config");
    }
    if (s_ncreds == 0) {
        ESP_LOGW(TAG, "no WiFi credentials; radio up, waiting for one (console: wifi add)");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t lerr = esp_event_loop_create_default();
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(lerr);
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (hostname && hostname[0]) esp_netif_set_hostname(sta, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    const esp_timer_create_args_t targs = { .callback = join_timeout, .name = "netlink_join" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_join_timer));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Install the top-priority credential before start so the common
     * single-network case authenticates first time, as relo-bridge found
     * it must. Other credentials are installed per attempt by connect_to. */
    if (s_ncreds > 0) {
        int top = 0;
        for (int i = 1; i < s_ncreds; i++) if (s_creds[i].prio > s_creds[top].prio) top = i;
        wifi_config_t cfg = { 0 };
        strlcpy((char *)cfg.sta.ssid, s_creds[top].ssid, sizeof cfg.sta.ssid);
        strlcpy((char *)cfg.sta.password, s_creds[top].pass, sizeof cfg.sta.password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Mains powered, no BLE: the radio never sleeps. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_enabled = true;

    if (s_ncreds > 0 && load_ap(&s_target, &s_target_cred)) {
        s_have_target = true;
        ESP_LOGI(TAG, "remembered \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch=%u",
                 s_target.ssid, s_target.bssid[0], s_target.bssid[1], s_target.bssid[2],
                 s_target.bssid[3], s_target.bssid[4], s_target.bssid[5], s_target.primary);
    }
    xTaskCreatePinnedToCore(join_task, "wifi_join", 4096, NULL, 5, &s_join_task, 0);
    xTaskNotify(s_join_task, NOTE_JOIN, eSetValueWithOverwrite);
}

bool netlink_is_up(void)
{
    return s_enabled && s_up;
}

void netlink_pause(bool paused)
{
    s_paused = paused;
    if (!paused && s_enabled && !s_up && s_join_task) {
        s_failed_mask = 0;
        s_backoff_ms = 1000;
        xTaskNotify(s_join_task, NOTE_JOIN, eSetValueWithOverwrite);
    }
}

int64_t netlink_last_known_seen_us(void)
{
    return s_last_known_seen_us;
}

int netlink_scan(netlink_scan_entry_t *out, int cap)
{
    if (!s_enabled) return 0;
    wifi_scan_config_t sc = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 120, .max = 300 } },
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t n = SCAN_MAX_AP;
    static wifi_ap_record_t recs[SCAN_MAX_AP];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) { esp_wifi_clear_ap_list(); return 0; }
    int m = 0;
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n && m < cap; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < m; k++) {
            if (!strcmp(out[k].ssid, (const char *)recs[i].ssid)) {
                if (recs[i].rssi > out[k].rssi) out[k].rssi = recs[i].rssi;
                dup = true;
                break;
            }
        }
        if (dup) continue;
        strlcpy(out[m].ssid, (const char *)recs[i].ssid, sizeof out[m].ssid);
        out[m].rssi = recs[i].rssi;
        out[m].known = cred_find(out[m].ssid) >= 0;
        m++;
    }
    xSemaphoreGive(s_cred_lock);
    /* strongest first */
    for (int i = 1; i < m; i++) {
        netlink_scan_entry_t t = out[i]; int k = i - 1;
        while (k >= 0 && out[k].rssi < t.rssi) { out[k + 1] = out[k]; k--; }
        out[k + 1] = t;
    }
    return m;
}

void netlink_get_info(netlink_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->drops = s_drops;
    out->roams = s_roams;
    if (!s_enabled || !s_up) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof out->ssid);
        out->rssi = ap.rssi;
    }
    strlcpy(out->ip, s_ip, sizeof out->ip);
}

void netlink_status(char *out, size_t len)
{
    if (!s_enabled) { snprintf(out, len, "wifi=off creds=%d", s_ncreds); return; }
    if (!s_up) {
        snprintf(out, len, "wifi=down creds=%d fails=%" PRIu32 " drops=%" PRIu32 " backoff=%" PRIu32,
                 s_ncreds, s_fails, s_drops, s_backoff_ms);
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(out, len, "wifi=up ssid=\"%s\" rssi=%d ch=%u ip=%s drops=%" PRIu32 " roams=%" PRIu32,
                 ap.ssid, ap.rssi, ap.primary, s_ip, s_drops, s_roams);
    } else {
        snprintf(out, len, "wifi=up ip=%s drops=%" PRIu32, s_ip, s_drops);
    }
}

/* ---- credential table API ---------------------------------------- */

bool netlink_cred_add(const char *ssid, const char *pass, uint8_t prio)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return false;
    if (!s_cred_lock) s_cred_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    int i = cred_find(ssid);
    bool is_new = i < 0;
    if (is_new) {
        if (s_ncreds >= MAX_CREDS) { xSemaphoreGive(s_cred_lock); return false; }
        i = s_ncreds++;
        s_creds[i].pass[0] = 0;
    }
    strlcpy(s_creds[i].ssid, ssid, sizeof s_creds[i].ssid);
    if (pass) strlcpy(s_creds[i].pass, pass, sizeof s_creds[i].pass);
    s_creds[i].prio = prio;
    bool ok = creds_save();
    xSemaphoreGive(s_cred_lock);
    ESP_LOGI(TAG, "credential \"%s\" prio %u %s", ssid, prio, ok ? "saved" : "NOT saved");
    if (ok && s_enabled && !s_up && s_join_task) {
        s_failed_mask = 0;
        s_backoff_ms = 1000;
        xTaskNotify(s_join_task, NOTE_JOIN, eSetValueWithOverwrite);
    }
    return ok;
}

bool netlink_cred_remove(const char *ssid)
{
    if (!s_cred_lock) s_cred_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    int i = cred_find(ssid);
    if (i < 0) { xSemaphoreGive(s_cred_lock); return false; }
    for (int k = i + 1; k < s_ncreds; k++) s_creds[k - 1] = s_creds[k];
    s_ncreds--;
    memset(&s_creds[s_ncreds], 0, sizeof s_creds[0]);
    bool ok = creds_save();
    xSemaphoreGive(s_cred_lock);
    ESP_LOGI(TAG, "credential \"%s\" removed", ssid);
    return ok;
}

int netlink_cred_list(netlink_cred_t *out, int cap)
{
    if (!s_cred_lock) s_cred_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    int n = s_ncreds < cap ? s_ncreds : cap;
    for (int i = 0; i < n; i++) {
        out[i] = s_creds[i];
        out[i].pass[0] = 0;
    }
    xSemaphoreGive(s_cred_lock);
    return n;
}
