#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

// PN532 I2C address (0x48 >> 1 = 0x24)
#define PN532_I2C_ADDRESS      0x24
#define PN532_MIFARE_ISO14443A 0x00

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} pn532_t;

// Initialize I2C bus and PN532 device
bool pn532_init(pn532_t *nfc, int sda_pin, int scl_pin);

// Get firmware version. Returns packed 32-bit: IC | Ver | Rev | Support
uint32_t pn532_get_firmware_version(pn532_t *nfc);

// Configure SAM (Secure Access Module)
bool pn532_sam_config(pn532_t *nfc);

// Wait for a passive target (ISO14443A). Returns true if found.
bool pn532_read_passive_target(pn532_t *nfc, uint8_t *uid, uint8_t *uid_len, uint16_t timeout_ms);

// MIFARE Classic: authenticate a block
bool pn532_mifare_authenticate(pn532_t *nfc, uint8_t *uid, uint8_t uid_len,
                                uint32_t block, uint8_t key_type, uint8_t *key);

// MIFARE Classic: read 16-byte data block
bool pn532_mifare_read_block(pn532_t *nfc, uint8_t block, uint8_t *data);

// MIFARE Classic: write 16-byte data block
bool pn532_mifare_write_block(pn532_t *nfc, uint8_t block, uint8_t *data);

// MIFARE Ultralight: read 4-byte page
bool pn532_ultralight_read_page(pn532_t *nfc, uint8_t page, uint8_t *data);

// MIFARE Ultralight: write 4-byte page
bool pn532_ultralight_write_page(pn532_t *nfc, uint8_t page, uint8_t *data);
