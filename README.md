# QIDI Box NFC Tool

A web-based NFC tag reader/writer for QIDI 3D printer filament spools, built on ESP32 with a PN532 NFC module.

Continuously scans for NFC tags and displays their data in a browser-based UI. Supports reading any ISO 14443A card and writing QIDI filament configuration (material + color) to MIFARE Classic 1K tags. Hardware settings are configurable at runtime via the web UI or a serial console — no rebuild required.

## Features

- **Continuous tag scanning** — automatically detects and reads tags placed on the reader
- **Multi-card support** — reads MIFARE Classic (sectors 0-3), Ultralight, and NTAG (all pages)
- **QIDI format decoding** — displays material name, color with swatch, and manufacturer byte
- **Tag writing** — write QIDI filament config with verification readback
- **Tag cloning** — load detected tag data into the writer with one click
- **Tag clearing** — zero out writable data on Classic or Ultralight/NTAG cards
- **Full hex dump** — raw data view with block/page labels for any card type
- **WiFi provisioning** — captive portal setup, no hardcoded credentials
- **Configurable LED indicators** — onboard or external WS2812/SK6812 strip with adjustable length, skip offset, brightness, and per-state colors
- **Runtime hardware config** — change I2C pins, LED settings, colors and timings from the web UI or serial console
- **Serial console** — interactive CLI over USB for headless setup when WiFi is unreachable
- **OTA-ready partition layout** — two ~3 MB app slots on 8 MB boards, two ~1.9 MB slots on 4 MB boards

## Supported Boards

| Board               | Flash | Default I2C (SDA / SCL) | Default LED          |
| ------------------- | ----- | ----------------------- | -------------------- |
| ESP32-S3-DevKitC-1  | 8 MB  | 8 / 9                   | GPIO 48 (WS2812)     |
| ESP32-C3-DevKitM-1  | 8 MB  | 6 / 7                   | GPIO 8 (WS2812)      |
| ESP32-C6-DevKitM-1  | 8 MB  | 6 / 7                   | GPIO 8 (WS2812)      |
| Seeed XIAO ESP32-C6 | 4 MB  | 22 / 23                 | GPIO 15 (plain GPIO) |

All defaults can be overridden at runtime — see [Runtime Configuration](#runtime-configuration).

### PN532 Wiring

Set the PN532 DIP switches to I2C mode: **SW1=ON, SW2=OFF**

| PN532 Pin | ESP32 Pin                                  |
| --------- | ------------------------------------------ |
| VCC       | 3.3V                                       |
| GND       | GND                                        |
| SDA       | board default (see table) or as configured |
| SCL       | board default (see table) or as configured |

## Getting Started

> **For full step-by-step setup**, including wiring, first-boot WiFi
> provisioning, recovery, and troubleshooting, see
> [docs/BUILDING.md](docs/BUILDING.md).

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)

### Build & Flash

Pick the environment matching your board:

```bash
pio run -e esp32-s3-devkitc-1 -t upload
pio run -e esp32-c3-devkitm-1 -t upload
pio run -e esp32-c6-devkitm-1 -t upload
pio run -e seeed_xiao_esp32c6 -t upload
```

### Serial Monitor

```bash
pio device monitor -e <env>
```

> **Tip:** use the integrated terminal (not the VS Code GUI Serial Monitor) — the GUI variant doesn't pass keystrokes through to the serial console.

### WiFi Setup

On first boot (or after a WiFi reset) the device starts an access point:

1. Connect to the **`QidiBox-Setup`** WiFi network
2. A captive portal opens automatically — enter your WiFi SSID and password
3. The device restarts and connects to your network
4. Open the printed IP address in a browser

## Runtime Configuration

All hardware settings are persisted in NVS and editable two ways:

### Web UI

Click **Settings** in the page footer (or visit `/settings`). You can change LED type, pin, length, skip offset, brightness, all status colors, OK/error display durations, I2C pins, and the BOOT button pin. The page tells you which changes need a restart.

### Serial Console

Open the serial monitor and press Enter — you should see a `>` prompt. Type `help` for the full command list:

```text
help                  show command list
status                show IP, NFC ready, I2C pins, LED state
get                   dump current config as JSON
set <key> <value>     change a hw setting (see keys below)
wifi <ssid> [pass]    save WiFi credentials and restart
wifi clear            forget WiFi credentials and restart
reset                 factory reset hardware config
restart               reboot
```

Settable keys: `led_en`, `led_type`, `led_pin`, `led_bright`, `led_len`, `led_skip`, `c_scan`, `c_ok`, `c_err`, `c_busy`, `c_setup`, `t_ok`, `t_err`, `i2c_sda`, `i2c_scl`, `boot_pin`.

### Resetting Everything

- **BOOT button** — hold during power-on to wipe both WiFi credentials and hardware config
- **Auto-fallback** — if the saved network is unreachable, the setup AP starts automatically

### Failure Modes Are Recoverable

