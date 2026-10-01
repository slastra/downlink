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

/* Advance one frame and return the gain for it, in Q15. */
static inline int32_t ramp_next(gain_ramp_t *r)
{
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
    return r->cur >> 15;
}

void gain_ramp_apply(gain_ramp_t *r, int16_t *pcm, size_t frames)
{
    if (r->left == 0 && r->cur == Q30(MIXER_UNITY_Q15)) return;
    for (size_t i = 0; i < frames; i++) {
        int32_t g = ramp_next(r);
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

/* ---- the duck ------------------------------------------------------- */

static int32_t db_to_q15(float db)
{
    if (db >= 0) return MIXER_UNITY_Q15;
    return (int32_t)lroundf(32768.0f * powf(10.0f, db / 20.0f));
}

void duck_init(duck_t *d, uint32_t attack_ms, uint32_t tail_ms, uint32_t release_ms, uint32_t rate)
{
    d->phase = DUCK_IDLE;
    d->level_db = d->depth_db = d->rate_db = 0;
    d->attack_frames  = attack_ms  * rate / 1000;
    d->tail_frames    = tail_ms    * rate / 1000;
    d->release_frames = release_ms * rate / 1000;
    if (d->attack_frames == 0)  d->attack_frames = 1;
    if (d->release_frames == 0) d->release_frames = 1;
    d->tail_left = 0;
    gain_ramp_init(&d->ramp, MIXER_UNITY_Q15);
}

void duck_start(duck_t *d, float depth_db)
{
    if (depth_db > 0)   depth_db = 0;
    if (depth_db < -40) depth_db = -40;
    d->depth_db = depth_db;
    if (d->level_db == depth_db) {
        d->phase = DUCK_HELD;
        return;
    }
    /* A full move takes attack_frames; the speed is set by the larger of
     * the depth and where the level is now, so a shallower retarget is
     * not a crawl and a deeper one is not a jump. */
    float span = fmaxf(fabsf(depth_db), fabsf(d->level_db));
    d->rate_db = span / d->attack_frames;
    d->phase = DUCK_ATTACK;
}

bool duck_ready(const duck_t *d)
{
    if (d->phase == DUCK_HELD) return true;
    return d->phase == DUCK_ATTACK && fabsf(d->level_db - d->depth_db) <= 1.0f;
}

void duck_clip_ended(duck_t *d, bool another_ready)
{
    if (another_ready) {
        if (d->phase != DUCK_ATTACK) d->phase = DUCK_HELD;
        return;
    }
    d->phase = DUCK_TAIL;
    d->tail_left = d->tail_frames;
}

static void release_by(duck_t *d, size_t frames)
{
    d->level_db += d->rate_db * frames;
    if (d->level_db >= -1e-4f) {    /* float steps fall a hair short */
        d->level_db = 0;
        d->phase = DUCK_IDLE;
    }
}

void duck_block(duck_t *d, size_t frames)
{
    switch (d->phase) {
    case DUCK_IDLE:
    case DUCK_HELD:
        break;
    case DUCK_ATTACK: {
        float step = d->rate_db * frames;
        if (fabsf(d->depth_db - d->level_db) <= step + 1e-4f) {   /* float steps fall a hair short */
            d->level_db = d->depth_db;
            d->phase = DUCK_HELD;
        } else {
            d->level_db += d->depth_db < d->level_db ? -step : step;
        }
        break;
    }
    case DUCK_TAIL:
        if (d->tail_left > frames) {
            d->tail_left -= (uint32_t)frames;
            break;
        }
        /* The tail ends inside this block: release for the rest of it, so
         * the tail is not a block longer than asked. A full release takes
         * release_frames from the held level. */
        d->rate_db = fabsf(d->level_db) / d->release_frames;
        d->phase = DUCK_RELEASE;
        release_by(d, frames - d->tail_left);
        d->tail_left = 0;
        break;
    case DUCK_RELEASE:
        release_by(d, frames);
        break;
    }
    gain_ramp_set(&d->ramp, db_to_q15(d->level_db), (uint32_t)frames);
}

void duck_apply(duck_t *d, int16_t *music, size_t frames)
{
    gain_ramp_apply(&d->ramp, music, frames);
}

float duck_level_db(const duck_t *d) { return d->level_db; }
duck_phase_t duck_phase(const duck_t *d) { return d->phase; }

/* ---- mixing --------------------------------------------------------- */

static inline int16_t clamp16(int64_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

void mix_block(int16_t *io, const int16_t *clip, size_t frames, gain_ramp_t *announce, gain_ramp_t *master)
{
    for (size_t i = 0; i < frames; i++) {
        int32_t ga = clip ? ramp_next(announce) : 0;
        int32_t gm = ramp_next(master);
        for (int ch = 0; ch < 2; ch++) {
            int64_t s = io[2 * i + ch];
            if (clip) s += ((int64_t)clip[2 * i + ch] * ga + (1 << 14)) >> 15;
            io[2 * i + ch] = clamp16((s * gm + (1 << 14)) >> 15);
        }
    }
}
