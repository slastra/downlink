/*
 * WiFi station, carried over unchanged from tspl-station.
 *
 * Descended from relo-bridge's netlink with one change of premise: this
 * device is mains powered and has no BLE, so the radio stays awake and there
 * is no boost window. What it adds is a credential TABLE. A station may be
 * moved between sites, and a station with one SSID baked in needs a laptop
 * when that happens; one with a table in NVS needs an MQTT message before
 * the move, or the provisioning portal after it.
 *
 * Join policy: scan, keep the APs whose SSID is in the table, sort by
 * priority then RSSI, skip any credential that failed this cycle, connect.
 * Re-knock on a rejection (band steering), rescan when the AP is absent,
 * back off when the whole table is exhausted, then start the cycle over.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void (*on_up)(void);     /* got an IP */
    void (*on_down)(void);   /* lost it */
} netlink_callbacks_t;

/* Brings up the station; returns immediately. `hostname` becomes the DHCP
 * hostname. With an empty credential table WiFi stays off and the
 * provisioning path (milestone 3) is the way in. */
void netlink_start(const char *hostname, const netlink_callbacks_t *cb);

bool netlink_is_up(void);

/* Fields for the status payload (docs/spec.md §3.3). ssid is "" when down. */
typedef struct {
    char     ssid[33];
    int      rssi;
    char     ip[16];
    uint32_t drops, roams;
} netlink_info_t;
void netlink_get_info(netlink_info_t *out);

/* One line for the console / heartbeat. */
void netlink_status(char *out, size_t len);

/* Provisioning support. pause(true) stops the join loop and any scanning
 * so the portal owns the radio's station side; pause(false) resumes it.
 * last_known_seen_us() is when a scan last saw any SSID from the table (or
 * boot), so the portal can trigger on "nothing known for a while". */
void    netlink_pause(bool paused);
int64_t netlink_last_known_seen_us(void);
/* Scan for the portal's list. Fills up to `cap` (ssid, rssi); returns count. */
typedef struct { char ssid[33]; int rssi; bool known; } netlink_scan_entry_t;
int     netlink_scan(netlink_scan_entry_t *out, int cap);

/* ---- credential table (NVS namespace "wifi") ------------------------- */

typedef struct {
    char    ssid[33];
    char    pass[65];
    uint8_t prio;       /* higher wins */
} netlink_cred_t;

/* Add or replace (by SSID). Persists. Kicks a rejoin if currently down.
 * pass == NULL keeps the stored password of a known SSID (and means an
 * open network for a new one). */
bool netlink_cred_add(const char *ssid, const char *pass, uint8_t prio);
bool netlink_cred_remove(const char *ssid);
/* Copies up to `cap` entries; passwords are blanked. Returns the count. */
int  netlink_cred_list(netlink_cred_t *out, int cap);
