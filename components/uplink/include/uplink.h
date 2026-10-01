/*
 * MQTT uplink: how a downlink deployed out of reach reports its health and
 * takes commands. The connection is outbound only, so it works behind any
 * site's NAT. Carried over from tspl-station's uplink (publisher task,
 * connect watchdog, MQTT 5 delayed will, SNTP) without the label contract.
 *
 * Topics, under CONFIG_UPLINK_TOPIC_ROOT and the device id:
 *   <root>/<id>/status      retained health; the will sets {"online":false}
 *   <root>/<id>/cmd         inbound JSON commands, {"cmd":"...", ...}
 *   <root>/<id>/cmd/result  one reply per command
 *
 * Nothing publishes from esp-mqtt's event task or blocks a caller on the
 * network: publishes go through a queue to one publisher task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

typedef struct {
    /* A parsed command (ownership transferred). Called on the esp-mqtt
     * event task: act quickly or hand it off. */
    void (*on_cmd)(cJSON *root);
    void (*on_up)(void);
    void (*on_down)(void);
} uplink_callbacks_t;

/* Returns immediately; the client starts when an IP arrives. */
void uplink_start(const char *device_id, const uplink_callbacks_t *cb);
bool uplink_is_up(void);

/* Retained status. Takes ownership of `o` (the app's fields) and adds
 * online, fw, reset reason, uptime, heap figures and a timestamp. */
void uplink_publish_status(cJSON *o);

/* <root>/<id>/cmd/result. `detail` is a message, or NULL. */
void uplink_publish_cmd_result(const char *cmd, bool ok, const char *detail);
/* Same, with a structured detail (ownership taken). */
void uplink_publish_cmd_result_json(const char *cmd, bool ok, cJSON *detail);

/* One line for the serial heartbeat. */
void uplink_status(char *out, size_t len);
