/*
 * Firmware updates over the air, from GitHub Releases.
 *
 * Ported from tspl-station's updater, with three changes for a board that
 * plays music in a venue:
 *
 *   1. Checking and installing are separate. A check (at boot, every few
 *      hours) only reports what is available; an install happens when the
 *      operator says so, because it ends in a reboot and a gap in the music.
 *   2. The manifest names one image per board (`boards.<CONFIG_DL_BOARD>`),
 *      and GitHub serves it through a redirect to a long signed URL.
 *   3. Every sector erase stalls both cores and PSRAM. Playing through a
 *      download was tried: the output never underran, but the music audibly
 *      stuttered. So the downloading 0% event comes FADE_WAIT_MS before the
 *      first write, for the caller to fade the music out, and writes pause
 *      briefly after each 4 KB to let the other tasks breathe.
 *
 * The rest is tspl's: the image's SHA-256 is checked by reading back the
 * written partition before it becomes bootable, the bootloader keeps the
 * old image until updater_mark_valid() (called on reaching the broker), and
 * an image that never confirmed is remembered and not retried unless forced.
 *
 * All work runs on one short-lived task ("ota"); events arrive through the
 * callback, on that task. Do not block in it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPD_EV_CHECKING,      /* fetching the manifest */
    UPD_EV_CURRENT,       /* manifest version == running version */
    UPD_EV_AVAILABLE,     /* a check found another version (check only) */
    UPD_EV_DOWNLOADING,   /* percent: 0, 10, ... 100 */
    UPD_EV_INSTALLED,     /* the new image is bootable; reboot to run it */
    UPD_EV_FAILED,        /* detail says why; the running image is untouched */
} updater_ev_kind_t;

typedef struct {
    updater_ev_kind_t kind;
    int  percent;
    char version[32];
    char detail[96];
} updater_event_t;

typedef enum { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING } updater_state_t;

typedef struct {
    updater_state_t state;
    int      percent;            /* while downloading */
    bool     unconfirmed;        /* running a new image not yet marked valid */
    char     available[32];      /* manifest version, when not the running one */
    int64_t  checked_us;         /* esp_timer time of the last good check; 0 never */
    char     last_error[96];     /* from the last check or install */
    char     rolled_back[32];    /* version the bootloader rejected, if any */
} updater_status_t;

void updater_init(const char *board, void (*on_event)(const updater_event_t *e));

/* Once at boot, after NVS: an update that did not confirm is marked bad. */
void updater_boot_audit(void);

/*
 * Fetch the manifest. With `install`, go on to install its image for this
 * board when the version differs from the running one. A dev build (not
 * exactly a release tag) and an image that failed here before are skipped
 * unless `force`. Returns false (and why) if it could not start.
 */
bool updater_check(const char *manifest_url, bool install, bool force, const char **why);

/* Install a specific image (canary, downgrade, bench). sha256 is required. */
bool updater_install(const char *url, const char *sha256_hex, const char **why);

bool updater_busy(void);

/* Cancel rollback for the running image. Idempotent; call on reaching the
 * broker, which is when the board has shown it can still be managed. */
void updater_mark_valid(void);

/* Running an image still pending verification. */
bool updater_unconfirmed(void);

/* Give up on an unconfirmed image: the bootloader goes back to the old one. */
void updater_rollback_and_reboot(void);

void updater_get_status(updater_status_t *out);
const char *updater_state_name(updater_state_t s);
const char *updater_ev_name(updater_ev_kind_t k);

#ifdef __cplusplus
}
#endif
