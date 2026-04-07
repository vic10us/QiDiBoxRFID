/**
 * QIDI Box NFC Tool — ESP-IDF Implementation
 * ESP32 (S3/C3/C6) + PN532 (I2C, Generic V3 Module)
 *
 * Continuously reads NFC tags and displays their data via a web UI.
 * Also supports writing QIDI filament config (material + color) to tags.
 * LED indicator provides visual status feedback (configurable via /settings).
 *
 * Pure ESP-IDF — no Arduino dependency.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "config.h"
#include "pn532.h"
#include "led.h"
#include "wifi.h"
#include "dns_server.h"
#include "webserver.h"
#include "console.h"

static const char *TAG = "main";

#define AP_SSID       "QidiBox-Setup"
#define NVS_NAMESPACE "qidibox"

#define MAX_TAG_DATA          256
#define CLASSIC_BLOCKS_TO_READ 16
#define UL_PAGES_TO_READ       44
#define QIDI_BLOCK             4

// Shared NFC device and state — accessed by webserver.c
pn532_t nfc_dev;
bool nfc_busy = false;
bool nfc_ready = false;  // false if PN532 init/probe failed; NFC ops are skipped

// These are defined in webserver.c
typedef enum {
    CARD_UNKNOWN = 0,
    CARD_MIFARE_CLASSIC,
    CARD_MIFARE_ULTRALIGHT
} card_type_t;

extern struct {
    bool valid;
    uint8_t uid[7];
    uint8_t uid_len;
    card_type_t card_type;
    uint8_t data[MAX_TAG_DATA];
    uint16_t data_len;
    uint8_t pages_read;
    uint8_t blocks_read;
    bool is_qidi_format;
    bool auth_failed;
    bool read_failed;
    uint32_t timestamp;
} last_tag;

extern uint8_t prev_uid[7];
extern uint8_t prev_uid_len;
extern bool tag_present;
extern uint32_t led_ok_until;
extern uint32_t led_err_until;

static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const char *card_type_name(card_type_t ct)
{
    switch (ct) {
        case CARD_MIFARE_CLASSIC:    return "MIFARE Classic";
        case CARD_MIFARE_ULTRALIGHT: return "MIFARE Ultralight / NTAG";
        default:                     return "Unknown";
    }
}

static card_type_t detect_card_type(uint8_t uid_len)
{
    switch (uid_len) {
        case 4:  return CARD_MIFARE_CLASSIC;
        case 7:  return CARD_MIFARE_ULTRALIGHT;
        default: return CARD_UNKNOWN;
    }
}

static void nfc_poll_task(void *arg)
{
    while (1) {
        uint32_t now_ms = millis();

        // Handle LED timeout
        if (led_ok_until && now_ms > led_ok_until) {
            led_ok_until = 0;
            led_scan();
        }
        if (led_err_until && now_ms > led_err_until) {
            led_err_until = 0;
            led_scan();
        }

        // Don't poll while write/clear is in progress
        if (nfc_busy) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Poll for a tag (500ms timeout)
        uint8_t uid[7];
        uint8_t uid_len;

        if (!pn532_read_passive_target(&nfc_dev, uid, &uid_len, 500)) {
            tag_present = false;
            continue;
        }

        // Check if same tag still sitting on reader
        bool same_tag = tag_present &&
                        uid_len == prev_uid_len &&
                        memcmp(uid, prev_uid, uid_len) == 0;

        if (same_tag) continue;

        // New tag detected
        tag_present = true;
        prev_uid_len = uid_len;
        memcpy(prev_uid, uid, uid_len);

        led_busy();

        ESP_LOGI(TAG, "Tag detected — UID:");
        for (int i = 0; i < uid_len; i++) {
            printf("%02X ", uid[i]);
        }
        printf("\n");

        // Build tag data
        memset(&last_tag, 0, sizeof(last_tag));
        last_tag.valid = true;
        memcpy(last_tag.uid, uid, uid_len);
        last_tag.uid_len = uid_len;
        last_tag.card_type = detect_card_type(uid_len);
        last_tag.timestamp = millis();

        ESP_LOGI(TAG, "Card type: %s", card_type_name(last_tag.card_type));

        if (last_tag.card_type == CARD_MIFARE_CLASSIC) {
            bool any_read = false;
            for (uint8_t sector = 0; sector < 4; sector++) {
                uint8_t first_block = sector * 4;
                if (!pn532_mifare_authenticate(&nfc_dev, uid, uid_len, first_block, 0, default_key)) {
                    ESP_LOGW(TAG, "Sector %d auth failed", sector);
                    if (sector == 0 && last_tag.blocks_read == 0) {
                        last_tag.auth_failed = true;
                    }
                    continue;
                }
                for (uint8_t b = 0; b < 4; b++) {
                    uint8_t block = first_block + b;
                    if (pn532_mifare_read_block(&nfc_dev, block, last_tag.data + (block * 16))) {
                        last_tag.blocks_read++;
                        any_read = true;
                    }
                }
            }
            last_tag.data_len = CLASSIC_BLOCKS_TO_READ * 16;

            if (!any_read) {
                if (last_tag.auth_failed) {
                    ESP_LOGW(TAG, "Auth failed on all sectors.");
                } else {
                    last_tag.read_failed = true;
                    ESP_LOGW(TAG, "Read failed on all blocks.");
                }
                led_err();
                led_err_until = millis() + hw_cfg.duration_err;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            // Check QIDI format at block 4: byte[2] == 0x01
            last_tag.is_qidi_format = (last_tag.data[QIDI_BLOCK * 16 + 2] == 0x01);

            if (last_tag.is_qidi_format) {
                ESP_LOGI(TAG, "QIDI tag — Material: %d, Color: %d",
                    last_tag.data[QIDI_BLOCK * 16], last_tag.data[QIDI_BLOCK * 16 + 1]);
            }

            ESP_LOGI(TAG, "Read %d/%d blocks", last_tag.blocks_read, CLASSIC_BLOCKS_TO_READ);

        } else if (last_tag.card_type == CARD_MIFARE_ULTRALIGHT) {
            uint8_t buf[4];
            for (uint8_t page = 0; page < UL_PAGES_TO_READ; page++) {
                if (!pn532_ultralight_read_page(&nfc_dev, page, buf)) break;
                memcpy(last_tag.data + (page * 4), buf, 4);
                last_tag.pages_read++;
            }
            last_tag.data_len = last_tag.pages_read * 4;

            if (last_tag.pages_read == 0) {
                last_tag.read_failed = true;
                ESP_LOGW(TAG, "UL/NTAG read failed — no pages read.");
                led_err();
                led_err_until = millis() + hw_cfg.duration_err;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ESP_LOGI(TAG, "Read %d pages (%d bytes)", last_tag.pages_read, last_tag.data_len);

        } else {
            ESP_LOGI(TAG, "Unknown card type — cannot read data.");
        }

        led_ok();
        led_ok_until = millis() + hw_cfg.duration_ok;
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Start console (also unbuffers stdio so logs appear promptly)
    console_start();

    // Quiet down the noisy WiFi driver logs so they don't bury the prompt
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);

    ESP_LOGI(TAG, "=== QIDI NFC Tool (ESP-IDF) ===");

    // Load hardware config
    config_load(&hw_cfg);

    // Init LED
    led_init();
    led_off();

    // Init PN532 — failures are non-fatal so settings remain reachable via web UI
    if (!pn532_init(&nfc_dev, hw_cfg.sda_pin, hw_cfg.scl_pin)) {
        ESP_LOGW(TAG, "I2C init failed (SDA=%d SCL=%d). NFC disabled — fix in /settings.",
            hw_cfg.sda_pin, hw_cfg.scl_pin);
    } else {
        uint32_t ver = pn532_get_firmware_version(&nfc_dev);
        if (!ver) {
            ESP_LOGW(TAG, "PN532 not detected on I2C (SDA=%d SCL=%d). "
                "Check wiring/DIP switches. NFC disabled.",
                hw_cfg.sda_pin, hw_cfg.scl_pin);
        } else {
            ESP_LOGI(TAG, "PN532 found. Chip: PN5%02X, Firmware: %d.%d",
                (ver >> 24) & 0xFF, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
            pn532_sam_config(&nfc_dev);
            nfc_ready = true;
        }
    }

    // Boot button check: hold to reset all settings
    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << 0) | (1ULL << hw_cfg.boot_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (gpio_get_level(0) == 0 || (hw_cfg.boot_pin != 0 && gpio_get_level(hw_cfg.boot_pin) == 0)) {
        ESP_LOGI(TAG, "BOOT button held — clearing all settings...");
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "ssid");
            nvs_erase_key(h, "pass");
            nvs_commit(h);
            nvs_close(h);
        }
        config_reset();
        config_load(&hw_cfg);
        for (int i = 0; i < 6; i++) {
            led_setup_color();
            vTaskDelay(pdMS_TO_TICKS(200));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // Load WiFi credentials
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    nvs_handle_t h;
    bool has_creds = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0] != '\0') {
            nvs_get_str(h, "pass", pass, &pass_len);
            has_creds = true;
        }
        nvs_close(h);
    }

    if (!has_creds) {
        printf("\r\nNo WiFi credentials found. Starting setup AP...\r\n");
        led_setup_color();
        wifi_init_ap(AP_SSID);

        char ip[16];
        wifi_get_ip_str(ip, sizeof(ip));
        printf("\r\n*** AP MODE ***\r\n");
        printf("SSID:      %s\r\n", AP_SSID);
        printf("Setup URL: http://%s\r\n\r\n", ip);

        dns_server_start(ip);
        webserver_start(true);
        return;
    }

    // Normal WiFi connection
    led_busy();
    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    if (!wifi_init_sta(ssid, pass, 15000)) {
        printf("\r\nWiFi connection failed! Falling back to setup AP...\r\n");
        wifi_stop();
        led_setup_color();
        wifi_init_ap(AP_SSID);

        char ip[16];
        wifi_get_ip_str(ip, sizeof(ip));
        printf("\r\n*** FALLBACK AP MODE ***\r\n");
        printf("SSID:      %s\r\n", AP_SSID);
        printf("Setup URL: http://%s\r\n\r\n", ip);

        dns_server_start(ip);
        webserver_start(true);
        return;
    }

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    printf("\r\n*** WIFI CONNECTED ***\r\n");
    printf("SSID: %s\r\n", ssid);
    printf("URL:  http://%s\r\n\r\n", ip);
    led_scan();

    // Start web server in normal mode
    webserver_start(false);
    ESP_LOGI(TAG, "Web server started. Open the IP above in your browser.");

    // Start NFC polling task only if the reader was detected
    if (nfc_ready) {
        xTaskCreate(nfc_poll_task, "nfc_poll", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "NFC polling disabled — reader not initialized.");
    }
}
