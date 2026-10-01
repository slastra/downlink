/*
 * Decode side: drains the stream ring, decodes Ogg Opus, applies volume,
 * writes I2S. Runs on core 1.
 *
 * Buffering policy, all in audio time (see drift.h): nothing plays until
 * DL_PREBUFFER_MS is buffered; an empty ring mid-play is an underrun (the
 * DMA auto-clears to silence) and sends the player back to buffering; the
 * depth is then held at DL_BUFFER_MS against clock drift, with a catch-up
 * above DL_BUFFER_MAX_MS.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLAYER_BUFFERING, PLAYER_PLAYING, PLAYER_SKIPPING } player_state_t;

void player_start(int volume_percent);
player_state_t player_state(void);
void player_set_volume(int volume_percent);

typedef struct {
    player_state_t state;
    uint32_t underruns, resets, errors;
    uint32_t played_s;        /* seconds of audio written to the DAC since boot */
    int      volume;
    int32_t  depth_ms;        /* audio buffered, -1 when not measurable right now */
    /* buffer-depth control (drift.h) */
    int      drift_mode;      /* 2 catching up, +1 dropping, -1 repeating, 0 idle */
    uint32_t drift_dropped, drift_repeated;
    uint32_t catchups, skipped_s;
} player_stats_t;
void player_get_stats(player_stats_t *out);
void player_status(char *out, size_t len);

#ifdef __cplusplus
}
#endif
