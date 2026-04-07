#include "pn532.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pn532";

// PN532 protocol constants
#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5

#define PN532_CMD_GETFIRMWAREVERSION  0x02
#define PN532_CMD_SAMCONFIGURATION    0x14
#define PN532_CMD_RFCONFIGURATION     0x32
#define PN532_CMD_INLISTPASSIVETARGET 0x4A
#define PN532_CMD_INDATAEXCHANGE      0x40

#define MIFARE_CMD_AUTH_A   0x60
#define MIFARE_CMD_AUTH_B   0x61
#define MIFARE_CMD_READ     0x30
#define MIFARE_CMD_WRITE    0xA0
#define MIFARE_UL_CMD_WRITE 0xA2

#define PN532_I2C_READY     0x01
#define PACKET_BUF_SIZE     64

static uint8_t pn532_ack[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
static uint8_t pkt_buf[PACKET_BUF_SIZE];

// --- Low-level I2C communication ---

static bool i2c_read_data(pn532_t *nfc, uint8_t *buf, uint8_t n)
{
    // PN532 I2C: first byte is RDY status, followed by data
    uint8_t tmp[n + 1];
    esp_err_t err = i2c_master_receive(nfc->dev, tmp, n + 1, 100);
    if (err != ESP_OK) return false;
    memcpy(buf, tmp + 1, n);
    return true;
}

static bool i2c_write_data(pn532_t *nfc, uint8_t *buf, uint8_t n)
{
    return i2c_master_transmit(nfc->dev, buf, n, 100) == ESP_OK;
}

static bool is_ready(pn532_t *nfc)
{
    uint8_t rdy;
    if (i2c_master_receive(nfc->dev, &rdy, 1, 100) != ESP_OK) return false;
    return rdy == PN532_I2C_READY;
}

static bool wait_ready(pn532_t *nfc, uint16_t timeout_ms)
{
    uint16_t elapsed = 0;
    while (!is_ready(nfc)) {
        if (timeout_ms != 0 && elapsed >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    return true;
}

static void write_command(pn532_t *nfc, uint8_t *cmd, uint8_t cmdlen)
{
    uint8_t LEN = cmdlen + 1;
    uint8_t packet[8 + cmdlen];
    packet[0] = PN532_PREAMBLE;
    packet[1] = PN532_STARTCODE1;
    packet[2] = PN532_STARTCODE2;
    packet[3] = LEN;
    packet[4] = ~LEN + 1;
    packet[5] = PN532_HOSTTOPN532;
    uint8_t sum = 0;
    for (uint8_t i = 0; i < cmdlen; i++) {
        packet[6 + i] = cmd[i];
        sum += cmd[i];
    }
    packet[6 + cmdlen] = ~(PN532_HOSTTOPN532 + sum) + 1;
    packet[7 + cmdlen] = PN532_POSTAMBLE;

    i2c_write_data(nfc, packet, 8 + cmdlen);
}

static bool read_ack(pn532_t *nfc)
{
    uint8_t buf[6];
    if (!i2c_read_data(nfc, buf, 6)) return false;
    return memcmp(buf, pn532_ack, 6) == 0;
}

static bool send_command_check_ack(pn532_t *nfc, uint8_t *cmd, uint8_t cmdlen, uint16_t timeout)
{
    write_command(nfc, cmd, cmdlen);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (!wait_ready(nfc, timeout)) return false;
    if (!read_ack(nfc)) return false;

    vTaskDelay(pdMS_TO_TICKS(1));
    if (!wait_ready(nfc, timeout)) return false;

    return true;
}

// --- Public API ---

bool pn532_init(pn532_t *nfc, int sda_pin, int scl_pin)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  // auto-select
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &nfc->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2C bus: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PN532_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(nfc->bus, &dev_cfg, &nfc->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        return false;
    }

    // Allow PN532 to wake up
    vTaskDelay(pdMS_TO_TICKS(50));

    // SAMConfig as wakeup
    pn532_sam_config(nfc);

    return true;
}

uint32_t pn532_get_firmware_version(pn532_t *nfc)
{
    pkt_buf[0] = PN532_CMD_GETFIRMWAREVERSION;
    if (!send_command_check_ack(nfc, pkt_buf, 1, 1000)) return 0;

    if (!i2c_read_data(nfc, pkt_buf, 13)) return 0;

    // Verify response header
    if (pkt_buf[0] != 0x00 || pkt_buf[1] != 0x00 || pkt_buf[2] != 0xFF) return 0;

    // Response: [preamble(3)] [len] [lcs] [D5] [03] [IC] [Ver] [Rev] [Support] ...
    uint32_t response = pkt_buf[7];  // IC
    response <<= 8;
    response |= pkt_buf[8];         // Ver
    response <<= 8;
    response |= pkt_buf[9];         // Rev
    response <<= 8;
    response |= pkt_buf[10];        // Support

    return response;
}

bool pn532_sam_config(pn532_t *nfc)
{
    pkt_buf[0] = PN532_CMD_SAMCONFIGURATION;
    pkt_buf[1] = 0x01; // normal mode
    pkt_buf[2] = 0x14; // timeout 50ms * 20 = 1s
    pkt_buf[3] = 0x01; // use IRQ pin

    if (!send_command_check_ack(nfc, pkt_buf, 4, 1000)) return false;

    if (!i2c_read_data(nfc, pkt_buf, 9)) return false;
    return (pkt_buf[6] == 0x15);
}

bool pn532_read_passive_target(pn532_t *nfc, uint8_t *uid, uint8_t *uid_len, uint16_t timeout_ms)
{
    pkt_buf[0] = PN532_CMD_INLISTPASSIVETARGET;
    pkt_buf[1] = 1; // max 1 card
    pkt_buf[2] = PN532_MIFARE_ISO14443A;

    if (!send_command_check_ack(nfc, pkt_buf, 3, timeout_ms)) return false;

    if (!i2c_read_data(nfc, pkt_buf, 20)) return false;

    // pkt_buf[7] = number of targets found
    if (pkt_buf[7] != 1) return false;

    *uid_len = pkt_buf[12];
    for (uint8_t i = 0; i < pkt_buf[12] && i < 7; i++) {
        uid[i] = pkt_buf[13 + i];
    }

    return true;
}

bool pn532_mifare_authenticate(pn532_t *nfc, uint8_t *uid, uint8_t uid_len,
                                uint32_t block, uint8_t key_type, uint8_t *key)
{
    pkt_buf[0] = PN532_CMD_INDATAEXCHANGE;
    pkt_buf[1] = 1; // card number
    pkt_buf[2] = (key_type == 1) ? MIFARE_CMD_AUTH_B : MIFARE_CMD_AUTH_A;
    pkt_buf[3] = (uint8_t)block;
    memcpy(pkt_buf + 4, key, 6);
    for (uint8_t i = 0; i < uid_len; i++) {
        pkt_buf[10 + i] = uid[i];
    }

    if (!send_command_check_ack(nfc, pkt_buf, 10 + uid_len, 1000)) return false;

    if (!i2c_read_data(nfc, pkt_buf, 12)) return false;

    // Success: byte 7 == 0x00
    return (pkt_buf[7] == 0x00);
}

bool pn532_mifare_read_block(pn532_t *nfc, uint8_t block, uint8_t *data)
{
    pkt_buf[0] = PN532_CMD_INDATAEXCHANGE;
    pkt_buf[1] = 1;
    pkt_buf[2] = MIFARE_CMD_READ;
    pkt_buf[3] = block;

    if (!send_command_check_ack(nfc, pkt_buf, 4, 1000)) return false;

    if (!i2c_read_data(nfc, pkt_buf, 26)) return false;

    if (pkt_buf[7] != 0x00) return false;

    memcpy(data, pkt_buf + 8, 16);
    return true;
}

bool pn532_mifare_write_block(pn532_t *nfc, uint8_t block, uint8_t *data)
{
    pkt_buf[0] = PN532_CMD_INDATAEXCHANGE;
    pkt_buf[1] = 1;
    pkt_buf[2] = MIFARE_CMD_WRITE;
    pkt_buf[3] = block;
    memcpy(pkt_buf + 4, data, 16);

    if (!send_command_check_ack(nfc, pkt_buf, 20, 1000)) return false;

    vTaskDelay(pdMS_TO_TICKS(10));

    if (!i2c_read_data(nfc, pkt_buf, 26)) return false;
    return true;
}

bool pn532_ultralight_read_page(pn532_t *nfc, uint8_t page, uint8_t *data)
{
    if (page >= 64) return false;

    pkt_buf[0] = PN532_CMD_INDATAEXCHANGE;
    pkt_buf[1] = 1;
    pkt_buf[2] = MIFARE_CMD_READ;
    pkt_buf[3] = page;

    if (!send_command_check_ack(nfc, pkt_buf, 4, 1000)) return false;

    if (!i2c_read_data(nfc, pkt_buf, 26)) return false;

    if (pkt_buf[7] != 0x00) return false;

    // Read command returns 16 bytes but we only need the first 4 for UL
    memcpy(data, pkt_buf + 8, 4);
    return true;
}

bool pn532_ultralight_write_page(pn532_t *nfc, uint8_t page, uint8_t *data)
{
    if (page >= 64) return false;

    pkt_buf[0] = PN532_CMD_INDATAEXCHANGE;
    pkt_buf[1] = 1;
    pkt_buf[2] = MIFARE_UL_CMD_WRITE;
    pkt_buf[3] = page;
    memcpy(pkt_buf + 4, data, 4);

    if (!send_command_check_ack(nfc, pkt_buf, 8, 1000)) return false;

    vTaskDelay(pdMS_TO_TICKS(10));

    if (!i2c_read_data(nfc, pkt_buf, 26)) return false;
    return true;
}