If the PN532 isn't detected at boot (wrong I2C pins, missing wiring), the firmware **does not halt**. The web UI and serial console remain reachable so you can correct the configuration without re-flashing.

## LED Behavior

| Color       | Meaning                                            |
| ----------- | -------------------------------------------------- |
| Purple      | AP/setup mode (waiting for WiFi config)            |
| Yellow      | Connecting to WiFi / busy reading or writing a tag |
| Blue        | Connected, scanning for tags                       |
| Green (3 s) | Tag read or write succeeded                        |
| Red (2 s)   | Error (auth failed, read/write failed)             |

For addressable strips, set `led_length` to the number of pixels you want lit and `led_skip` to leave the first N pixels off (useful when the strip starts with a sacrificial buffer LED).

For boards without an onboard addressable LED, set `led_type=0` (plain GPIO) — the chosen pin is driven on for any active state, off otherwise. The Seeed XIAO ESP32-C6 environment defaults to `led_type=0` since the board has no user RGB LED.

## Project Layout

```text
src/
  main.c              app entry, NFC poll task, boot orchestration
  pn532.[ch]          custom ESP-IDF I2C driver for the PN532
  webserver.[ch]      esp_http_server routes (status/write/clear/settings)
  wifi.[ch]           STA + AP mode helpers
  dns_server.[ch]     captive portal DNS responder (raw lwIP UDP)
  led.[ch]            led_strip RMT driver with length/skip
  config.[ch]         NVS-backed hardware config
  console.[ch]        interactive serial CLI
data/
  index.html, style.css, app.js     main web UI
  setup.html                        captive portal page
  settings.html                     hardware settings page
scripts/
  gen_web_assets.py   pre-build: gzip data/* into src/web_assets.h
  fix_flash_size.py   pre-build: write per-env sdkconfig flash size override
partitions.csv        OTA partition table for 8 MB boards
partitions_4mb.csv    OTA partition table for 4 MB boards
sdkconfig.defaults    shared ESP-IDF settings
```

Web assets in `data/` are gzipped at build time and embedded into the firmware binary as C arrays — no filesystem partition needed. Edit the source files normally; gzip happens automatically.

## Web API

| Endpoint                    | Method | Description                                  |
| --------------------------- | ------ | -------------------------------------------- |
| `/`                         | GET    | Main web UI                                  |
| `/style.css`                | GET    | Stylesheet                                   |
| `/app.js`                   | GET    | JavaScript                                   |
| `/status`                   | GET    | Last-read tag data + `nfcReady` flag (JSON)  |
| `/write?material=N&color=N` | GET    | Write QIDI data to a Classic 1K tag          |
| `/clear`                    | GET    | Zero out tag data                            |
| `/settings`                 | GET    | Hardware settings page                       |
| `/api/config`               | GET    | Current hardware config (JSON)               |
| `/api/config`               | POST   | Update hardware config                       |
| `/api/config/reset`         | POST   | Reset hardware config to defaults            |
| `/api/restart`              | POST   | Reboot the device                            |
| `/wifi-setup`               | GET    | WiFi configuration page                      |
| `/wifi-save`                | POST   | Save WiFi credentials and restart            |

When the NFC reader is unavailable, `/write` and `/clear` return HTTP 503 instead of hanging.

## Supported Cards

**The QIDI Box only accepts MIFARE Classic 1K tags.** That's the one card type you can write a working filament profile to. The other types are supported for reading and raw-data display only — useful for inspecting random ISO 14443A cards, but they cannot be used as QIDI filament tags.

| Card Type         | QIDI compatible? | Read           | Write               | Clear           |
| ----------------- | ---------------- | -------------- | ------------------- | --------------- |
| MIFARE Classic 1K | Yes              | Sectors 0-3    | Block 4 (QIDI data) | Blocks 4-6      |
| MIFARE Ultralight | No (read only)   | All pages      | —                   | User pages (4+) |
| NTAG 213/215/216  | No (read only)   | Up to 44 pages | —                   | User pages (4+) |

## Architecture Notes

- **Pure ESP-IDF** — no Arduino dependency. Uses `esp_wifi`, `esp_http_server`, `nvs_flash`, `driver/i2c_master`, `driver/usb_serial_jtag`, and the `led_strip` managed component.
- **Custom PN532 driver** — a minimal C reimplementation of the protocol covering only the eight commands the app uses (firmware version, SAM config, passive target detection, MIFARE Classic auth/read/write, Ultralight read/write).
- **Custom captive portal DNS** — a few lines of raw lwIP UDP that point all A queries at the AP IP, triggering the OS captive-portal popup.
- **NFC polling task** — runs as a FreeRTOS task; the HTTP handlers coordinate with it via an `nfc_busy` flag so write/clear requests don't race with background reads.
- **Boot is non-fatal** — every configurable subsystem can fail at startup without halting the firmware. If something looks wrong, the web UI and serial console are always reachable.

## License

MIT
