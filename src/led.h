#pragma once

#include "config.h"

void led_init(void);
void led_set_color(uint32_t packed_rgb);
void led_off(void);
void led_scan(void);
void led_ok(void);
void led_err(void);
void led_busy(void);
void led_setup_color(void);
