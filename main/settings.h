/*
 * Device settings in NVS (namespace "downlink"), seeded from Kconfig.
 * NVS wins when a key is present, so the portal can change them without a
 * rebuild.
 */
#pragma once
#include <stdbool.h>

void settings_load(void);
const char *settings_id(void);
const char *settings_url(void);

/* Persist. An empty string erases the key, falling back to the Kconfig seed. */
bool settings_set_id(const char *id);
bool settings_set_url(const char *url);
