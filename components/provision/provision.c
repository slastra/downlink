#include "provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "dns_hijack.h"
#include "netlink.h"
#include "portal_page.h"

static const char *TAG = "provision";

static char   s_id[32];
static char   s_url[256];
static char   s_ap_ssid[48];
static bool   s_active;
static bool   s_ap_netif_created;
static httpd_handle_t     s_httpd;
static esp_timer_handle_t s_timeout;
static int64_t            s_started_us;
static int64_t            s_last_http_us;   /* last request served: the idle clock */

#define SCAN_MAX 24
static netlink_scan_entry_t s_scan[SCAN_MAX];
static int s_nscan;
static void (*s_client_cb)(int);
static void (*s_reboot_cb)(void);
static void (*s_save_cb)(const char *id, const char *url);

void provision_set_reboot_cb(void (*cb)(void)) { s_reboot_cb = cb; }
void provision_set_save_cb(void (*cb)(const char *id, const char *url)) { s_save_cb = cb; }
static bool s_ap_events_registered;

int provision_clients(void)
{
    wifi_sta_list_t sl = { 0 };
    if (!s_active || esp_wifi_ap_get_sta_list(&sl) != ESP_OK) return 0;
    return sl.num;
}

void provision_set_client_cb(void (*cb)(int))
{
    s_client_cb = cb;
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x joined (aid %d)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client %02x:%02x:%02x:%02x:%02x:%02x left (reason %d)",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->reason);
    } else {
        return;
    }
    if (s_client_cb) s_client_cb(provision_clients());
}

/* ------------------------------------------------------------------ */
/* page                                                                */

static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 6 < cap; in++) {
        const char *e = NULL;
        switch (*in) {
        case '&': e = "&amp;"; break;
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '"': e = "&quot;"; break;
        case '\'': e = "&#39;"; break;
        }
        if (e) { size_t n = strlen(e); memcpy(out + o, e, n); o += n; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static int bars(int rssi)
{
    return rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
}

static esp_err_t root_get(httpd_req_t *req)
{
    s_last_http_us = esp_timer_get_time();
    ESP_LOGD(TAG, "GET /");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char esc[100];
    char *buf = malloc(4096);   /* the head alone is ~3.2 KB; keep it off the httpd stack */
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    snprintf(buf, 4096, PORTAL_HEAD, s_id, netlink_is_up() ? "connected" : "not connected");
    httpd_resp_sendstr_chunk(req, buf);
    for (int i = 0; i < s_nscan; i++) {
        html_escape(s_scan[i].ssid, esc, sizeof esc);
        snprintf(buf, 4096, PORTAL_ROW, esc, i == 0 ? " checked" : "", esc,
                 s_scan[i].known ? "<span class=k>&#10003; saved</span>" : "", bars(s_scan[i].rssi));
        httpd_resp_sendstr_chunk(req, buf);
    }
    char url_esc[400];
    html_escape(s_url, url_esc, sizeof url_esc);
    snprintf(buf, 4096, PORTAL_TAIL, url_esc, s_id);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, NULL);
    free(buf);
    return ESP_OK;
}

/* application/x-www-form-urlencoded → value for key (decoded). */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '+') { out[o++] = ' '; p++; }
                else if (*p == '%' && p[1] && p[2]) {
                    char h[3] = { p[1], p[2], 0 };
                    out[o++] = (char)strtol(h, NULL, 16);
                    p += 3;
                } else out[o++] = *p++;
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = 0;
    return false;
}

static esp_err_t save_post(httpd_req_t *req)
{
    s_last_http_us = esp_timer_get_time();
    char body[1024];
    if (req->content_len >= sizeof body) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "form too large");
        return ESP_FAIL;
    }
    /* A form can arrive in more than one segment; read until content_len. */
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        got += n;
    }
    body[got] = 0;

    char ssid[40], ssid2[40], pass[72], prio_s[8], id[32], url[256];
    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "ssid2", ssid2, sizeof ssid2);
    form_get(body, "pass", pass, sizeof pass);
    form_get(body, "prio", prio_s, sizeof prio_s);
    form_get(body, "id", id, sizeof id);
    form_get(body, "url", url, sizeof url);
    if (!ssid[0]) strlcpy(ssid, ssid2, sizeof ssid);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char page[1200];
    if (!ssid[0] || strlen(ssid) > 32) {
        snprintf(page, sizeof page, PORTAL_DONE, "No network chosen", "Pick one from the list or type its name. <a href=/>Back</a>");
        httpd_resp_sendstr(req, page);
        return ESP_OK;
    }
    int prio = atoi(prio_s);
    if (prio < 0) prio = 0;
    if (prio > 255) prio = 255;

    /* Never log the password; the ssid/prio/id line is all anyone needs. */
    bool ok = netlink_cred_add(ssid, pass, (uint8_t)prio);
    ESP_LOGI(TAG, "save: ssid=\"%s\" prio=%d id=\"%s\" -> %s", ssid, prio, id, ok ? "stored" : "REJECTED");
    /* The app owns its settings; pass along only what changed. */
    if (ok && s_save_cb) {
        bool id_changed  = id[0] && strcmp(id, s_id) != 0;
        bool url_changed = url[0] && strcmp(url, s_url) != 0;
        if (id_changed || url_changed) s_save_cb(id_changed ? id : NULL, url_changed ? url : NULL);
    }
    char esc[100], msg[300];
    html_escape(ssid, esc, sizeof esc);
    snprintf(msg, sizeof msg, ok ? "<b>%s</b> stored. The player is rebooting now and this network will disappear."
                                 : "<b>%s</b> was rejected: name too long, or the table is full.", esc);
    snprintf(page, sizeof page, PORTAL_DONE, ok ? "Saved" : "Not saved", msg);
    httpd_resp_sendstr(req, page);
    /* Measured once so the stack size below is a number, not a guess. */
    ESP_LOGI(TAG, "httpd stack high-water mark after save: %u bytes free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (ok) { if (s_reboot_cb) s_reboot_cb(); else esp_restart(); }
    return ESP_OK;
}

