/*
 * The output stage: per-sample ramps that land exactly, never click, and
 * never overflow int16; the duck envelope's timing, shape and edge cases;
 * and the mix's clamp and headroom.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mixer.h"

static void test_volume_law(void)
{
    assert(mixer_volume_q15(100) == MIXER_UNITY_Q15);
    assert(mixer_volume_q15(0) == 0);
    assert(abs(mixer_volume_q15(70) - 5827) <= 1);        /* -15 dB */
    for (int v = 1; v <= 100; v++) assert(mixer_volume_q15(v) > mixer_volume_q15(v - 1));
}

/* A ramp moves every frame, by at most its step, monotonically, and lands
 * exactly on the target after the requested number of frames. */
static void test_ramp(int32_t from, int32_t to, uint32_t frames)
{
    gain_ramp_t r;
    gain_ramp_init(&r, from);
    gain_ramp_set(&r, to, frames);
    int16_t pcm[2 * 64];
    int64_t prev = (int64_t)from * 32768;
    int64_t max_step = llabs(((int64_t)to - from) * 32768) / frames + 1;
    uint32_t n = 0;
    while (n < frames + 100) {
        for (int i = 0; i < 64; i++) pcm[2 * i] = pcm[2 * i + 1] = 32767;
        gain_ramp_apply(&r, pcm, 64);
        n += 64;
        if (llabs(r.cur - prev) > max_step * 64) fprintf(stderr, "from %d to %d over %u: frame %u moved %lld > %lld\n", (int)from, (int)to, (unsigned)frames, (unsigned)n, (long long)llabs(r.cur - prev), (long long)(max_step * 64));
        assert(llabs(r.cur - prev) <= max_step * 64);
        assert(to >= from ? r.cur >= prev : r.cur <= prev);
        prev = r.cur;
    }
    assert(r.cur == (int64_t)to * 32768 && r.left == 0);
}

static void test_no_overflow(void)
{
    gain_ramp_t r;
    gain_ramp_init(&r, MIXER_UNITY_Q15);
    int16_t pcm[4] = { 32767, -32768, 32767, -32768 };
    gain_ramp_apply(&r, pcm, 2);                        /* unity is a no-op */
    assert(pcm[0] == 32767 && pcm[1] == -32768);
    gain_ramp_set(&r, MIXER_UNITY_Q15 - 1, 0);
    gain_ramp_apply(&r, pcm, 2);
    assert(pcm[0] == 32766 && pcm[1] == -32767);
}

/* Retargeting mid-ramp continues from where the gain is, with no jump. */
static void test_ramp_retarget(void)
{
    gain_ramp_t r;
    gain_ramp_init(&r, 0);
    gain_ramp_set(&r, MIXER_UNITY_Q15, 2400);
    int16_t pcm[2 * 1200] = { 0 };
    gain_ramp_apply(&r, pcm, 1200);
    int32_t mid = r.cur;
    gain_ramp_set(&r, 0, 2400);
    assert(r.cur == mid);
    gain_ramp_apply(&r, pcm, 1);
    assert(abs(r.cur - mid) <= abs(mid / 2400) + 1);
}

/* ---- the duck ------------------------------------------------------ */

#define RATE  48000
#define BLOCK 480
#define MS(f) ((f) * 1000.0 / RATE)

static duck_t new_duck(void)
{
    duck_t d;
    duck_init(&d, 300, 150, 700, RATE);
    return d;
}

/* Run one block of constant full-scale music through the duck; return the
 * gain (Q15) of each sample via the output. */
static void run_block(duck_t *d, int16_t *gains)
{
    static int16_t pcm[2 * BLOCK];
    for (int i = 0; i < 2 * BLOCK; i++) pcm[i] = 32767;
    duck_block(d, BLOCK);
    duck_apply(d, pcm, BLOCK);
    for (int i = 0; i < BLOCK; i++) gains[i] = pcm[2 * i];
}

static double db_of(int16_t v) { return 20 * log10(v / 32767.0); }

/* Max per-sample change in the gain while ramping: the dB-linear duck is
 * steepest near unity, ~3.6 Q15 units per sample for a 300 ms -14 dB attack. */
#define MAX_GAIN_STEP 5

