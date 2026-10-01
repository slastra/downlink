#include "mixer.h"

#include <math.h>

#define Q30(q15) ((int32_t)(q15) << 15)

void gain_ramp_init(gain_ramp_t *r, int32_t q15)
{
    r->cur = r->target = Q30(q15);
    r->step = r->rem = r->err = 0;
    r->span = r->left = 0;
}

void gain_ramp_set(gain_ramp_t *r, int32_t q15, uint32_t frames)
{
    r->target = Q30(q15);
    if (frames == 0 || r->cur == r->target) {
        r->cur = r->target;
        r->left = 0;
        return;
    }
    int32_t diff = r->target - r->cur;
    r->step = diff / (int32_t)frames;
    r->rem = diff % (int32_t)frames;      /* same sign as diff */
    r->err = 0;
    r->span = frames;
    r->left = frames;
}

int32_t gain_ramp_target_q15(const gain_ramp_t *r)
{
    return r->target >> 15;
}

void gain_ramp_apply(gain_ramp_t *r, int16_t *pcm, size_t frames)
{
    if (r->left == 0 && r->cur == Q30(MIXER_UNITY_Q15)) return;
    for (size_t i = 0; i < frames; i++) {
        if (r->left) {
            /* Exact linear interpolation without a division per frame: the
             * truncated step, plus one unit whenever the remainder adds up,
             * so the ramp lands on the target with no jump at the end. */
            r->cur += r->step;
            r->err += r->rem;
            if (r->err >= (int32_t)r->span)       { r->cur++; r->err -= (int32_t)r->span; }
            else if (r->err <= -(int32_t)r->span) { r->cur--; r->err += (int32_t)r->span; }
            if (--r->left == 0) r->cur = r->target;
        }
        int32_t g = r->cur >> 15;
        pcm[2 * i]     = (int16_t)((pcm[2 * i] * g + (1 << 14)) >> 15);
        pcm[2 * i + 1] = (int16_t)((pcm[2 * i + 1] * g + (1 << 14)) >> 15);
    }
}

int32_t mixer_volume_q15(int volume_percent)
{
    if (volume_percent <= 0) return 0;
    if (volume_percent >= 100) return MIXER_UNITY_Q15;
    return (int32_t)lroundf(32768.0f * powf(10.0f, (volume_percent - 100) * 0.5f / 20.0f));
}
