/*
 * Status LED: one WS2812B (GPIO48 on both the N16R8 and the SuperMini).
 *
 * Everything runs on the esp_timer task under one lock; the led_strip RMT
 * backend is not safe against overlapping refreshes.
 */
#pragma once
#include <stdint.h>

void led_init(void);

/* Steady colours, at the configured brightness. The base colour is what the
 * LED returns to after a flash or a pulse ends. */
void led_base(uint8_t r, uint8_t g, uint8_t b);

/* Brief flash of a colour, then back to base. */
void led_flash(uint8_t r, uint8_t g, uint8_t b);

/* Alternate colour/off at `period_ms` until led_base() is called. */
void led_pulse(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms);

/* downlink's vocabulary, in priority order (main.c picks the first that
 * applies). Colours follow tspl-station where the meaning overlaps. */
#define LED_PROVISION()        led_pulse(255, 0, 255, 500)  /* magenta pulse: portal up */
#define LED_PROVISION_CLIENT() led_base(255, 0, 255)        /* magenta: someone is on it */
#define LED_NO_WIFI()          led_base(255, 0, 0)          /* red */
#define LED_WRONG_CODEC()      led_pulse(255, 0, 0, 1000)   /* slow red pulse: mount is not Opus */
#define LED_CONNECTING()       led_pulse(255, 255, 0, 1000) /* yellow pulse: stream not connected */
#define LED_BUFFERING()        led_base(0, 0, 255)          /* blue: filling the ring */
#define LED_PLAYING()          led_base(0, 255, 0)          /* green */
