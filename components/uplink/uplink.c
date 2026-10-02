#include "uplink.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
/* mqtt_client.h must lead: the two headers include each other. */
#include "mqtt_client.h"
#include "mqtt5_client.h"
#include "nvs.h"

static const char *TAG = "uplink";

#define TOPIC_MAX 96

static char s_id[32];
static char s_client_id[48];
static char s_topic_status[TOPIC_MAX];
static char s_topic_cmd[TOPIC_MAX];
static char s_topic_cmd_result[TOPIC_MAX];

static char s_uri[128], s_user[64], s_pass[64];

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t      s_queue;
static uplink_callbacks_t s_cb;
static volatile bool      s_up, s_enabled;
static bool               s_started;
static int64_t            s_connect_due_us, s_down_since_us;
static uint32_t           s_connect_stalls, s_sent, s_dropped, s_connects;
static volatile bool      s_time_synced;

typedef struct {
    char *topic;
    char *payload;
    int   qos;
    bool  retain;
} pub_item_t;

/* ------------------------------------------------------------------ */
/* time                                                                */

static void on_time_sync(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "time synced");
}

static void iso_now(char *out, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    size_t n = strftime(out, len, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + n, len - n, ".%03dZ", (int)(tv.tv_usec / 1000) % 1000);
}

/* ------------------------------------------------------------------ */
/* config                                                              */

/* NVS "mqtt" wins; the build's seed is written there once, the way netlink
 * seeds WiFi, so an image built without credentials (a public release)
 * keeps the broker a board already knows. */
static void load_broker_config(void)
{
    s_uri[0] = s_user[0] = s_pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open("mqtt", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof s_uri;
    if (nvs_get_str(h, "uri", s_uri, &n) != ESP_OK && CONFIG_UPLINK_URI[0]) {
        nvs_set_str(h, "uri",  CONFIG_UPLINK_URI);
        nvs_set_str(h, "user", CONFIG_UPLINK_USER);
        nvs_set_str(h, "pass", CONFIG_UPLINK_PASS);
        nvs_commit(h);
        strlcpy(s_uri, CONFIG_UPLINK_URI, sizeof s_uri);
        ESP_LOGI(TAG, "seeded broker settings from build config");
    }
    n = sizeof s_user; nvs_get_str(h, "user", s_user, &n);
    n = sizeof s_pass; nvs_get_str(h, "pass", s_pass, &n);
    nvs_close(h);
}

/* Why the last boot happened, in a word. A panic on a board nobody is
 * watching is otherwise invisible; retained status carries it. */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* publishing                                                          */

static bool publish(const char *topic, const char *json, int qos, bool retain)
{
    if (!s_enabled || !s_queue) return false;
    pub_item_t *it = malloc(sizeof *it);
    if (!it) return false;
    it->topic = strdup(topic);
    it->payload = strdup(json);
    it->qos = qos;
    it->retain = retain;
    if (!it->topic || !it->payload || xQueueSend(s_queue, &it, 0) != pdTRUE) {
        free(it->topic); free(it->payload); free(it);
        s_dropped++;
        ESP_LOGW(TAG, "publish queue full; dropped %s", topic);
        return false;
    }
    return true;
}

/* Force a reconnect when the broker has been unreachable (with an IP) for
 * longer than the watchdog allows. esp-mqtt retries on its own; this is for
 * the case where it has wedged. */
static void connect_watchdog(void)
{
    if (s_up || !s_connect_due_us || esp_timer_get_time() <= s_connect_due_us) return;
    s_connect_stalls++;
    ESP_LOGE(TAG, "no broker for %llds (attempt %" PRIu32 "); forcing reconnect to %s",
             (esp_timer_get_time() - s_down_since_us) / 1000000, s_connect_stalls, s_uri);
    esp_mqtt_client_reconnect(s_client);
    s_connect_due_us = esp_timer_get_time() + (int64_t)CONFIG_UPLINK_CONNECT_TIMEOUT_S * 1000000;
}

static void publisher_task(void *arg)
{
    for (;;) {
        pub_item_t *it;
        if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(250)) != pdTRUE) {
            connect_watchdog();
            continue;
        }
        /* Hold the message until the broker is back -- and keep the watchdog
         * running meanwhile, or a queued message would disarm it. */
        while (!s_up) {
            vTaskDelay(pdMS_TO_TICKS(250));
            connect_watchdog();
        }
        int mid = esp_mqtt_client_publish(s_client, it->topic, it->payload, 0, it->qos, it->retain);
        if (mid < 0) {
            ESP_LOGW(TAG, "publish to %s failed; retrying", it->topic);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (xQueueSendToFront(s_queue, &it, 0) == pdTRUE) continue;
            s_dropped++;
        } else {
            s_sent++;
            ESP_LOGD(TAG, "-> %s %s", it->topic, it->payload);
        }
        free(it->topic); free(it->payload); free(it);
    }
}

