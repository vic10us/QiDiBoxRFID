# Building and Installing QIDI Box NFC

This guide covers the full setup from cloning the repo to a working device on
your bench, plus what to do when something goes wrong.

## Prerequisites

You need:

- **A supported board.** See [README — Supported Boards](../README.md#supported-boards).
- **A PN532 NFC module** in I2C mode (DIP switches: `SW1=ON, SW2=OFF`).
- **Wires** to connect the PN532 to the ESP32 (4 wires: VCC, GND, SDA, SCL).
- **A USB cable** that supports data, not just power.
- **Python 3.10+** (any 3.10/3.11/3.12/3.13 — note: 3.14 is not supported by PlatformIO yet).
- **Git**.

You **don't** need to install ESP-IDF separately. PlatformIO downloads and
manages its own copy.

## Installing PlatformIO

The easiest path is the VS Code extension:

1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. From the Extensions panel, install **PlatformIO IDE** (publisher: `platformio`).
3. Reload VS Code when prompted. PlatformIO will install its toolchain on first launch (~5 minutes the first time).

If you prefer the command line only:

```bash
python -m pip install --upgrade platformio
```

Verify it works:

```bash
pio --version
```

## Cloning the Repository

```bash
git clone https://github.com/vic10us/QidiBoxNFC.git
cd QidiBoxNFC
```

## Wiring the PN532

| PN532 Pin | ESP32 Pin (default)        |
| --------- | -------------------------- |
| VCC       | 3.3V                       |
| GND       | GND                        |
| SDA       | board default — see below  |
| SCL       | board default — see below  |

| Board               | SDA | SCL |
| ------------------- | --- | --- |
| ESP32-S3-DevKitC-1  |   8 |   9 |
| ESP32-C3-DevKitM-1  |   6 |   7 |
| ESP32-C6-DevKitM-1  |   6 |   7 |
| Seeed XIAO ESP32-C6 |  22 |  23 |

These are just the compile-time defaults. You can change them at runtime
without re-flashing — see [Recovery and Reconfiguration](#recovery-and-reconfiguration).

**Important:** Set the PN532 DIP switches to I2C mode (`SW1=ON, SW2=OFF`).
The module will not respond to I2C commands in SPI or HSU mode.

## First Build

Pick the environment that matches your board and run a clean build to
download all dependencies:

```bash
pio run -e esp32-s3-devkitc-1
# or
pio run -e esp32-c3-devkitm-1
# or
pio run -e esp32-c6-devkitm-1
# or
pio run -e seeed_xiao_esp32c6
```

The first build will take several minutes — PlatformIO downloads the
ESP-IDF framework, the toolchain, and the `led_strip` managed component.
Subsequent builds are typically 10-30 seconds.

## Flashing

Connect the board, then:

```bash
pio run -e <env-name> -t upload
```

PlatformIO auto-detects the serial port. If you have multiple devices
plugged in, set the port explicitly in `platformio.ini` for that env:

```ini
[env:my-board]
upload_port = COM5     ; or /dev/ttyUSB0, /dev/cu.usbserial-XXXX
```

### Notes per board

- **ESP32-S3-DevKitC-1** — has two USB ports labeled "UART" and "USB". Use
  the **USB** port (the one wired directly to the chip) so both flashing
  and the serial console work over the same cable.
- **ESP32-C3-DevKitM-1** — single USB port via on-board USB-UART; works
  out of the box.
- **ESP32-C6-DevKitM-1** — uses the chip's built-in USB-Serial-JTAG.
  Should just work.
- **Seeed XIAO ESP32-C6** — single USB-C port via the chip's USB-Serial-JTAG.
  Just works. The board has no on-board USB-UART, so a normal data USB-C
  cable is enough.

## Opening the Serial Monitor

Use the integrated terminal (not the VS Code GUI Serial Monitor — it
intercepts keystrokes and breaks the interactive console):

```bash
pio device monitor -e <env-name>
```

Exit with `Ctrl-C`. Press `Ctrl-T` then `Ctrl-H` for the monitor's own
help menu.

You should see boot logs followed by either a `*** WIFI CONNECTED ***`
banner with your IP, or `*** AP MODE ***` if no credentials are saved.

## Initial WiFi Setup

On first boot the device starts an open access point:

1. Connect any phone or computer to the SSID **`QidiBox-Setup`**.
2. Most operating systems pop up a captive portal automatically. If yours
   doesn't, open `http://192.168.4.1/` in a browser.
3. Enter your home WiFi SSID and password and submit.
4. The device restarts and connects. Open the IP printed on the serial
   console in a browser.

## Configuring the Device

Two interfaces, both fully equivalent. Pick whichever is more convenient.

### Web UI

Open the device's IP in a browser, then click **Settings** in the page
footer (or navigate directly to `http://<device-ip>/settings`).

You can change:

- LED type, pin, length, skip offset, brightness
- Per-state colors (scan / busy / OK / error / setup)
- OK and error display durations
- I2C SDA/SCL pins (changes require restart)
- BOOT button pin

The page tells you which fields require a restart.

### Serial Console

Open the serial monitor and press Enter — you should see a `>` prompt.

Try `help` for the full command list. Useful commands:

```text
status                show IP, NFC ready, I2C pins, LED state
get                   dump current config as JSON
set i2c_sda 6         change SDA pin to GPIO 6
set led_type 1        switch to WS2812 strip
set c_ok #00ff00      change "OK" color to bright green
wifi MyNet pass123    save credentials and restart
wifi clear            forget WiFi (useful for moving the device)
reset                 factory reset hardware config
restart               reboot
```

The console works **even when WiFi is unavailable** — useful for first
setup, debugging, and recovery.

## Recovery and Reconfiguration

### "I configured the wrong I2C pins and now NFC doesn't work"

NFC failures don't halt the firmware. Boot, use the web UI or console to
fix `i2c_sda` / `i2c_scl`, then `restart`.

### "I forgot the WiFi password"

Three options:

- **Hold the BOOT button** during power-on. This wipes WiFi credentials
  *and* hardware config back to defaults. The device restarts in setup AP
  mode.
- **Serial console:** `wifi clear` followed by Enter. The device restarts.
- **Auto-fallback:** if the saved network is simply unreachable, the
  setup AP starts automatically after a 15-second timeout.

### "The device is bricked / won't boot"

Erase the entire flash and re-upload:

```bash
pio run -e <env-name> -t erase
pio run -e <env-name> -t upload
```

## Troubleshooting

### "PN532 not detected on I2C"

- Confirm wiring: VCC → 3.3V, GND → GND, SDA/SCL match the board's defaults
  (or the `i2c_sda`/`i2c_scl` values you've configured).
- Confirm the PN532 DIP switches: `SW1=ON, SW2=OFF` (I2C mode).
- Some PN532 modules need a brief power cycle after applying power.
- Try slower wiring — long jumpers above ~20 cm can cause I2C errors.

### "Flash memory size mismatch detected"

You're using a board variant whose actual flash size doesn't match the
declared one. The build script auto-corrects this for the included
environments. If you're adding a new board, set `board_upload.flash_size`
in `platformio.ini` and the pre-build script will update the IDF binary
header to match.

### "Build fails with `kconfgen` UnicodeDecodeError"

Your `sdkconfig.defaults*` files contain non-ASCII characters that
`kconfgen` can't decode under Windows code pages. Use plain ASCII
(no em-dashes, no smart quotes) in those files. The pre-build scripts
already write ASCII; the warning typically only happens to hand-edited
defaults files.

### "Serial monitor shows boot logs but I can't type"

You're probably using the VS Code GUI Serial Monitor, which doesn't
forward keystrokes. Switch to the integrated terminal:

```bash
pio device monitor -e <env-name>
```

PuTTY and Tera Term also work fine.

### "I see only `ESP-ROM:` boot messages and nothing else (S3 dev kit)"

The S3 dev kit has two USB ports labeled UART and USB. The UART port
shows ROM messages but the firmware routes the runtime console to the
chip's built-in USB-Serial-JTAG, which is on the **USB** port. Switch
the cable to the other port.

## Adding a New Board Variant

1. Add an env to `platformio.ini`:

   ```ini
   [env:my-new-board]
   board = my-board-name
   board_upload.flash_size = 4MB        ; or 8MB / 16MB
   board_build.partitions = partitions_4mb.csv   ; or partitions.csv
   build_flags = -DBOARD_MY_NEW_BOARD             ; optional
   ```

2. If the chip needs different default I2C pins or LED settings, add a
   branch in [`src/config.h`](../src/config.h) under the appropriate
   `CONFIG_IDF_TARGET_*` block, optionally gated by your `BOARD_*` flag.

3. The `fix_flash_size.py` pre-build script will automatically generate
   the correct sdkconfig override for the env's flash size on the next
   build — no manual sdkconfig editing required.

4. Build and flash as usual:

   ```bash
   pio run -e my-new-board -t upload
   ```

## Doing OTA Updates Later

The included partition tables reserve two app slots (`ota_0` and `ota_1`)
on every board, so you don't need to re-partition the chip when you're
ready to add OTA. The current firmware doesn't include an OTA endpoint
yet, but the layout is ready for it.

## Build Internals (for the curious)

- The web assets in `data/` are gzipped at build time by
  [`scripts/gen_web_assets.py`](../scripts/gen_web_assets.py) and
  embedded into [`src/web_assets.h`](../src/) as C string literals. Edit
  the source files normally — the regeneration is automatic and only
  runs when a `data/` file changes.
- [`scripts/fix_flash_size.py`](../scripts/fix_flash_size.py) writes a
  per-env `sdkconfig.defaults.<env>` file matching the board's
  `board_upload.flash_size`. PlatformIO doesn't propagate the upload
  flash size into the IDF binary by default, so this script bridges the
  gap and prevents the "size mismatch" warning.
- The console subsystem ([`src/console.c`](../src/console.c)) installs
  the USB-Serial-JTAG driver and runs a FreeRTOS task that reads bytes
  from the host. Without the explicit driver install, ESP-IDF leaves the
  USB OUT endpoint undrained and host keystrokes never reach the chip.
- All console output is routed through USB-Serial-JTAG via
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in
  [`sdkconfig.defaults`](../sdkconfig.defaults). UART0 is left free for
  application use.
