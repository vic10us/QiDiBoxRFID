#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "qidibox"

hw_config_t hw_cfg;

void config_load(hw_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // No saved config — use defaults
        cfg->led_enabled   = true;
        cfg->led_type      = DEFAULT_LED_TYPE;
        cfg->led_pin       = DEFAULT_LED_PIN;
        cfg->led_brightness = DEFAULT_LED_BRIGHTNESS;
        cfg->color_scan    = DEFAULT_COLOR_SCAN;
        cfg->color_ok      = DEFAULT_COLOR_OK;
        cfg->color_err     = DEFAULT_COLOR_ERR;
        cfg->color_busy    = DEFAULT_COLOR_BUSY;
        cfg->color_setup   = DEFAULT_COLOR_SETUP;
        cfg->duration_ok   = DEFAULT_DURATION_OK;
        cfg->duration_err  = DEFAULT_DURATION_ERR;
        cfg->sda_pin       = DEFAULT_SDA_PIN;
        cfg->scl_pin       = DEFAULT_SCL_PIN;
        cfg->boot_pin      = DEFAULT_BOOT_PIN;
        return;
    }

    uint8_t u8;
    uint16_t u16;
    uint32_t u32;

    cfg->led_enabled = (nvs_get_u8(h, "led_en", &u8) == ESP_OK) ? (u8 != 0) : true;
    cfg->led_type = (nvs_get_u8(h, "led_type", &u8) == ESP_OK) ? (led_type_t)u8 : DEFAULT_LED_TYPE;
    cfg->led_pin = (nvs_get_u8(h, "led_pin", &u8) == ESP_OK) ? u8 : DEFAULT_LED_PIN;
    cfg->led_brightness = (nvs_get_u8(h, "led_bright", &u8) == ESP_OK) ? u8 : DEFAULT_LED_BRIGHTNESS;

    cfg->color_scan = (nvs_get_u32(h, "c_scan", &u32) == ESP_OK) ? u32 : DEFAULT_COLOR_SCAN;
    cfg->color_ok   = (nvs_get_u32(h, "c_ok", &u32) == ESP_OK) ? u32 : DEFAULT_COLOR_OK;
    cfg->color_err  = (nvs_get_u32(h, "c_err", &u32) == ESP_OK) ? u32 : DEFAULT_COLOR_ERR;
    cfg->color_busy = (nvs_get_u32(h, "c_busy", &u32) == ESP_OK) ? u32 : DEFAULT_COLOR_BUSY;
    cfg->color_setup = (nvs_get_u32(h, "c_setup", &u32) == ESP_OK) ? u32 : DEFAULT_COLOR_SETUP;

    cfg->duration_ok  = (nvs_get_u16(h, "t_ok", &u16) == ESP_OK) ? u16 : DEFAULT_DURATION_OK;
    cfg->duration_err = (nvs_get_u16(h, "t_err", &u16) == ESP_OK) ? u16 : DEFAULT_DURATION_ERR;

    cfg->sda_pin  = (nvs_get_u8(h, "i2c_sda", &u8) == ESP_OK) ? u8 : DEFAULT_SDA_PIN;
    cfg->scl_pin  = (nvs_get_u8(h, "i2c_scl", &u8) == ESP_OK) ? u8 : DEFAULT_SCL_PIN;
    cfg->boot_pin = (nvs_get_u8(h, "boot_pin", &u8) == ESP_OK) ? u8 : DEFAULT_BOOT_PIN;

    nvs_close(h);
}

void config_save(const hw_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "led_en", cfg->led_enabled ? 1 : 0);
    nvs_set_u8(h, "led_type", (uint8_t)cfg->led_type);
    nvs_set_u8(h, "led_pin", cfg->led_pin);
    nvs_set_u8(h, "led_bright", cfg->led_brightness);

    nvs_set_u32(h, "c_scan", cfg->color_scan);
    nvs_set_u32(h, "c_ok", cfg->color_ok);
    nvs_set_u32(h, "c_err", cfg->color_err);
    nvs_set_u32(h, "c_busy", cfg->color_busy);
    nvs_set_u32(h, "c_setup", cfg->color_setup);

    nvs_set_u16(h, "t_ok", cfg->duration_ok);
    nvs_set_u16(h, "t_err", cfg->duration_err);

    nvs_set_u8(h, "i2c_sda", cfg->sda_pin);
    nvs_set_u8(h, "i2c_scl", cfg->scl_pin);
    nvs_set_u8(h, "boot_pin", cfg->boot_pin);

    nvs_commit(h);
    nvs_close(h);
}

void config_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    const char *keys[] = {
        "led_en", "led_type", "led_pin", "led_bright",
        "c_scan", "c_ok", "c_err", "c_busy", "c_setup",
        "t_ok", "t_err", "i2c_sda", "i2c_scl", "boot_pin"
    };
    for (int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        nvs_erase_key(h, keys[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void color_to_hex(uint32_t c, char *buf, size_t len)
{
    snprintf(buf, len, "#%06lX", (unsigned long)(c & 0xFFFFFF));
}

uint32_t hex_to_color(const char *hex)
{
    if (hex[0] == '#') hex++;
    return (uint32_t)strtoul(hex, NULL, 16);
}

int config_to_json(const hw_config_t *cfg, char *buf, size_t buf_size)
{
    char cs[8], co[8], ce[8], cb[8], cu[8];
    color_to_hex(cfg->color_scan, cs, sizeof(cs));
    color_to_hex(cfg->color_ok, co, sizeof(co));
    color_to_hex(cfg->color_err, ce, sizeof(ce));
    color_to_hex(cfg->color_busy, cb, sizeof(cb));
    color_to_hex(cfg->color_setup, cu, sizeof(cu));

    return snprintf(buf, buf_size,
        "{\"led\":{\"enabled\":%s,\"type\":%d,\"pin\":%d,\"brightness\":%d},"
        "\"colors\":{\"scan\":\"%s\",\"ok\":\"%s\",\"err\":\"%s\",\"busy\":\"%s\",\"setup\":\"%s\"},"
        "\"timings\":{\"ok_ms\":%d,\"err_ms\":%d},"
        "\"i2c\":{\"sda\":%d,\"scl\":%d},"
        "\"boot_pin\":%d}",
        cfg->led_enabled ? "true" : "false",
        (int)cfg->led_type, cfg->led_pin, cfg->led_brightness,
        cs, co, ce, cb, cu,
        cfg->duration_ok, cfg->duration_err,
        cfg->sda_pin, cfg->scl_pin,
        cfg->boot_pin);
}