static void publish_object(const char *topic, cJSON *o, int qos, bool retain)
{
    /* No timestamp until SNTP has synced: 1970 is worse than none, and
     * uptime is in every status anyway. */
    if (s_time_synced) {
        char ts[32];
        iso_now(ts, sizeof ts);
        cJSON_AddStringToObject(o, "timestamp", ts);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return;
    publish(topic, s, qos, retain);
    free(s);
}

void uplink_publish_status(cJSON *o)
{
    if (!s_enabled) { cJSON_Delete(o); return; }
    cJSON_AddBoolToObject(o, "online", true);
    cJSON_AddStringToObject(o, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "reset", reset_reason_str());
    cJSON_AddBoolToObject(o, "timeSynced", s_time_synced);
    cJSON_AddNumberToObject(o, "mqttConnects", s_connects);
    cJSON_AddNumberToObject(o, "heapFree", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heapMin", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psramFree", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    publish_object(s_topic_status, o, 1, true);
}

void uplink_publish_event(const char *sub, cJSON *o)
{
    if (!s_enabled) { cJSON_Delete(o); return; }
    char topic[TOPIC_MAX];
    snprintf(topic, sizeof topic, "%s/%s/%s", CONFIG_UPLINK_TOPIC_ROOT, s_id, sub);
    publish_object(topic, o, 1, false);
}

void uplink_publish_cmd_result_json(const char *cmd, bool ok, cJSON *detail)
{
    if (!s_enabled) { cJSON_Delete(detail); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(o, "ok", ok);
    if (detail) cJSON_AddItemToObject(o, "detail", detail);
    publish_object(s_topic_cmd_result, o, 1, false);
}

void uplink_publish_cmd_result(const char *cmd, bool ok, const char *detail)
{
    uplink_publish_cmd_result_json(cmd, ok, detail ? cJSON_CreateString(detail) : NULL);
}

/* ------------------------------------------------------------------ */
/* inbound                                                             */

static bool topic_is(const esp_mqtt_event_handle_t e, const char *t)
{
    return e->topic_len == (int)strlen(t) && strncmp(e->topic, t, e->topic_len) == 0;
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_up = true;
        s_connects++;
        s_connect_due_us = 0;
        if (s_connect_stalls) {
            ESP_LOGW(TAG, "broker reachable again after %" PRIu32 " stalls", s_connect_stalls);
            s_connect_stalls = 0;
        }
        ESP_LOGI(TAG, "connected as %s", s_client_id);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        if (s_cb.on_up) s_cb.on_up();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_up) {
            s_down_since_us = esp_timer_get_time();
            s_connect_due_us = s_down_since_us + (int64_t)CONFIG_UPLINK_CONNECT_TIMEOUT_S * 1000000;
        }
        s_up = false;
        ESP_LOGW(TAG, "disconnected");
        if (s_cb.on_down) s_cb.on_down();
        break;
    case MQTT_EVENT_DATA:
        if (topic_is(e, s_topic_cmd)) {
            cJSON *root = cJSON_ParseWithLength(e->data, e->data_len);
            if (!root) {
                ESP_LOGW(TAG, "unparseable cmd");
                uplink_publish_cmd_result("", false, "unparseable JSON");
                break;
            }
            if (s_cb.on_cmd) s_cb.on_cmd(root); else cJSON_Delete(root);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "error: transport %d", e->error_handle ? e->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

/* Start the client only once there is an IP: a doomed first attempt costs
 * the whole reconnect backoff. SNTP starts here too, for the timestamps. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_started || !s_client) return;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = on_time_sync;
    esp_netif_sntp_init(&sc);
    s_started = true;
    s_down_since_us = esp_timer_get_time();
    s_connect_due_us = s_down_since_us + (int64_t)CONFIG_UPLINK_CONNECT_TIMEOUT_S * 1000000;
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(err));
        s_started = false;
    }
}

/* ------------------------------------------------------------------ */

void uplink_start(const char *device_id, const uplink_callbacks_t *cb)
{
    if (cb) s_cb = *cb;
    strlcpy(s_id, device_id, sizeof s_id);
    snprintf(s_topic_status,     sizeof s_topic_status,     "%s/%s/status",     CONFIG_UPLINK_TOPIC_ROOT, s_id);
    snprintf(s_topic_cmd,        sizeof s_topic_cmd,        "%s/%s/cmd",        CONFIG_UPLINK_TOPIC_ROOT, s_id);
    snprintf(s_topic_cmd_result, sizeof s_topic_cmd_result, "%s/%s/cmd/result", CONFIG_UPLINK_TOPIC_ROOT, s_id);

    /* The broker drops the older of two clients sharing an id, so two boards
     * left on the default device id would kick each other off in a loop.
     * The MAC tail makes the client id unique; topics stay on the device id. */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof s_client_id, "%s-%02x%02x%02x", s_id, mac[3], mac[4], mac[5]);

    load_broker_config();
    if (!s_uri[0]) {
        ESP_LOGW(TAG, "no broker configured; MQTT off");
        return;
    }
    s_queue = xQueueCreate(CONFIG_UPLINK_QUEUE_DEPTH, sizeof(pub_item_t *));

    /* Retained on the status topic, so a dead board reads as offline
     * instead of showing its last healthy report forever. */
    static const char will[] = "{\"online\":false}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .credentials.client_id = s_client_id,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_pass[0] ? s_pass : NULL,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_topic_status, .msg = will, .msg_len = (int)strlen(will), .qos = 1, .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "client init failed"); return; }

    esp_mqtt5_connection_property_config_t conn = {
        .will_delay_interval = CONFIG_UPLINK_WILL_DELAY_S,
        .payload_format_indicator = true,
    };
    esp_mqtt5_client_set_connect_property(s_client, &conn);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL));

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    xTaskCreatePinnedToCore(publisher_task, "mqtt_pub", 4096, NULL, 4, NULL, 0);
    s_enabled = true;
    ESP_LOGI(TAG, "%s -> %s (topics %s/%s/...)", s_id, s_uri, CONFIG_UPLINK_TOPIC_ROOT, s_id);
}

bool uplink_is_up(void)
{
    return s_enabled && s_up;
}

void uplink_status(char *out, size_t len)
{
    if (!s_enabled) { snprintf(out, len, "mqtt=off"); return; }
    UBaseType_t q = s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
    if (!s_up && s_down_since_us) {
        snprintf(out, len, "mqtt=down %llds sent=%" PRIu32 " queued=%u dropped=%" PRIu32,
                 (esp_timer_get_time() - s_down_since_us) / 1000000, s_sent, (unsigned)q, s_dropped);
    } else {
        snprintf(out, len, "mqtt=%s sent=%" PRIu32 " queued=%u dropped=%" PRIu32 " time=%s",
                 s_up ? "up" : "down", s_sent, (unsigned)q, s_dropped, s_time_synced ? "synced" : "unsynced");
    }
}
