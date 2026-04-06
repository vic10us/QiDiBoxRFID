# QIDI Box NFC Tool

A web-based NFC tag reader/writer for QIDI 3D printer filament spools, built on the ESP32-S3 with a PN532 NFC module.

Continuously scans for NFC tags and displays their data in a browser-based UI. Supports reading any ISO 14443A card and writing QIDI filament configuration (material + color) to MIFARE Classic 1K tags.

## Features

- **Continuous tag scanning** -- automatically detects and reads tags placed on the reader
- **Multi-card support** -- reads MIFARE Classic (sectors 0-3), Ultralight, and NTAG (all pages)
- **QIDI format decoding** -- displays material name, color with swatch, and manufacturer byte
- **Tag writing** -- write QIDI filament config with verification readback
- **Tag cloning** -- load detected tag data into the writer with one click
- **Tag clearing** -- zero out writable data on Classic or Ultralight/NTAG cards
- **Full hex dump** -- raw data view with block/page labels for any card type
- **WiFi provisioning** -- captive portal setup, no hardcoded credentials
- **RGB LED indicators** -- visual feedback for scanning, success, errors, and setup mode

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3-DevKitC-1 |
| NFC Module | PN532 (I2C mode) |
| Protocol | ISO/IEC 14443-A, 13.56 MHz |
| LED | Onboard WS2812 RGB (GPIO 48) |

### Wiring

Set the PN532 DIP switches to I2C mode: **SW1=ON, SW2=OFF**

| PN532 Pin | ESP32-S3 Pin |
|-----------|-------------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)

### Build & Flash

```bash
pio run -t upload
```

### WiFi Setup

On first boot (or after a WiFi reset), the device starts an access point:

1. Connect to the **`QidiBox-Setup`** WiFi network from your phone or computer
2. A captive portal opens automatically -- enter your WiFi SSID and password
3. The device restarts and connects to your network
4. Open the device IP (shown in serial output) in a browser

### Reconfiguring WiFi

Three options:

- **Web UI** -- click "WiFi Settings" in the page footer
- **BOOT button** -- hold GPIO 0 during power-on to clear saved credentials
- **Auto-fallback** -- if the saved network is unreachable, the setup AP starts automatically

## LED Status

| Color | Meaning |
|-------|---------|
| Purple | AP/setup mode (waiting for WiFi config) |
| Yellow | Connecting to WiFi / reading or writing a tag |
| Blue | Connected, scanning for tags |
| Green (3s) | Tag read or write succeeded |
| Red (2s) | Error (auth failed, read/write failed) |
| Flashing red | PN532 not detected (check wiring) |

## Project Structure

```
src/main.cpp         C++ firmware (NFC logic, web server, WiFi)
data/index.html      Main web UI (HTML structure)
data/style.css       Main web UI (styles)
data/app.js          Main web UI (tag display, polling, write/clear)
data/setup.html      WiFi provisioning captive portal page
platformio.ini       PlatformIO config and dependencies
```

Web assets in `data/` are embedded into the firmware binary at compile time via `board_build.embed_txtfiles` -- no filesystem partition needed.

## Web API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main web UI |
| `/style.css` | GET | Stylesheet |
| `/app.js` | GET | JavaScript |
| `/status` | GET | Last-read tag data (JSON) |
| `/write?material=N&color=N` | GET | Write QIDI data to tag |
| `/clear` | GET | Zero out tag data |
| `/wifi-setup` | GET | WiFi configuration page |
| `/wifi-save` | POST | Save WiFi credentials and restart |

## Supported Cards

| Card Type | Read | Write | Clear |
|-----------|------|-------|-------|
| MIFARE Classic 1K | Sectors 0-3 | Block 4 (QIDI data) | Blocks 4-6 |
| MIFARE Ultralight | All pages | -- | User pages (4+) |
| NTAG 213/215/216 | Up to 44 pages | -- | User pages (4+) |

## Dependencies

Managed automatically by PlatformIO:

- [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532) -- NFC reader/writer
- [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO) -- I2C abstraction
- [ESPAsyncWebServer](https://github.com/mathieucarbou/ESPAsyncWebServer) -- Async HTTP server
- [AsyncTCP](https://github.com/esphome/AsyncTCP) -- TCP layer for async server
- DNSServer, Preferences -- built into ESP32 Arduino framework

## License

MIT
