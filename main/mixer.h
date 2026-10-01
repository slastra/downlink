/*
 * Output-stage gain and mixing, in plain C with no ESP-IDF dependencies so
 * test/host can check it (like drift.c).
 *
 * Every gain change is a per-sample linear ramp. A gain that steps once per
 * block is audible as zipper noise or a click -- ESPHome shipped both, an
 * instant duck and an unramped volume, before fixing them.
 *
 * Per block the output stage does: duck_block() and duck_apply() on the
 * music, then mix_block() to add the announcement and apply the master
 * volume, clamping once on the whole sum.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIXER_UNITY_Q15 32768

/* A Q15 gain (0..32768) that moves to its target linearly, one frame at a
 * time. Held internally in Q30 so slow ramps still move every frame. */
typedef struct {
    int32_t  cur;       /* Q30 */
    int32_t  target;    /* Q30 */
    int32_t  step;      /* Q30 per frame, truncated */
    int32_t  rem;       /* the division's remainder, spread Bresenham-style */
    int32_t  err;
    uint32_t span;      /* the ramp's length in frames */
    uint32_t left;      /* frames until cur == target */
} gain_ramp_t;

void gain_ramp_init(gain_ramp_t *r, int32_t q15);
/* Head for `q15` over `frames` (0 = jump). Continues from the current level. */
void gain_ramp_set(gain_ramp_t *r, int32_t q15, uint32_t frames);
int32_t gain_ramp_target_q15(const gain_ramp_t *r);
/* Scale interleaved stereo in place. */
void gain_ramp_apply(gain_ramp_t *r, int16_t *pcm, size_t frames);

/* The 0.5 dB-per-step volume law, 100 = unity, 0 = silence. */
int32_t mixer_volume_q15(int volume_percent);

/*
 * The duck: the music's gain while an announcement plays.
 *
 *   IDLE -> ATTACK -> HELD -> TAIL -> RELEASE -> IDLE
 *
 * The level moves linearly in dB (natural to the ear), stepped once per
 * block; within a block the gain is interpolated per sample, so there are
 * no steps to hear. Moves are rate-based: a full duck takes attack_ms down
 * and release_ms back up, and a partial move (a new clip arriving mid
 * release, a deeper duck) takes proportionally less, always from the
 * current level.
 *
 * The clip waits for duck_ready() -- within 1 dB of the target -- so the
 * first word lands on music that is already down. After the clip, TAIL
 * holds the duck a moment so the last word isn't swamped, then RELEASE.
 * With another clip ready, the music stays down instead (no pumping).
 */
typedef enum { DUCK_IDLE, DUCK_ATTACK, DUCK_HELD, DUCK_TAIL, DUCK_RELEASE } duck_phase_t;

typedef struct {
    duck_phase_t phase;
    float    level_db;        /* at the last block boundary; 0 = unity */
    float    depth_db;        /* the target while ducking, <= 0 */
    float    rate_db;         /* dB per frame for the current move */
    uint32_t attack_frames, tail_frames, release_frames;
    uint32_t tail_left;
    gain_ramp_t ramp;         /* the per-sample gain within a block */
} duck_t;

void duck_init(duck_t *d, uint32_t attack_ms, uint32_t tail_ms, uint32_t release_ms, uint32_t rate);
/* A clip is ready: head for depth_db (clamped to -40..0) from wherever the
 * level is. */
void duck_start(duck_t *d, float depth_db);
/* True once the first sample of a clip may play. */
bool duck_ready(const duck_t *d);
/* The clip finished. With another ready, stay down; else tail, release. */
void duck_clip_ended(duck_t *d, bool another_ready);
/* Advance one block of `frames`; call before duck_apply on that block. */
void duck_block(duck_t *d, size_t frames);
/* Apply this block's gain to interleaved stereo music, in place. */
void duck_apply(duck_t *d, int16_t *music, size_t frames);
float duck_level_db(const duck_t *d);
duck_phase_t duck_phase(const duck_t *d);

/*
 * io = clamp(master * (io + clip * announce)), per sample, interleaved
 * stereo. `io` is the (ducked) music; `clip` may be NULL. Summed in 64-bit
 * and clamped once to int16, so a full-scale sum saturates instead of
 * wrapping. With no clip it is exactly the master ramp alone.
 */
void mix_block(int16_t *io, const int16_t *clip, size_t frames, gain_ramp_t *announce, gain_ramp_t *master);

#ifdef __cplusplus
}
#endif
