/*
 * The buffer-depth controller against simulated clocks, in audio time (as
 * the player measures it, from Ogg granules): a source feeding the ring
 * while the DAC runs r ppm fast or slow, with clumpy arrival and a page of
 * measurement error, for three simulated days per case. Plus the case that
 * broke the byte-based version: joining during silence, when Icecast's
 * byte-sized burst is minutes of audio.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drift.h"

#define FS         48000.0
#define PACKET     960                  /* 20 ms Opus frames */
#define PAGE       4800                 /* 100 ms: the depth measurement's grain */
#define TARGET     (4 * 48000)
#define CATCHUP    (12 * 48000)
#define PREBUFFER  (2 * 48000)
#define DECODE_X   12.0                 /* catch-up decodes this much faster than real time */

static void check_apply(void)
{
    int16_t pcm[2 * 12];
    drift_t d;
    drift_init(&d, TARGET, CATCHUP);

    for (int m = -1; m <= 1; m += 2) {
        for (int i = 0; i < 10; i++) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(i * 100 + (i % 2));
        d.mode = m;
        d.acc = DRIFT_INTERVAL_FRAMES - 10;          /* due on this packet */
        size_t n = drift_apply(&d, pcm, 10, 12);
        int want_drop[] = { 0, 1, 2, 3, 4, 6, 7, 8, 9 };
        int want_rep[]  = { 0, 1, 2, 3, 4, 5, 5, 6, 7, 8, 9 };
        int *want = m > 0 ? want_drop : want_rep;
        assert(n == (m > 0 ? 9u : 11u));
        for (size_t i = 0; i < n; i++)          /* order kept, L/R still paired */
            assert(pcm[2 * i] == want[i] * 100 + (want[i] % 2) && pcm[2 * i] == pcm[2 * i + 1]);
        assert(d.acc == 0);
    }
    d.mode = -1;
    d.acc = DRIFT_INTERVAL_FRAMES;
    assert(drift_apply(&d, pcm, 12, 12) == 12);  /* never past capacity */
}

/* `burst_s`: audio Icecast hands over on connect (4 s at 128 kbps; minutes
 * during silence). Playback starts at the prebuffer while it lands. */
static void simulate(double ppm, double burst_s, double days)
{
    drift_t d;
    drift_init(&d, TARGET, CATCHUP);
    srand(1234);

    double depth = PREBUFFER, burst = burst_s * FS - PREBUFFER, pending = 0, t = 0;
    double lo = 1e18, hi = -1e18, settle_s = 0;
    long underruns = 0;
    while (t < days * 86400.0) {
        int64_t seen = (int64_t)depth - rand() % PAGE;          /* a page of measurement grain */
        bool skipping = drift_catching_up(&d, seen < 0 ? 0 : seen);
        double dt;
        if (skipping) {
            drift_skipped(&d, PACKET);
            dt = PACKET / FS / DECODE_X;
        } else {
            drift_update(&d, seen < 0 ? 0 : seen, PACKET);
            size_t out = drift_apply(&d, (int16_t[2 * (PACKET + 1)]){0}, PACKET, PACKET + 1);
            dt = out / (FS * (1 + ppm * 1e-6));                 /* the DAC's clock */
        }
        t += dt;
        depth -= PACKET;                                         /* one packet of audio consumed */
        pending += FS * dt;                                      /* the source's clock */
        if (burst > 0) { double b = fmin(burst, FS * 4 * dt * 60); pending += b; burst -= b; }
        if (pending > 2 * PACKET || rand() % 4 == 0) { depth += pending; pending = 0; }   /* clumps */
        if (depth < 0) { underruns++; depth = 0; }
        if (!skipping && !d.catching_up && settle_s == 0 && fabs(depth - TARGET) < FS) settle_s = t;
        if (t > 600) { lo = fmin(lo, depth); hi = fmax(hi, depth); }
    }
    double expect = fabs(ppm) * 1e-6 * days * 86400.0 * FS / DRIFT_INTERVAL_FRAMES * DRIFT_INTERVAL_FRAMES;
    double got = d.dropped + d.repeated;
    printf("  %+5.0f ppm, %5.0f s burst: depth %4.2f..%4.2f s (target %.0f)  catchups %u skipped %5.1f s  "
           "dropped %7u repeated %7u (expected ~%.0f)\n",
           ppm, burst_s, lo / FS, hi / FS, TARGET / FS, d.catchups, d.skipped / FS, d.dropped, d.repeated, expect);
    assert(underruns == 0);
    assert(lo > TARGET - 1.0 * FS && hi < TARGET + 1.0 * FS);   /* held to within a second */
    if (burst_s > CATCHUP / FS + 2) {
        assert(d.catchups == 1);                                 /* one jump, then steady */
        /* The backlog, plus what keeps arriving in real time while the
         * catch-up runs at DECODE_X. */
        double want = (burst_s - TARGET / FS) * DECODE_X / (DECODE_X - 1);
        assert(fabs(d.skipped / FS - want) < 3);
    } else {
        assert(d.catchups == 0);
    }
    if (ppm > 0) assert(d.dropped == 0);
    if (ppm < 0) assert(d.repeated == 0);
    if (ppm != 0) assert(fabs(got - expect) < 0.05 * expect + 50);
    else assert(got < 50);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    check_apply();
    printf("drift_apply: ok\n");
    double cases[] = { -200, -100, -50, 0, 50, 100, 200 };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) simulate(cases[i], 4, 3);
    simulate(0, 170, 1);      /* joined during silence: 64 KB of 3 kbps Opus */
    simulate(100, 170, 1);
    printf("drift: simulations passed\n");
    return 0;
}