/* Everything else: captive-portal probes (Android generate_204, Apple
 * hotspot-detect, Windows connecttest) and stray URLs, redirected to the
 * page. iOS wants a body, not just the redirect. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    s_last_http_us = esp_timer_get_time();
    ESP_LOGD(TAG, "redirecting %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "Redirecting to the downlink setup page");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

/*
 * Lifetime policy. The portal takes the station off the fleet network, so
 * it must not outlive its usefulness -- but a person mid-form must never
 * watch it vanish either. So: close after PROVISION_TIMEOUT_S with nobody
 * associated, extend in one-minute steps while someone is on it AND has
 * touched the page in the last PROVISION_IDLE_S, and close regardless at
 * PROVISION_MAX_S. A phone that merely stays associated cannot hold the
 * station hostage.
 */
static void timeout_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    int64_t age_s  = (now - s_started_us) / 1000000;
    int64_t idle_s = (now - s_last_http_us) / 1000000;
    int clients = provision_clients();
    if (clients > 0 && idle_s < CONFIG_PROVISION_IDLE_S && age_s < CONFIG_PROVISION_MAX_S) {
        ESP_LOGI(TAG, "portal open %llds, %d client(s), last request %llds ago; extending", age_s, clients, idle_s);
        esp_timer_start_once(s_timeout, 60ULL * 1000000);
        return;
    }
    ESP_LOGW(TAG, "portal closing (open %llds, %d client(s), idle %llds)", age_s, clients, idle_s);
    provision_stop();
}

bool provision_active(void)
{
    return s_active;
}

void provision_init(const char *device_id, const char *stream_url)
{
    strlcpy(s_id, device_id, sizeof s_id);
    strlcpy(s_url, stream_url, sizeof s_url);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "downlink-%s", s_id);
    const esp_timer_create_args_t ta = { .callback = timeout_cb, .name = "prov_timeout" };
    esp_timer_create(&ta, &s_timeout);
}

void provision_start(void)
{
    if (s_active) return;
    ESP_LOGW(TAG, "starting portal: AP \"%s\", open, http://192.168.4.1/", s_ap_ssid);
    s_active = true;
    s_started_us = esp_timer_get_time();
    s_last_http_us = s_started_us;

    /* Own the station side of the radio: no joins or roam scans while the
     * portal runs, but the scan list needs one scan first. */
    netlink_pause(true);
    s_nscan = netlink_scan(s_scan, SCAN_MAX);
    ESP_LOGI(TAG, "scan list: %d networks", s_nscan);

    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }
    if (!s_ap_events_registered) {
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, NULL, NULL);
        s_ap_events_registered = true;
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    /* AP+STA only while the station side has a link. With the STA idle the
     * driver holds no channel and the AP never beacons -- seen on the bench:
     * "softAP started", DHCP up, nothing on the air. AP-only fixes that, and
     * the scan above already happened while the STA was still there. */
    /* Stop, configure, start: configuring the AP on a running driver left
     * it silent on the bench (mode "softAP" logged, DHCP up, ssid-not-found
     * from every client). Costs the station link for a moment; it is being
     * provisioned, so that is fine. */
    bool sta_up = netlink_is_up();
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(sta_up ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 7;
    hc.lru_purge_enable = true;
    /* 4 KB overflowed in save_post (2.3 KB of locals over an NVS commit);
     * the handler logs its high-water mark so this can be trimmed later. */
    hc.stack_size = 10240;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &save);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    }
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    dns_hijack_start(ESP_IP4TOADDR(192, 168, 4, 1));

    esp_timer_stop(s_timeout);
    esp_timer_start_once(s_timeout, (uint64_t)CONFIG_PROVISION_TIMEOUT_S * 1000000);
}

void provision_stop(void)
{
    /* Not re-entrant: the timer and the console can both get here. Clear
     * the flag first so the second caller, and the AP_STADISCONNECTED
     * events esp_wifi_stop raises, see a portal that is already closed. */
    if (!s_active) return;
    s_active = false;
    esp_timer_stop(s_timeout);
    dns_hijack_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    netlink_pause(false);
    ESP_LOGI(TAG, "portal closed");
    if (s_client_cb) s_client_cb(0);   /* let the app know the portal is gone */
}

void provision_poll(void)
{
#if CONFIG_PROVISION_AUTO_S > 0
    if (s_active || netlink_is_up()) return;
    int64_t idle = (esp_timer_get_time() - netlink_last_known_seen_us()) / 1000000;
    if (idle >= CONFIG_PROVISION_AUTO_S) {
        ESP_LOGW(TAG, "no known network for %llds", idle);
        provision_start();
    }
#endif
}
