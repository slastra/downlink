#include "updater.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
/* PSA, not mbedtls/sha256.h: IDF v6 ships mbedTLS 4, which dropped the
 * legacy hash headers. */
#include "psa/crypto.h"

static const char *TAG = "updater";

#define URL_MAX        512
#define SHA_HEX_LEN    64
#define MANIFEST_MAX   2048
#define MAX_REDIRECTS  5
/* esp_https_ota_perform() reports a read timeout as "in progress", so a
 * server that goes quiet without closing would hold the update (and its
 * LED) forever. These bound it. */
/* Time the caller gets, after the downloading 0% event, to fade the music
 * out before the first flash erase (main.c: player_set_hold). */
#define FADE_WAIT_MS   600
#define STALL_S        30
#define DOWNLOAD_MAX_S (10 * 60)
#define USER_AGENT     "downlink (ESP32-S3)"
/* Both transfers run TLS on this stack, which must be internal RAM: a PSRAM
 * stack is unreachable while a flash write has the cache off. */
#define TASK_STACK     10240
/* Below this much free internal RAM an update could starve the rest of the
 * board (the stack above, HTTP buffers, TLS bookkeeping: ~25 KB at peak). */
#define MIN_FREE_HEAP  (40 * 1024)
/* GitHub answers release downloads with a redirect to a signed URL of
 * ~800 characters, and the request line has to fit the tx buffer. Both are
 * sized past SPIRAM_MALLOC_ALWAYSINTERNAL so malloc puts them (and
 * esp_https_ota's write buffer, sized from rx) in PSRAM: a TLS handshake
 * already takes ~30 KB of internal RAM for a moment, measured. */
#define HTTP_RX_BUF    (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL + 1024)
#define HTTP_TX_BUF    (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL + 1024)

typedef enum { JOB_CHECK, JOB_INSTALL } job_kind_t;

static struct {
    job_kind_t kind;
    bool install, force;
    char manifest[URL_MAX];
    char url[URL_MAX];
    char sha[SHA_HEX_LEN + 1];
    char version[32];         /* expected in the image; "" = any */
} s_job;

static char s_board[24];
static void (*s_on_event)(const updater_event_t *e);
static SemaphoreHandle_t s_lock;
static updater_status_t s_st;
static volatile bool s_busy;

/* ---- small helpers -------------------------------------------------- */

static void set_state(updater_state_t st, int percent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.state = st;
    s_st.percent = percent;
    xSemaphoreGive(s_lock);
}

static void emit(updater_ev_kind_t kind, int percent, const char *version, const char *detail)
{
    updater_event_t e = { .kind = kind, .percent = percent };
    if (version) strlcpy(e.version, version, sizeof e.version);
    if (detail) strlcpy(e.detail, detail, sizeof e.detail);
    if (kind == UPD_EV_FAILED) {
        ESP_LOGE(TAG, "%s", e.detail);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_st.last_error, e.detail, sizeof s_st.last_error);
        xSemaphoreGive(s_lock);
    }
    if (s_on_event) s_on_event(&e);
}

static const char *running_version(void) { return esp_app_get_description()->version; }

/* A release is exactly a tag: "v0.1.0", not "v0.1.0-3-gabc1234", "abc1234"
 * or anything "-dirty". Anything else is a bench build, which a manifest
 * must not silently replace. */
static bool is_release(const char *v)
{
    return v[0] == 'v' && isdigit((unsigned char)v[1]) && !strstr(v, "-g") && !strstr(v, "-dirty");
}

static bool is_sha_hex(const char *s)
{
    if (strlen(s) != SHA_HEX_LEN) return false;
    for (const char *p = s; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
    return true;
}

/* ---- failed-image memory (NVS "ota": pending, bad) ---------------------- */

static void ota_nvs_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (val) nvs_set_str(h, key, val); else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool ota_nvs_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = 0;
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = cap;
    bool ok = nvs_get_str(h, key, out, &n) == ESP_OK && out[0];
    nvs_close(h);
    return ok;
}

bool updater_unconfirmed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

void updater_boot_audit(void)
{
    /* The bootloader's own record: the slot it refused or abandoned. */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        strlcpy(s_st.rolled_back, d.version, sizeof s_st.rolled_back);
        ESP_LOGW(TAG, "rolled back from %s", d.version);
    }
    if (updater_unconfirmed()) return;   /* we are the candidate; the pending SHA is ours to confirm */
    char pending[SHA_HEX_LEN + 1];
    if (ota_nvs_get("pending", pending, sizeof pending)) {
        ESP_LOGW(TAG, "the last update (sha %.12s...) did not confirm; skipping that image unless forced", pending);
        ota_nvs_set("bad", pending);
        ota_nvs_set("pending", NULL);
    }
}

