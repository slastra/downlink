/*
 * Announcements: a short Ogg Opus clip, downloaded on command and played
 * over the music, which ducks under it and comes back after.
 *
 *   MQTT task  -- announce_enqueue() -->  job table  <-- fetch task (core 0)
 *   player task -- announce_process() each 10 ms block: queue, duck, clip
 *   main task   -- announce_wait_event() --> downlink/<id>/announce
 *
 * The fetch task downloads each clip as soon as it is queued (the URLs are
 * signed and expire), into 16 KB PSRAM chunks under a total budget, and
 * checks it is Ogg Opus before anything can reach the DAC. The player task
 * plays them in order on a second decoder, mixing at the same output block
 * as the music, so a clip plays whether or not there is music and carries
 * on through a stream reconnect. It never builds JSON or publishes: events
 * are fixed-size structs on a queue the main task drains.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ANN_DOWNLOADING, ANN_PLAYING, ANN_DONE, ANN_FAILED } announce_state_t;

typedef struct {
    char             id[33];
    announce_state_t state;
    uint32_t         duration_ms;   /* with ANN_PLAYING */
    char             error[48];     /* with ANN_FAILED */
} announce_event_t;

typedef struct {
    char     playing_id[33];        /* "" when none */
    uint32_t played, failed;        /* since boot */
    uint32_t queued;                /* jobs now: fetching, ready or playing */
} announce_stats_t;

/* Before player_start(). */
void announce_init(void);

/* MQTT task. Queues a clip; returns its position (jobs ahead + 1), or 0 if
 * the queue is full. volume 0-100 and duck_db -40..0 are clamped. */
int announce_enqueue(const char *id, const char *url, int volume, int duck_db);

/* Player task, once per output block, before mixing: moves the queue
 * along, ducks `music` in place, and returns the clip's PCM for this block
 * (zero-padded) or NULL, with *gain set to the clip's gain ramp. */
const int16_t *announce_process(int16_t *music, size_t frames, gain_ramp_t **gain);

/* Player task, after each I2S write: total frames written so far. Events
 * are released when their sample has actually left the DAC. */
void announce_written(uint64_t frames_written);

/* True while a clip is sounding (the music source then discards less per
 * block during a catch-up). */
bool announce_sounding(void);
/* Average clip decode time per block while one sounds (for the status). */
uint32_t announce_clip_avg_us(void);

/* Main task: the next event, waiting up to `wait`. */
bool announce_wait_event(announce_event_t *e, TickType_t wait);

void announce_get_stats(announce_stats_t *out);
const char *announce_state_name(announce_state_t s);

#ifdef __cplusplus
}
#endif
