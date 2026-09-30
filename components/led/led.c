/*
 * WS2812B status LED, carried over from tspl-station (base / flash / pulse
 * on esp_timer under one lock) with the brightness made configurable.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "led.h"

static const char *TAG = "led";

#define LED_GPIO       CONFIG_DL_LED_GPIO
#define LED_FLASH_US   150000
#define LED_BRIGHTNESS_NUM CONFIG_DL_LED_BRIGHTNESS
#define LED_BRIGHTNESS_DEN 100

#if CONFIG_DL_LED_ORDER_RGB
#define LED_COLOR_FMT LED_STRIP_COLOR_COMPONENT_FMT_RGB
#else
#define LED_COLOR_FMT LED_STRIP_COLOR_COMPONENT_FMT_GRB
#endif

static led_strip_handle_t  s_led;
static SemaphoreHandle_t   s_lock;
static esp_timer_handle_t  s_flash_timer;
static esp_timer_handle_t  s_pulse_timer;
static uint8_t s_base_r, s_base_g, s_base_b;
static uint8_t s_pulse_r, s_pulse_g, s_pulse_b;
static bool    s_pulse_on;

static void led_show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_strip_set_pixel(s_led, 0,
                        r * LED_BRIGHTNESS_NUM / LED_BRIGHTNESS_DEN,
                        g * LED_BRIGHTNESS_NUM / LED_BRIGHTNESS_DEN,
                        b * LED_BRIGHTNESS_NUM / LED_BRIGHTNESS_DEN);
    led_strip_refresh(s_led);
    xSemaphoreGive(s_lock);
}

static void flash_return(void *arg)
{
    led_show(s_base_r, s_base_g, s_base_b);
}

static void pulse_step(void *arg)
{
    s_pulse_on = !s_pulse_on;
    led_show(s_pulse_on ? s_pulse_r : 0, s_pulse_on ? s_pulse_g : 0, s_pulse_on ? s_pulse_b : 0);
}

void led_base(uint8_t r, uint8_t g, uint8_t b)
{
    esp_timer_stop(s_pulse_timer);
    s_base_r = r; s_base_g = g; s_base_b = b;
    led_show(r, g, b);
}

void led_flash(uint8_t r, uint8_t g, uint8_t b)
{
    led_show(r, g, b);
    esp_timer_stop(s_flash_timer);
    esp_timer_start_once(s_flash_timer, LED_FLASH_US);
}

void led_pulse(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms)
{
    esp_timer_stop(s_pulse_timer);
    s_pulse_r = r; s_pulse_g = g; s_pulse_b = b;
    s_pulse_on = false;
    pulse_step(NULL);
    esp_timer_start_periodic(s_pulse_timer, (uint64_t)period_ms * 500);
}

void led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_COLOR_FMT,
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_led) != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed on GPIO%d -- running dark", LED_GPIO);
        s_led = NULL;
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    esp_timer_create_args_t fa = { .callback = flash_return, .name = "led_flash" };
    esp_timer_create_args_t pa = { .callback = pulse_step,   .name = "led_pulse" };
    ESP_ERROR_CHECK(esp_timer_create(&fa, &s_flash_timer));
    ESP_ERROR_CHECK(esp_timer_create(&pa, &s_pulse_timer));
    LED_NO_WIFI();
}
