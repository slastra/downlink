/*
 * The audio task on core 1: a 10 ms output loop (the blocking I2S write is
 * the clock) that pulls music from the stream ring, decodes Ogg Opus,
 * applies the volume ramp and writes the DAC. Sources hand over what they
 * have and the rest is silence, so nothing upstream can stall the output.
 *
 * Buffering policy, all in audio time (see drift.h): nothing plays until
 * DL_PREBUFFER_MS is buffered; an empty ring mid-play is an underrun (the
 * DMA auto-clears to silence) and sends the player back to buffering; the
 * depth is then held at DL_BUFFER_MS against clock drift, with a catch-up
 * above DL_BUFFER_MAX_MS.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLAYER_BUFFERING, PLAYER_PLAYING, PLAYER_SKIPPING } player_state_t;

void player_start(int volume_percent);
player_state_t player_state(void);
void player_set_volume(int volume_percent);
/* Fade the whole output out (true) or back to the volume (false) over
 * PLAYER_HOLD_FADE_MS. For a firmware download: its flash erases stall
 * decoding, and silence beats a stutter. */
#define PLAYER_HOLD_FADE_MS 500
void player_set_hold(bool hold);

typedef struct {
    player_state_t state;
    uint32_t underruns, resets, errors;
    uint32_t played_s;        /* seconds of music written to the DAC since boot */
    uint32_t progress;        /* frames played or deliberately discarded (wraps): the supervisor's pulse */
    int      volume;
    int32_t  depth_ms;        /* audio buffered, -1 when not measurable right now */
    /* buffer-depth control (drift.h) */
    int      drift_mode;      /* 2 catching up, +1 dropping, -1 repeating, 0 idle */
    uint32_t drift_dropped, drift_repeated;
    uint32_t catchups, skipped_s;
    uint32_t block_max_us;    /* worst decode+mix time per 10 ms block, last 30 s */
    uint32_t block_avg_us;
} player_stats_t;
void player_get_stats(player_stats_t *out);
void player_status(char *out, size_t len);

#ifdef __cplusplus
}
#endif
