#pragma once

#include <Arduino.h>
#include <Preferences.h>

// ─── BOARD DEFAULTS ─────────────────────────────────────────────────────────
// Auto-detected via CONFIG_IDF_TARGET_* (set by ESP-IDF/Arduino framework)

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

#define DEFAULT_LED_TYPE       1       // WS2812
#define DEFAULT_LED_BRIGHTNESS 40
#define DEFAULT_COLOR_SCAN     0x000028  // blue
#define DEFAULT_COLOR_OK       0x002800  // green
#define DEFAULT_COLOR_ERR      0x280000  // red
#define DEFAULT_COLOR_BUSY     0x281E00  // yellow
#define DEFAULT_COLOR_SETUP    0x1E0028  // purple
#define DEFAULT_DURATION_OK    3000      // ms
#define DEFAULT_DURATION_ERR   2000      // ms

// ─── LED TYPE ENUM ──────────────────────────────────────────────────────────

enum LedType : uint8_t {
  LED_GPIO   = 0,   // Simple on/off digital LED
  LED_WS2812 = 1,   // RGB addressable (neopixelWrite)
  LED_SK6812 = 2    // RGBW addressable (neopixelWrite, W=0)
};

// ─── CONFIG STRUCT ──────────────────────────────────────────────────────────

struct HwConfig {
  // LED
  bool     ledEnabled;
  LedType  ledType;
  uint8_t  ledPin;
  uint8_t  ledBrightness;

  // LED colors (packed 0xRRGGBB)
  uint32_t colorScan;
  uint32_t colorOk;
  uint32_t colorErr;
  uint32_t colorBusy;
  uint32_t colorSetup;

  // LED timings (ms)
  uint16_t durationOk;
  uint16_t durationErr;

  // I2C for NFC
  uint8_t  sdaPin;
  uint8_t  sclPin;

  // Boot button
  uint8_t  bootPin;
};

// Global config instance (defined in main.cpp)
extern HwConfig hwCfg;

// ─── NVS LOAD / SAVE / RESET ───────────────────────────────────────────────

inline void configLoad(HwConfig& cfg, Preferences& prefs) {
  prefs.begin("qidibox", true); // read-only

  cfg.ledEnabled  = prefs.getUChar("led_en", 1) != 0;
  cfg.ledType     = (LedType)prefs.getUChar("led_type", DEFAULT_LED_TYPE);
  cfg.ledPin      = prefs.getUChar("led_pin", DEFAULT_LED_PIN);
  cfg.ledBrightness = prefs.getUChar("led_bright", DEFAULT_LED_BRIGHTNESS);

  cfg.colorScan   = prefs.getUInt("c_scan", DEFAULT_COLOR_SCAN);
  cfg.colorOk     = prefs.getUInt("c_ok", DEFAULT_COLOR_OK);
  cfg.colorErr    = prefs.getUInt("c_err", DEFAULT_COLOR_ERR);
  cfg.colorBusy   = prefs.getUInt("c_busy", DEFAULT_COLOR_BUSY);
  cfg.colorSetup  = prefs.getUInt("c_setup", DEFAULT_COLOR_SETUP);

  cfg.durationOk  = prefs.getUShort("t_ok", DEFAULT_DURATION_OK);
  cfg.durationErr = prefs.getUShort("t_err", DEFAULT_DURATION_ERR);

  cfg.sdaPin      = prefs.getUChar("i2c_sda", DEFAULT_SDA_PIN);
  cfg.sclPin      = prefs.getUChar("i2c_scl", DEFAULT_SCL_PIN);
  cfg.bootPin     = prefs.getUChar("boot_pin", DEFAULT_BOOT_PIN);

  prefs.end();
}

