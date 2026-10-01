/*
 * The output-stage gain: per-sample ramps that land exactly, never click,
 * and never overflow int16.
 */
#include <assert.h>
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
static void test_retarget(void)
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

int main(void)
{
    test_volume_law();
    test_ramp(0, MIXER_UNITY_Q15, 2400);
    test_ramp(MIXER_UNITY_Q15, 0, 2400);
    test_ramp(5827, 20000, 2400);
    test_ramp(20000, 5827, 7);
    test_ramp(100, 101, 2400);                         /* slower than one Q15 step per frame */
    test_no_overflow();
    test_retarget();
    printf("mixer: ok\n");
    return 0;
}