static void test_attack_hold_release(void)
{
    duck_t d = new_duck();
    int16_t g[BLOCK];
    int16_t prev = 32767;
    uint32_t frames = 0, ready_at = 0, reached_at = 0;

    duck_start(&d, -14);
    assert(!duck_ready(&d));
    while (duck_phase(&d) != DUCK_HELD) {
        run_block(&d, g);
        frames += BLOCK;
        for (int i = 0; i < BLOCK; i++) {
            assert(g[i] <= prev);                              /* monotonic down */
            assert(prev - g[i] <= MAX_GAIN_STEP);              /* no step */
            prev = g[i];
        }
        if (!ready_at && duck_ready(&d)) ready_at = frames;
        assert(frames < RATE);                                 /* it does arrive */
    }
    reached_at = frames;
    assert(fabs(db_of(prev) - -14) < 0.1);
    assert(fabs(MS(reached_at) - 300) <= MS(BLOCK));           /* 300 ms +-1 block */
    /* Ready within 1 dB: 13/14 of the way, ~279 ms, and not before. */
    assert(fabs(MS(ready_at) - 300.0 * 13 / 14) <= MS(BLOCK));

    for (int b = 0; b < 200; b++) {                            /* HELD: flat */
        run_block(&d, g);
        for (int i = 0; i < BLOCK; i++) assert(g[i] == prev);
    }

    duck_clip_ended(&d, false);
    frames = 0;
    uint32_t rise_from = 0, released_at = 0;
    const int16_t held = prev;
    while (duck_phase(&d) != DUCK_IDLE) {
        run_block(&d, g);
        for (int i = 0; i < BLOCK; i++) {
            assert(g[i] >= prev);                              /* monotonic up */
            assert(g[i] - prev <= MAX_GAIN_STEP);
            if (!rise_from && g[i] > held) rise_from = frames + i;   /* the tail's end */
            prev = g[i];
        }
        frames += BLOCK;
        assert(frames < 2 * RATE);
    }
    released_at = frames;
    assert(prev == 32767);
    assert(fabs(MS(rise_from) - 150) <= MS(BLOCK));            /* tail 150 ms +-1 block */
    assert(fabs(MS(released_at) - 150 - 700) <= MS(BLOCK));    /* then 700 ms of release */
}

static void test_back_to_back(void)
{
    duck_t d = new_duck();
    int16_t g[BLOCK];
    duck_start(&d, -14);
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    duck_clip_ended(&d, true);                                 /* the next clip is ready */
    assert(duck_phase(&d) == DUCK_HELD && duck_ready(&d));
    duck_start(&d, -14);                                       /* ...and it starts */
    assert(duck_ready(&d));
    run_block(&d, g);
    assert(fabs(duck_level_db(&d) - -14) < 1e-3);
}

/* A clip arriving mid-release goes back down from where the level is. */
static void test_retarget(void)
{
    duck_t d = new_duck();
    int16_t g[BLOCK];
    duck_start(&d, -14);
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    duck_clip_ended(&d, false);
    while (duck_phase(&d) != DUCK_RELEASE || duck_level_db(&d) < -7) run_block(&d, g);
    int16_t prev = g[BLOCK - 1];
    duck_start(&d, -14);
    uint32_t frames = 0;
    while (!duck_ready(&d)) {
        run_block(&d, g);
        frames += BLOCK;
        for (int i = 0; i < BLOCK; i++) {
            assert(abs(g[i] - prev) <= MAX_GAIN_STEP);         /* no jump at the turn */
            prev = g[i];
        }
    }
    /* ~6 dB to cover at the full-duck rate: well under a full attack. */
    assert(MS(frames) < 250);

    /* A deeper duck while held, and a shallower one. */
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    duck_start(&d, -20);
    assert(duck_phase(&d) == DUCK_ATTACK);
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    assert(fabs(duck_level_db(&d) - -20) < 1e-3);
    duck_start(&d, -6);
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    assert(fabs(duck_level_db(&d) - -6) < 1e-3);
}

static void test_edges(void)
{
    duck_t d = new_duck();
    int16_t g[BLOCK];
    duck_start(&d, 0);                                         /* no duck at all */
    assert(duck_ready(&d) && duck_phase(&d) == DUCK_HELD);
    run_block(&d, g);
    assert(g[0] == 32767 && g[BLOCK - 1] == 32767);
    duck_clip_ended(&d, false);
    for (int b = 0; b < 20; b++) run_block(&d, g);
    assert(duck_phase(&d) == DUCK_IDLE && g[BLOCK - 1] == 32767);

    duck_start(&d, -60);                                       /* clamped to -40 */
    while (duck_phase(&d) != DUCK_HELD) run_block(&d, g);
    assert(fabs(duck_level_db(&d) - -40) < 1e-3);
    run_block(&d, g);
    assert(abs(g[0] - 328) <= 1);                              /* -40 dB of 32767 */
}

/* A full-scale 1 kHz sine through a whole duck cycle: the envelope must add
 * no discontinuity, so every output step stays within the sine's own. */
