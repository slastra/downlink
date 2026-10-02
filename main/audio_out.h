/*
 * I2S output to a PCM5102A: Philips format, stereo, 16-bit samples in
 * 32-bit slots (BCK = 64 fs). SCK is grounded on the DAC, so no MCLK: the
 * PCM5102A derives its clock from BCK with its internal PLL.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio queued in the DMA ahead of the DAC: a sample written now is heard
 * this many frames later. */
#define AUDIO_OUT_DMA_DESC   8
#define AUDIO_OUT_DMA_FRAMES_PER_DESC 480
#define AUDIO_OUT_DMA_FRAMES (AUDIO_OUT_DMA_DESC * AUDIO_OUT_DMA_FRAMES_PER_DESC)

void audio_out_init(uint32_t sample_rate);
/* Interleaved L/R frames. Blocks until the DMA has room. */
void audio_out_write(const int16_t *frames, size_t nframes);

#ifdef __cplusplus
}
#endif
