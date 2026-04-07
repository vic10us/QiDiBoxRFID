#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

// Board defaults — auto-detected via CONFIG_IDF_TARGET_*
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define DEFAULT_SDA_PIN    8
  #define DEFAULT_SCL_PIN    9
  #define DEFAULT_LED_PIN    48
  #define DEFAULT_BOOT_PIN   0
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define DEFAULT_SDA_PIN    6
  #define DEFAULT_SCL_PIN    7
  #define DEFAULT_LED_PIN    8
  #define DEFAULT_BOOT_PIN   9
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #ifdef BOARD_SEEED_XIAO_ESP32C6
    #define DEFAULT_SDA_PIN    22
    #define DEFAULT_SCL_PIN    23
    #define DEFAULT_LED_PIN    15
    #define DEFAULT_BOOT_PIN   9
  #else
    #define DEFAULT_SDA_PIN    6
    #define DEFAULT_SCL_PIN    7
    #define DEFAULT_LED_PIN    8
    #define DEFAULT_BOOT_PIN   9
  #endif
#else
  #error "Unsupported ESP32 target. Supported: ESP32-S3, ESP32-C3, ESP32-C6."
#endif

#ifndef DEFAULT_LED_TYPE
  #define DEFAULT_LED_TYPE     1       // 0=GPIO, 1=WS2812, 2=SK6812
#endif
#define DEFAULT_LED_BRIGHTNESS 40
#define DEFAULT_LED_LENGTH     1
#define DEFAULT_LED_SKIP       0
#define DEFAULT_COLOR_SCAN     0x000028  // blue
#define DEFAULT_COLOR_OK       0x002800  // green
#define DEFAULT_COLOR_ERR      0x280000  // red
#define DEFAULT_COLOR_BUSY     0x281E00  // yellow
#define DEFAULT_COLOR_SETUP    0x1E0028  // purple
#define DEFAULT_DURATION_OK    3000      // ms
#define DEFAULT_DURATION_ERR   2000      // ms

typedef enum {
    LED_GPIO   = 0,
    LED_WS2812 = 1,
    LED_SK6812 = 2
} led_type_t;

typedef struct {
    bool     led_enabled;
    led_type_t led_type;
    uint8_t  led_pin;
    uint8_t  led_brightness;
    uint8_t  led_length;
    uint8_t  led_skip;

    uint32_t color_scan;
    uint32_t color_ok;
    uint32_t color_err;
    uint32_t color_busy;
    uint32_t color_setup;

    uint16_t duration_ok;
    uint16_t duration_err;

    uint8_t  sda_pin;
    uint8_t  scl_pin;
    uint8_t  boot_pin;
} hw_config_t;

extern hw_config_t hw_cfg;

void config_load(hw_config_t *cfg);
void config_save(const hw_config_t *cfg);
void config_reset(void);
int config_to_json(const hw_config_t *cfg, char *buf, size_t buf_size);
uint32_t hex_to_color(const char *hex);
