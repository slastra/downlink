/*
 * Output-stage gain and mixing, in plain C with no ESP-IDF dependencies so
 * test/host can check it (like drift.c).
 *
 * Every gain change is a per-sample linear ramp. A gain that steps once per
 * block is audible as zipper noise or a click -- ESPHome shipped both, an
 * instant duck and an unramped volume, before fixing them.
 */
#pragma once
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

#ifdef __cplusplus
}
#endif
