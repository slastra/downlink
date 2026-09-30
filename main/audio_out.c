#include "audio_out.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "audio_out";

static i2s_chan_handle_t s_tx;
static uint32_t          s_rate;

void audio_out_init(uint32_t sample_rate)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 8 x 480 frames = 80 ms of DMA at 48 kHz: enough slack for a WiFi
     * burst on the other core, short enough that volume changes land fast. */
    cc.dma_desc_num = 8;
    cc.dma_frame_num = 480;
    /* An underrun plays silence instead of looping the last buffer. */
    cc.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_DL_I2S_BCK_GPIO,
            .ws   = CONFIG_DL_I2S_WS_GPIO,
            .dout = CONFIG_DL_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    /* 64 fs is the PCM5102A's most comfortable BCK ratio; with 16-bit data
     * the low half of each slot is zero, which the DAC reads correctly. */
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    sc.slot_cfg.ws_width = 32;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    s_rate = sample_rate;
    ESP_LOGI(TAG, "I2S up: %lu Hz, BCK=%d WS=%d DOUT=%d", (unsigned long)sample_rate,
             CONFIG_DL_I2S_BCK_GPIO, CONFIG_DL_I2S_WS_GPIO, CONFIG_DL_I2S_DOUT_GPIO);
}

void audio_out_set_rate(uint32_t sample_rate)
{
    if (sample_rate == s_rate || sample_rate == 0) return;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx, &clk));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_LOGI(TAG, "rate %lu -> %lu Hz", (unsigned long)s_rate, (unsigned long)sample_rate);
    s_rate = sample_rate;
}

void audio_out_write(const int16_t *frames, size_t nframes)
{
    size_t written;
    i2s_channel_write(s_tx, frames, nframes * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}
