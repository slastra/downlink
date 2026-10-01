/*
 * Buffer-depth control: clock drift and backlog.
 *
 * Everything here is in samples (48 kHz), measured from Ogg granule
 * positions, never in bytes. Opus is variable-bitrate: a quiet passage
 * drops from ~130 kbps to ~3 kbps, so a byte count says nothing about how
 * much audio is buffered. (A byte-based first version prebuffered 85 s of
 * silence and then fought every change in loudness.)
 *
 * Two jobs, both holding the buffer at a fixed depth:
 *
 *  - Drift. The source's sound card and this board's I2S clock differ by
 *    tens of ppm; uncorrected that is a rebuffer every 10-40 hours, or a
 *    backlog that grows until Icecast drops us. When a 30 s average of the
 *    depth leaves target +/- 0.5 s, drop (too deep: play faster) or repeat
 *    (too shallow: play slower) one stereo frame every 50 ms -- ~400 ppm,
 *    twice the worst plausible drift, one 21 us sample at a time, which is
 *    not audible -- until it is back within 0.125 s.
 *
 *  - Backlog. Icecast's connect burst is sized in bytes too: joining during
 *    a silent passage can hand us minutes of audio at once. Above the
 *    catch-up limit the player decodes and discards until the depth is
 *    back at target -- one jump, almost always through silence, instead of
 *    staying minutes behind live and overflowing the ring when it gets loud.
 *
 * Pure C with no ESP dependencies, so test/host can run it over simulated
 * days.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRIFT_RATE             48000
#define DRIFT_SETTLE_FRAMES    (DRIFT_RATE * 5)     /* ignored after a reset: the burst is landing */
#define DRIFT_TAU_FRAMES       (DRIFT_RATE * 30)    /* averaging time constant */
#define DRIFT_BAND_FRAMES      (DRIFT_RATE / 2)     /* start correcting beyond 0.5 s */
#define DRIFT_INTERVAL_FRAMES  2400                 /* one correction per 50 ms: ~417 ppm */

typedef struct {
    int64_t  target;          /* depth to hold, samples */
    int64_t  catchup_above;   /* depth that triggers a catch-up, samples */
    double   avg;             /* smoothed depth */
    bool     have_avg;
    uint32_t settle;          /* frames seen since the reset */
    int      mode;            /* +1 drop, -1 repeat, 0 idle */
    uint32_t acc;             /* frames since the last correction */
    bool     catching_up;
    uint32_t dropped, repeated;
    uint32_t catchups;
    uint64_t skipped;         /* frames discarded catching up */
} drift_t;

void drift_init(drift_t *d, int64_t target, int64_t catchup_above);

/* Forget the average: after a reconnect or a rebuffer the depth starts
 * over. Keeps the counters. */
void drift_reset(drift_t *d);

/* Feed the current depth once per decoded packet of `frames`, whenever the
 * depth is known. */
void drift_update(drift_t *d, int64_t depth, size_t frames);

/* True while a catch-up is under way: the caller discards decoded audio
 * (and reports the frames with drift_skipped). Starts above catchup_above,
 * ends at target. */
bool drift_catching_up(drift_t *d, int64_t depth);
void drift_skipped(drift_t *d, size_t frames);

/* Apply any due correction to interleaved stereo `pcm` holding `frames`
 * (capacity `cap` frames). Returns the new frame count. */
size_t drift_apply(drift_t *d, int16_t *pcm, size_t frames, size_t cap);

#ifdef __cplusplus
}
#endif
