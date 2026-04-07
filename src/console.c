#include "console.h"
#include "config.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
#include "driver/uart.h"
#endif

static const char *TAG = "console";

#define NVS_NAMESPACE "qidibox"
#define LINE_MAX 128

extern bool nfc_ready;

static void print_help(void)
{
    printf("\r\n");
    printf("Available commands:\r\n");
    printf("  help                  show this help\r\n");
    printf("  status                show wifi, nfc, ip\r\n");
    printf("  get                   show all hardware settings (JSON)\r\n");
    printf("  set <key> <value>     change a hw setting (see keys below)\r\n");
    printf("  wifi <ssid> [pass]    save WiFi credentials and restart\r\n");
    printf("  wifi clear            forget WiFi credentials and restart\r\n");
    printf("  reset                 factory reset all settings\r\n");
    printf("  restart               reboot\r\n");
    printf("\r\n");
    printf("Settable keys:\r\n");
    printf("  led_en, led_type, led_pin, led_bright, led_len, led_skip\r\n");
    printf("  c_scan, c_ok, c_err, c_busy, c_setup    (hex e.g. #00ff00)\r\n");
    printf("  t_ok, t_err  (ms)\r\n");
    printf("  i2c_sda, i2c_scl, boot_pin\r\n");
    printf("\r\n");
    printf("Note: i2c, led pin/type/length/skip changes require restart.\r\n");
    printf("\r\n");
}

static void cmd_status(void)
{
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    printf("\r\n");
    printf("IP:        %s\r\n", ip);
    printf("NFC ready: %s\r\n", nfc_ready ? "yes" : "no");
    printf("I2C pins:  SDA=%d  SCL=%d\r\n", hw_cfg.sda_pin, hw_cfg.scl_pin);
    printf("LED:       type=%d  pin=%d  len=%d  skip=%d  bright=%d  enabled=%s\r\n",
        hw_cfg.led_type, hw_cfg.led_pin, hw_cfg.led_length, hw_cfg.led_skip,
        hw_cfg.led_brightness, hw_cfg.led_enabled ? "yes" : "no");
    printf("\r\n");
}

static void cmd_get(void)
{
    char buf[512];
    config_to_json(&hw_cfg, buf, sizeof(buf));
    printf("\r\n%s\r\n\r\n", buf);
}

static bool set_u8_field(const char *key, const char *val)
{
    int v = atoi(val);
    if (v < 0 || v > 255) return false;
    uint8_t u = (uint8_t)v;

    if      (!strcmp(key, "led_en"))     hw_cfg.led_enabled = u != 0;
    else if (!strcmp(key, "led_type"))   { if (u > 2) return false; hw_cfg.led_type = u; }
    else if (!strcmp(key, "led_pin"))    hw_cfg.led_pin = u;
    else if (!strcmp(key, "led_bright")) hw_cfg.led_brightness = u;
    else if (!strcmp(key, "led_len"))    { if (u < 1) u = 1; hw_cfg.led_length = u; if (hw_cfg.led_skip >= u) hw_cfg.led_skip = 0; }
    else if (!strcmp(key, "led_skip"))   { if (u >= hw_cfg.led_length) return false; hw_cfg.led_skip = u; }
    else if (!strcmp(key, "i2c_sda"))    hw_cfg.sda_pin = u;
    else if (!strcmp(key, "i2c_scl"))    hw_cfg.scl_pin = u;
    else if (!strcmp(key, "boot_pin"))   hw_cfg.boot_pin = u;
    else return false;
    return true;
}

static bool set_color_field(const char *key, const char *val)
{
    uint32_t c = hex_to_color(val);
    if      (!strcmp(key, "c_scan"))  hw_cfg.color_scan = c;
    else if (!strcmp(key, "c_ok"))    hw_cfg.color_ok = c;
    else if (!strcmp(key, "c_err"))   hw_cfg.color_err = c;
    else if (!strcmp(key, "c_busy"))  hw_cfg.color_busy = c;
    else if (!strcmp(key, "c_setup")) hw_cfg.color_setup = c;
    else return false;
    return true;
}

