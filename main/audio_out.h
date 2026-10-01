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

void audio_out_init(uint32_t sample_rate);
/* Interleaved L/R frames. Blocks until the DMA has room. */
void audio_out_write(const int16_t *frames, size_t nframes);

#ifdef __cplusplus
}
#endif
