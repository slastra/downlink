/*
 * Decode side: drains the stream ring, decodes Ogg Opus, applies volume,
 * writes I2S. Runs on core 1.
 *
 * Buffering policy: nothing plays until the ring holds DL_PREBUFFER_KB;
 * an empty ring mid-play is an underrun (the DMA auto-clears to silence)
 * and sends the player back to buffering.
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLAYER_BUFFERING, PLAYER_PLAYING, PLAYER_SKIPPING } player_state_t;

void player_start(int volume_percent);
player_state_t player_state(void);
void player_status(char *out, size_t len);

#ifdef __cplusplus
}
#endif