static bool set_u16_field(const char *key, const char *val)
{
    int v = atoi(val);
    if (v < 0 || v > 65535) return false;
    if      (!strcmp(key, "t_ok"))  hw_cfg.duration_ok = (uint16_t)v;
    else if (!strcmp(key, "t_err")) hw_cfg.duration_err = (uint16_t)v;
    else return false;
    return true;
}

static void cmd_set(char *args)
{
    char *key = strtok(args, " \t");
    char *val = strtok(NULL, " \t");
    if (!key || !val) {
        printf("usage: set <key> <value>\r\n");
        return;
    }
    bool ok = set_u8_field(key, val) || set_color_field(key, val) || set_u16_field(key, val);
    if (!ok) {
        printf("unknown key: %s  (try 'help')\r\n", key);
        return;
    }
    config_save(&hw_cfg);
    printf("ok — saved. some changes require 'restart'.\r\n");
}

static void cmd_wifi(char *args)
{
    char *first = strtok(args, " \t");
    if (!first) {
        printf("usage: wifi <ssid> [pass]   |   wifi clear\r\n");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        printf("err: nvs_open failed\r\n");
        return;
    }

    if (!strcmp(first, "clear")) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
        printf("WiFi credentials cleared. Restarting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    char *pass = strtok(NULL, "");  // rest of line, may contain spaces
    nvs_set_str(h, "ssid", first);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
    printf("WiFi saved (ssid=%s). Restarting...\r\n", first);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void process_line(char *line)
{
    // Trim leading whitespace
    while (*line == ' ' || *line == '\t') line++;
    // Strip trailing CR/LF/space
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    char *args = strtok(NULL, "");
    if (!args) args = "";

    if      (!strcmp(cmd, "help"))    print_help();
    else if (!strcmp(cmd, "?"))       print_help();
    else if (!strcmp(cmd, "status"))  cmd_status();
    else if (!strcmp(cmd, "get"))     cmd_get();
    else if (!strcmp(cmd, "set"))     cmd_set(args);
    else if (!strcmp(cmd, "wifi"))    cmd_wifi(args);
    else if (!strcmp(cmd, "reset"))   { config_reset(); printf("Hardware config reset. Restart to apply.\r\n"); }
    else if (!strcmp(cmd, "restart")) { printf("Restarting...\r\n"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    else printf("unknown command: %s  (try 'help')\r\n", cmd);

    printf("> ");
    fflush(stdout);
}

static void console_task(void *arg)
{
    char line[LINE_MAX];
    size_t pos = 0;

    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\r\nQIDI Box NFC console — type 'help' for commands\r\n> ");
    fflush(stdout);

    while (1) {
        uint8_t ch;
        int n = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        n = usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
        n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, portMAX_DELAY);
#else
        vTaskDelay(portMAX_DELAY);
#endif
        if (n <= 0) continue;

        // Echo so the user can see what they typed
        if (ch == '\r' || ch == '\n') {
            printf("\r\n");
            fflush(stdout);
            line[pos] = '\0';
            process_line(line);
            pos = 0;
            continue;
        }

        if (ch == 0x08 || ch == 0x7F) { // backspace / DEL
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (ch >= 0x20 && ch < 0x7F && pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

void console_start(void)
{
    // Disable stdio buffering so prints appear immediately
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // Install the full JTAG driver so the ISR actually pulls bytes from the
    // USB OUT endpoint into a RX buffer. Without this, usb_serial_jtag_read_bytes
    // always times out because the endpoint is never drained.
    usb_serial_jtag_driver_config_t jcfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    jcfg.rx_buffer_size = 256;
    jcfg.tx_buffer_size = 256;
    esp_err_t err = usb_serial_jtag_driver_install(&jcfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "JTAG driver already installed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG driver install failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "JTAG driver installed");
    }
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
    uart_config_t uart_cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_cfg);
#endif

    BaseType_t ok = xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    } else {
        ESP_LOGI(TAG, "Console task created");
    }
}
