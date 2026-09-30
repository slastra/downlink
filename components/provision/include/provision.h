/*
 * Soft-AP captive portal for entering WiFi credentials, the stream URL and
 * the device id without a laptop. Carried over from tspl-station.
 *
 * Brings up an open AP "downlink-<id>" alongside the station, answers
 * every DNS query with its own address, serves one page with a live scan
 * list, and writes what is submitted to NVS via netlink_cred_add(). Then
 * reboots. Closes on its own: PROVISION_TIMEOUT_S with nobody on it,
 * PROVISION_IDLE_S without a page request from a connected client, or
 * PROVISION_MAX_S regardless.
 */
#pragma once
#include <stdbool.h>

void provision_init(const char *device_id, const char *stream_url);
/* Start the portal (idempotent). Called by the BOOT hold, the console, and
 * the automatic trigger. */
void provision_start(void);
void provision_stop(void);
bool provision_active(void);
/* Clients associated to the soft-AP right now. */
int  provision_clients(void);
/* Called when the client count changes (esp event task; do not block). */
void provision_set_client_cb(void (*cb)(int clients));
/* Called on save with whichever of id / url changed (NULL if unchanged),
 * before the reboot. The app persists them. */
void provision_set_save_cb(void (*cb)(const char *id, const char *url));
/* How to reboot after a save; defaults to esp_restart(). */
void provision_set_reboot_cb(void (*cb)(void));
/* Call periodically (the status task does): starts the portal when the
 * station has seen no known network for PROVISION_AUTO_S. */
void provision_poll(void);