void updater_mark_valid(void)
{
    if (!updater_unconfirmed()) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "new image %s confirmed: it reached the broker, so rollback is cancelled", running_version());
        ota_nvs_set("pending", NULL);
    }
}

void updater_rollback_and_reboot(void)
{
    ESP_LOGE(TAG, "new image %s never confirmed; rolling back", running_version());
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();   /* not an OTA boot after all: a plain reboot */
}

/* ---- install ----------------------------------------------------------- */

/*
 * Hash what actually landed in flash, not what we think we sent: catches a
 * truncated download, a proxy that served something else, and a write that
 * did not stick. Also checks the image was built for this board's flash
 * size: an N16R8 image on a SuperMini (or the reverse) would otherwise boot
 * into the wrong PSRAM and flash setup.
 */
static const char *verify_written(const esp_partition_t *part, size_t len, const char *want_hex)
{
    esp_image_header_t mine, theirs;
    if (esp_partition_read(esp_ota_get_running_partition(), 0, &mine, sizeof mine) != ESP_OK ||
        esp_partition_read(part, 0, &theirs, sizeof theirs) != ESP_OK) return "flash read failed";
    if (mine.spi_size != theirs.spi_size || mine.chip_id != theirs.chip_id) return "image is for another board";

    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) return "no memory";
    const char *why = NULL;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        why = "sha256 unavailable";
    }
    for (size_t off = 0; !why && off < len; ) {
        size_t n = len - off < 4096 ? len - off : 4096;
        if (esp_partition_read(part, off, buf, n) != ESP_OK || psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            why = "flash read failed";
        }
        off += n;
    }
    uint8_t digest[32];
    size_t dlen = 0;
    if (!why && (psa_hash_finish(&op, digest, sizeof digest, &dlen) != PSA_SUCCESS || dlen != sizeof digest)) {
        why = "sha256 failed";
    }
    if (why) psa_hash_abort(&op);
    free(buf);
    if (why) return why;

    char got[SHA_HEX_LEN + 1];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        got[2 * i] = hex[digest[i] >> 4];
        got[2 * i + 1] = hex[digest[i] & 15];
    }
    got[SHA_HEX_LEN] = 0;
    if (strcasecmp(got, want_hex)) {
        ESP_LOGE(TAG, "sha256 mismatch\n  want %s\n  got  %s", want_hex, got);
        return "sha256 mismatch";
    }
    return NULL;
}

static void install(const char *url, const char *sha, const char *expect_version)
{
    char why[96] = "";
    ESP_LOGI(TAG, "installing %s", url);
    set_state(UPD_DOWNLOADING, 0);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        .keep_alive_enable = true,
        .user_agent = USER_AGENT,
        .max_redirection_count = MAX_REDIRECTS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t cfg = { .http_config = &http, .buffer_caps = MALLOC_CAP_SPIRAM };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        snprintf(why, sizeof why, "download failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* The app descriptor arrives with the first block: reject the wrong
     * project or version before a single sector is erased. */
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) {
        snprintf(why, sizeof why, "not a firmware image");
        goto abort;
    }
    if (strcmp(desc.project_name, esp_app_get_description()->project_name)) {
        snprintf(why, sizeof why, "image is \"%.32s\", not downlink", desc.project_name);
        goto abort;
    }
    if (expect_version[0] && strcmp(desc.version, expect_version)) {
        snprintf(why, sizeof why, "image is %.32s, manifest says %.32s", desc.version, expect_version);
        goto abort;
    }
    char version[32];
    strlcpy(version, desc.version, sizeof version);

    /* Nothing has been written yet: the first perform() erases. Announce
     * the download now (a wrong image never gets this far, so it never
     * interrupts the music) and give the caller time to fade out. */
    emit(UPD_EV_DOWNLOADING, 0, version, NULL);
    vTaskDelay(pdMS_TO_TICKS(FADE_WAIT_MS));

    int total = esp_https_ota_get_image_size(h);
    int last_step = 0, last_got = -1;
    int64_t t0 = esp_timer_get_time(), moved_us = t0;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        int64_t now = esp_timer_get_time();
        if (got != last_got) {
            last_got = got;
            moved_us = now;
        } else if (now - moved_us > (int64_t)STALL_S * 1000000) {
            snprintf(why, sizeof why, "download stalled at %d of %d bytes", got, total);
            goto abort;
        }
        if (now - t0 > (int64_t)DOWNLOAD_MAX_S * 1000000) {
            snprintf(why, sizeof why, "download took over %d minutes", DOWNLOAD_MAX_S / 60);
            goto abort;
        }
        int pct = total > 0 ? (int)((int64_t)got * 100 / total) : -1;
        if (pct >= 0 && pct / 10 > last_step && pct < 100) {
            last_step = pct / 10;
            set_state(UPD_DOWNLOADING, last_step * 10);
            emit(UPD_EV_DOWNLOADING, last_step * 10, version, NULL);
        }
        /* One 4 KB block per perform, and up to one sector erase with it:
         * let the player catch up before the next stall. */
        if (CONFIG_DL_OTA_PAUSE_MS) vTaskDelay(pdMS_TO_TICKS(CONFIG_DL_OTA_PAUSE_MS));
    }
    if (err != ESP_OK) {
        snprintf(why, sizeof why, "download failed: %s", esp_err_to_name(err));
        goto abort;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        snprintf(why, sizeof why, "download incomplete");
        goto abort;
    }

    /* Verify before finish(), because finish() is what makes the slot
     * bootable: a bad image that never becomes bootable costs nothing. */
    int len = esp_https_ota_get_image_len_read(h);
    const char *bad = verify_written(esp_ota_get_next_update_partition(NULL), (size_t)len, sha);
    if (bad) {
        strlcpy(why, bad, sizeof why);
        if (!strcmp(bad, "sha256 mismatch") || !strcmp(bad, "image is for another board")) ota_nvs_set("bad", sha);
        goto abort;
    }
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        snprintf(why, sizeof why, "image rejected: %s", esp_err_to_name(err));
        goto fail;
    }
    ota_nvs_set("pending", sha);
    ESP_LOGW(TAG, "installed %s: %d bytes in %d s; reboot to run it", version, len,
             (int)((esp_timer_get_time() - t0) / 1000000));
    set_state(UPD_IDLE, 0);
    emit(UPD_EV_INSTALLED, 100, version, NULL);
    return;

