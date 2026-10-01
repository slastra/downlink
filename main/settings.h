/*
 * Device settings in NVS (namespace "downlink"). The build's values are a
 * seed: copied into NVS on first boot, after which NVS is the truth and the
 * portal or an MQTT command changes them. That keeps a board's stream URL
 * through an update to a public image built without it.
 */
#pragma once
#include <stdbool.h>

void settings_load(void);
const char *settings_id(void);
const char *settings_url(void);
int settings_volume(void);

/* Persist. An empty string erases the key, falling back to the Kconfig seed. */
bool settings_set_id(const char *id);
bool settings_set_url(const char *url);
bool settings_set_volume(int volume);
