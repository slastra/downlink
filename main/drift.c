#include "drift.h"

#include <string.h>

void drift_init(drift_t *d, int64_t target, int64_t catchup_above)
{
    memset(d, 0, sizeof *d);
    d->target = target;
    d->catchup_above = catchup_above;
}

void drift_reset(drift_t *d)
{
    d->have_avg = false;
    d->settle = 0;
    d->mode = 0;
    d->acc = 0;
}

void drift_update(drift_t *d, int64_t depth, size_t frames)
{
    if (d->catching_up) return;
    /* Playback starts at the prebuffer while the rest of Icecast's burst is
     * still arriving; seeding the average there would read as "too
     * shallow" for the next minute. */
    if (d->settle < DRIFT_SETTLE_FRAMES) {
        d->settle += frames;
        return;
    }
    if (!d->have_avg) {
        d->avg = (double)depth;
        d->have_avg = true;
    } else {
        double a = (double)frames / DRIFT_TAU_FRAMES;
        if (a > 1) a = 1;
        d->avg += ((double)depth - d->avg) * a;
    }

    double err = d->avg - (double)d->target;
    if (d->mode == 0) {
        if (err > DRIFT_BAND_FRAMES)       d->mode = +1;
        else if (err < -DRIFT_BAND_FRAMES) d->mode = -1;
    } else if (err < DRIFT_BAND_FRAMES / 4 && err > -DRIFT_BAND_FRAMES / 4) {
        d->mode = 0;
    }
}

bool drift_catching_up(drift_t *d, int64_t depth)
{
    if (!d->catching_up && depth > d->catchup_above) {
        d->catching_up = true;
        d->catchups++;
    } else if (d->catching_up && depth <= d->target) {
        d->catching_up = false;
        drift_reset(d);   /* the average from before the jump means nothing now */
    }
    return d->catching_up;
}

void drift_skipped(drift_t *d, size_t frames)
{
    d->skipped += frames;
}

size_t drift_apply(drift_t *d, int16_t *pcm, size_t frames, size_t cap)
{
    if (d->mode == 0 || frames < 2) {
        d->acc = 0;
        return frames;
    }
    d->acc += frames;
    if (d->acc < DRIFT_INTERVAL_FRAMES) return frames;
    d->acc -= DRIFT_INTERVAL_FRAMES;

    /* Mid-packet, where a one-sample step is buried in the signal. */
    size_t at = frames / 2;
    if (d->mode > 0) {
        memmove(pcm + at * 2, pcm + (at + 1) * 2, (frames - at - 1) * 2 * sizeof *pcm);
        d->dropped++;
        return frames - 1;
    }
    if (frames >= cap) return frames;
    memmove(pcm + (at + 1) * 2, pcm + at * 2, (frames - at) * 2 * sizeof *pcm);
    d->repeated++;
    return frames + 1;
}