abort:
    esp_https_ota_abort(h);
fail:
    set_state(UPD_IDLE, 0);
    emit(UPD_EV_FAILED, 0, NULL, why);
}

/* ---- manifest ---------------------------------------------------------- */

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Fetch into `buf`; returns bytes or -1 with `why` set. esp_http_client_open
 * does not follow redirects by itself, and GitHub's "latest" is one. */
static int fetch_manifest(const char *url, char *buf, size_t cap, char *why, size_t whycap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        .user_agent = USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(why, whycap, "bad manifest url"); return -1; }
    int n = -1, status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            snprintf(why, whycap, "unreachable: %s", esp_err_to_name(e));
            goto out;
        }
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (!is_redirect(status)) break;
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    if (status != 200) {
        snprintf(why, whycap, "manifest: HTTP %d", status);
        goto out;
    }
    n = 0;
    for (;;) {
        int r = esp_http_client_read(c, buf + n, (int)(cap - 1 - n));
        if (r < 0) { snprintf(why, whycap, "manifest: read failed"); n = -1; break; }
        if (r == 0) break;
        n += r;
        if ((size_t)n >= cap - 1) { snprintf(why, whycap, "manifest over %d bytes", MANIFEST_MAX); n = -1; break; }
    }
    if (n >= 0) buf[n] = 0;
out:
    esp_http_client_cleanup(c);
    return n;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static void check(void)
{
    char why[96] = "";
    set_state(UPD_CHECKING, 0);
    emit(UPD_EV_CHECKING, 0, NULL, NULL);

    char *buf = heap_caps_malloc(MANIFEST_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) { emit(UPD_EV_FAILED, 0, NULL, "no memory"); set_state(UPD_IDLE, 0); return; }
    int n = fetch_manifest(s_job.manifest, buf, MANIFEST_MAX, why, sizeof why);
    cJSON *m = n >= 0 ? cJSON_ParseWithLength(buf, (size_t)n) : NULL;
    free(buf);
    if (n < 0) { set_state(UPD_IDLE, 0); emit(UPD_EV_FAILED, 0, NULL, why); return; }

    const char *version = m ? jstr(m, "version") : NULL;
    const cJSON *boards = m ? cJSON_GetObjectItemCaseSensitive(m, "boards") : NULL;
    const cJSON *mine = boards ? cJSON_GetObjectItemCaseSensitive(boards, s_board) : NULL;
    const char *url = mine ? jstr(mine, "url") : NULL;
    const char *sha = mine ? jstr(mine, "sha256") : NULL;
    if (!m || !version || strlen(version) >= sizeof s_job.version) {
        snprintf(why, sizeof why, m ? "bad manifest: no version" : "bad manifest: not JSON");
    } else if (!cJSON_IsObject(mine)) {
        snprintf(why, sizeof why, "manifest %.32s has no image for board \"%s\"", version, s_board);
    } else if (!url || strlen(url) >= URL_MAX || !sha || !is_sha_hex(sha)) {
        snprintf(why, sizeof why, "bad manifest: board \"%s\" needs url and sha256", s_board);
    }
    if (why[0]) {
        cJSON_Delete(m);
        set_state(UPD_IDLE, 0);
        emit(UPD_EV_FAILED, 0, NULL, why);
        return;
    }

    const char *running = running_version();
    bool current = !strcmp(version, running);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.checked_us = esp_timer_get_time();
    s_st.last_error[0] = 0;
    strlcpy(s_st.available, current ? "" : version, sizeof s_st.available);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "manifest: %s (running %s)%s", version, running, current ? ", current" : "");

    char bad[SHA_HEX_LEN + 1];
    if (current) {
        set_state(UPD_IDLE, 0);
        emit(UPD_EV_CURRENT, 0, version, NULL);
    } else if (!s_job.install) {
        set_state(UPD_IDLE, 0);
        emit(UPD_EV_AVAILABLE, 0, version, NULL);
    } else if (!s_job.force && !is_release(running)) {
        set_state(UPD_IDLE, 0);
        snprintf(why, sizeof why, "running a dev build (%.32s); send force to replace it", running);
        emit(UPD_EV_FAILED, 0, version, why);
    } else if (!s_job.force && ota_nvs_get("bad", bad, sizeof bad) && !strcasecmp(bad, sha)) {
        set_state(UPD_IDLE, 0);
        snprintf(why, sizeof why, "%.32s failed here before; send force to retry it", version);
        emit(UPD_EV_FAILED, 0, version, why);
    } else {
        strlcpy(s_job.url, url, sizeof s_job.url);
        strlcpy(s_job.sha, sha, sizeof s_job.sha);
        strlcpy(s_job.version, version, sizeof s_job.version);
        cJSON_Delete(m);
        install(s_job.url, s_job.sha, s_job.version);
        return;
    }
    cJSON_Delete(m);
}