static void test_no_click_signal(void)
{
    duck_t d = new_duck();
    static int16_t pcm[2 * BLOCK];
    double phase = 0, w = 2 * M_PI * 1000 / RATE;
    int16_t last = 0;
    int max_step = 0;
    const int sine_step = (int)ceil(32767 * w);                /* ~4289 */
    for (int b = 0; b < 400; b++) {                            /* 4 s */
        if (b == 10) duck_start(&d, -14);
        if (b == 150) duck_clip_ended(&d, false);
        for (int i = 0; i < BLOCK; i++, phase += w) pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lround(32767 * sin(phase));
        duck_block(&d, BLOCK);
        duck_apply(&d, pcm, BLOCK);
        for (int i = 0; i < BLOCK; i++) {
            int step = abs(pcm[2 * i] - last);
            if (step > max_step) max_step = step;
            last = pcm[2 * i];
        }
    }
    assert(max_step <= sine_step + 8);
}

static void test_mix(void)
{
    gain_ramp_t unity, half, ann;
    gain_ramp_init(&unity, MIXER_UNITY_Q15);
    gain_ramp_init(&half, MIXER_UNITY_Q15 / 2);
    gain_ramp_init(&ann, MIXER_UNITY_Q15);

    int16_t io[4] = { 32767, -32768, 20000, -20000 }, clip[4] = { 32767, -32768, 20000, -20000 };
    mix_block(io, clip, 2, &ann, &unity);                      /* full scale + full scale */
    assert(io[0] == 32767 && io[1] == -32768 && io[2] == 32767 && io[3] == -32768);

    int16_t a[4] = { 20000, -20000, 1000, -1 }, c[4] = { 10000, -10000, 0, 0 };
    mix_block(a, c, 2, &ann, &half);                           /* (m + c) * 0.5 */
    assert(a[0] == 15000 && a[1] == -15000 && a[2] == 500 && a[3] == 0);   /* -0.5 rounds half up */

    /* No clip: exactly the master ramp alone. */
    int16_t x[2 * 64], y[2 * 64];
    for (int i = 0; i < 128; i++) x[i] = y[i] = (int16_t)(i * 517 - 30000);
    gain_ramp_t m1, m2;
    gain_ramp_init(&m1, 20000); gain_ramp_set(&m1, 5000, 50);
    gain_ramp_init(&m2, 20000); gain_ramp_set(&m2, 5000, 50);
    gain_ramp_apply(&m1, x, 64);
    mix_block(y, NULL, 64, &ann, &m2);
    assert(!memcmp(x, y, sizeof x));

    /* The announce gain applies to the clip only. */
    gain_ramp_t g85;
    gain_ramp_init(&g85, mixer_volume_q15(85));
    int16_t mu[2] = { 0, 1000 }, cl[2] = { 10000, 0 };
    mix_block(mu, cl, 1, &g85, &unity);
    assert(abs(mu[0] - (int)lround(10000 * pow(10, -7.5 / 20))) <= 1 && mu[1] == 1000);
}

/* Music ducked 14 dB plus a -1.5 dBTP clip at announce volume 85, under a
 * unity master, never reaches the clamp. */
static void test_headroom(void)
{
    gain_ramp_t unity, g85;
    gain_ramp_init(&unity, MIXER_UNITY_Q15);
    gain_ramp_init(&g85, mixer_volume_q15(85));
    int16_t music = (int16_t)lround(32767 * pow(10, -14 / 20.0));
    int16_t clip  = (int16_t)lround(32767 * pow(10, -1.5 / 20.0));
    int16_t io[2] = { music, (int16_t)-music }, c[2] = { clip, (int16_t)-clip };
    mix_block(io, c, 1, &g85, &unity);
    assert(io[0] < 32767 && io[1] > -32768);
    assert(io[0] < 0.6 * 32767);                               /* comfortably */
}

int main(void)
{
    test_volume_law();
    test_ramp(0, MIXER_UNITY_Q15, 2400);
    test_ramp(MIXER_UNITY_Q15, 0, 2400);
    test_ramp(5827, 20000, 2400);
    test_ramp(20000, 5827, 7);
    test_ramp(100, 101, 2400);                         /* slower than one Q15 step per frame */
    test_no_overflow();
    test_ramp_retarget();
    printf("mixer ramps: ok\n");
    test_attack_hold_release();
    test_back_to_back();
    test_retarget();
    test_edges();
    test_no_click_signal();
    test_mix();
    test_headroom();
    printf("duck and mix: ok\n");
    return 0;
}
