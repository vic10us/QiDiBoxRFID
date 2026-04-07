#include "led.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "led";
static led_strip_handle_t strip_handle = NULL;

void led_init(void)
{
    if (hw_cfg.led_type == LED_GPIO) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << hw_cfg.led_pin),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        gpio_set_level(hw_cfg.led_pin, 0);
    } else {
        // WS2812 or SK6812 via RMT
        led_strip_config_t strip_cfg = {
            .strip_gpio_num = hw_cfg.led_pin,
            .max_leds = 1,
            .led_model = (hw_cfg.led_type == LED_SK6812) ? LED_MODEL_SK6812 : LED_MODEL_WS2812,
            .led_pixel_format = (hw_cfg.led_type == LED_SK6812) ? LED_PIXEL_FORMAT_GRBW : LED_PIXEL_FORMAT_GRB,
            .flags.invert_out = false,
        };
        led_strip_rmt_config_t rmt_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000, // 10 MHz
            .flags.with_dma = false,
        };
        esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init LED strip: %s", esp_err_to_name(err));
            strip_handle = NULL;
            return;
        }
        led_strip_clear(strip_handle);
    }
}

void led_set_color(uint32_t packed_rgb)
{
    if (!hw_cfg.led_enabled) return;

    uint8_t r = (packed_rgb >> 16) & 0xFF;
    uint8_t g = (packed_rgb >> 8) & 0xFF;
    uint8_t b = packed_rgb & 0xFF;

    r = (uint8_t)((r * hw_cfg.led_brightness) / 255);
    g = (uint8_t)((g * hw_cfg.led_brightness) / 255);
    b = (uint8_t)((b * hw_cfg.led_brightness) / 255);

    if (hw_cfg.led_type == LED_GPIO) {
        gpio_set_level(hw_cfg.led_pin, (r || g || b) ? 1 : 0);
    } else if (strip_handle) {
        led_strip_set_pixel(strip_handle, 0, r, g, b);
        led_strip_refresh(strip_handle);
    }
}

void led_off(void)
{
    if (!hw_cfg.led_enabled) return;
    if (strip_handle) {
        led_strip_clear(strip_handle);
    }
    if (hw_cfg.led_type == LED_GPIO) {
        gpio_set_level(hw_cfg.led_pin, 0);
    }
}

void led_scan(void)       { led_set_color(hw_cfg.color_scan); }
void led_ok(void)         { led_set_color(hw_cfg.color_ok); }
void led_err(void)        { led_set_color(hw_cfg.color_err); }
void led_busy(void)       { led_set_color(hw_cfg.color_busy); }
void led_setup_color(void) { led_set_color(hw_cfg.color_setup); }
