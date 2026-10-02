/*
 * Why the last crash happened, for a board nobody can plug a cable into.
 *
 * IDF's panic handler prints the reason and a backtrace to the console and
 * resets, which tells a deployed board's operator only "reset: panic". This
 * wraps that handler (-Wl,--wrap=esp_panic_handler) to copy the essentials
 * into RTC memory first -- it survives the panic reset, not a power cut --
 * so the next boot can put them in the status:
 *
 *   "crash": {"fw","reason","task","core","uptimeS","backtrace", "wdt"?}
 *
 * The backtrace is program counters only; decode them against that
 * release's downlink-<board>-<tag>.elf (README "Crashes").
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* At boot, before anything that could crash: takes last boot's record (if
 * this boot followed a panic) and arms the handler for this one. */
void crash_init(void);

/* Adds "crash" to the status when the last reset was a recorded crash. */
void crash_add_json(cJSON *status);

#ifdef __cplusplus
}
#endif