inline void configSave(const HwConfig& cfg, Preferences& prefs) {
  prefs.begin("qidibox", false); // read-write

  prefs.putUChar("led_en", cfg.ledEnabled ? 1 : 0);
  prefs.putUChar("led_type", (uint8_t)cfg.ledType);
  prefs.putUChar("led_pin", cfg.ledPin);
  prefs.putUChar("led_bright", cfg.ledBrightness);

  prefs.putUInt("c_scan", cfg.colorScan);
  prefs.putUInt("c_ok", cfg.colorOk);
  prefs.putUInt("c_err", cfg.colorErr);
  prefs.putUInt("c_busy", cfg.colorBusy);
  prefs.putUInt("c_setup", cfg.colorSetup);

  prefs.putUShort("t_ok", cfg.durationOk);
  prefs.putUShort("t_err", cfg.durationErr);

  prefs.putUChar("i2c_sda", cfg.sdaPin);
  prefs.putUChar("i2c_scl", cfg.sclPin);
  prefs.putUChar("boot_pin", cfg.bootPin);

  prefs.end();
}

inline void configReset(Preferences& prefs) {
  prefs.begin("qidibox", false);
  const char* keys[] = {
    "led_en", "led_type", "led_pin", "led_bright",
    "c_scan", "c_ok", "c_err", "c_busy", "c_setup",
    "t_ok", "t_err", "i2c_sda", "i2c_scl", "boot_pin"
  };
  for (auto key : keys) {
    prefs.remove(key);
  }
  prefs.end();
}

// ─── LED ABSTRACTION ────────────────────────────────────────────────────────

inline void ledSetColor(uint32_t packedRGB) {
  if (!hwCfg.ledEnabled) return;

  uint8_t r = (packedRGB >> 16) & 0xFF;
  uint8_t g = (packedRGB >> 8) & 0xFF;
  uint8_t b = packedRGB & 0xFF;

  // Scale by brightness
  r = (uint8_t)((r * hwCfg.ledBrightness) / 255);
  g = (uint8_t)((g * hwCfg.ledBrightness) / 255);
  b = (uint8_t)((b * hwCfg.ledBrightness) / 255);

  switch (hwCfg.ledType) {
    case LED_GPIO:
      digitalWrite(hwCfg.ledPin, (r || g || b) ? HIGH : LOW);
      break;
    case LED_WS2812:
    case LED_SK6812:
      neopixelWrite(hwCfg.ledPin, r, g, b);
      break;
  }
}

inline void ledOff()   { if (!hwCfg.ledEnabled) return; neopixelWrite(hwCfg.ledPin, 0, 0, 0); if (hwCfg.ledType == LED_GPIO) digitalWrite(hwCfg.ledPin, LOW); }
inline void ledScan()  { ledSetColor(hwCfg.colorScan); }
inline void ledOk()    { ledSetColor(hwCfg.colorOk); }
inline void ledErr()   { ledSetColor(hwCfg.colorErr); }
inline void ledBusy()  { ledSetColor(hwCfg.colorBusy); }
inline void ledSetup() { ledSetColor(hwCfg.colorSetup); }

// ─── CONFIG JSON SERIALIZATION ──────────────────────────────────────────────

inline String colorToHex(uint32_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06X", c & 0xFFFFFF);
  return String(buf);
}

inline uint32_t hexToColor(const String& hex) {
  String h = hex;
  if (h.startsWith("#")) h = h.substring(1);
  return (uint32_t)strtoul(h.c_str(), NULL, 16);
}

inline String configToJson(const HwConfig& cfg) {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"led\":{\"enabled\":%s,\"type\":%d,\"pin\":%d,\"brightness\":%d},"
    "\"colors\":{\"scan\":\"%s\",\"ok\":\"%s\",\"err\":\"%s\",\"busy\":\"%s\",\"setup\":\"%s\"},"
    "\"timings\":{\"ok_ms\":%d,\"err_ms\":%d},"
    "\"i2c\":{\"sda\":%d,\"scl\":%d},"
    "\"boot_pin\":%d}",
    cfg.ledEnabled ? "true" : "false",
    (int)cfg.ledType, cfg.ledPin, cfg.ledBrightness,
    colorToHex(cfg.colorScan).c_str(),
    colorToHex(cfg.colorOk).c_str(),
    colorToHex(cfg.colorErr).c_str(),
    colorToHex(cfg.colorBusy).c_str(),
    colorToHex(cfg.colorSetup).c_str(),
    cfg.durationOk, cfg.durationErr,
    cfg.sdaPin, cfg.sclPin,
    cfg.bootPin);
  return String(buf);
}