/* ---- task and API ---------------------------------------------------- */

static void ota_task(void *arg)
{
    if (s_job.kind == JOB_CHECK) check();
    else install(s_job.url, s_job.sha, s_job.version);
    ESP_LOGI(TAG, "done; stack %u spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_busy = false;
    vTaskDelete(NULL);
}

static bool claim(const char **why)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool ok = false;
    taskENTER_CRITICAL(&mux);
    if (!s_busy) s_busy = ok = true;
    taskEXIT_CRITICAL(&mux);
    if (!ok) { *why = "an update is already running"; return false; }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MIN_FREE_HEAP ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < TASK_STACK + 1024) {
        s_busy = false;
        *why = "low memory";
        return false;
    }
    return true;
}

static bool launch(const char **why)
{
    if (xTaskCreatePinnedToCore(ota_task, "ota", TASK_STACK, NULL, 3, NULL, 0) != pdPASS) {
        s_busy = false;
        *why = "no memory for the update task";
        return false;
    }
    return true;
}

void updater_init(const char *board, void (*on_event)(const updater_event_t *e))
{
    strlcpy(s_board, board, sizeof s_board);
    s_on_event = on_event;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool updater_check(const char *manifest_url, bool install, bool force, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    if (!manifest_url || strncmp(manifest_url, "http", 4) || strlen(manifest_url) >= URL_MAX) {
        *why = "bad manifest url";
        return false;
    }
    if (!claim(why)) return false;
    s_job.kind = JOB_CHECK;
    s_job.install = install;
    s_job.force = force;
    strlcpy(s_job.manifest, manifest_url, sizeof s_job.manifest);
    return launch(why);
}

bool updater_install(const char *url, const char *sha256_hex, const char **why)
{
    const char *dummy;
    if (!why) why = &dummy;
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strlen(url) >= URL_MAX) {
        *why = "bad url";
        return false;
    }
    if (!sha256_hex || !is_sha_hex(sha256_hex)) {
        *why = "sha256 must be 64 hex characters";
        return false;
    }
    if (!claim(why)) return false;
    s_job.kind = JOB_INSTALL;
    strlcpy(s_job.url, url, sizeof s_job.url);
    strlcpy(s_job.sha, sha256_hex, sizeof s_job.sha);
    s_job.version[0] = 0;
    return launch(why);
}

bool updater_busy(void) { return s_busy; }

void updater_get_status(updater_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
    out->unconfirmed = updater_unconfirmed();
}

const char *updater_state_name(updater_state_t s)
{
    switch (s) {
    case UPD_CHECKING:    return "checking";
    case UPD_DOWNLOADING: return "downloading";
    default:              return "idle";
    }
}

const char *updater_ev_name(updater_ev_kind_t k)
{
    switch (k) {
    case UPD_EV_CHECKING:    return "checking";
    case UPD_EV_CURRENT:     return "current";
    case UPD_EV_AVAILABLE:   return "available";
    case UPD_EV_DOWNLOADING: return "downloading";
    case UPD_EV_INSTALLED:   return "installed";
    default:                 return "failed";
    }
}
