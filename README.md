# ESP32 NTRIP Duo

[![Build Test](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-test.yml/badge.svg)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-test.yml)
[![Build and Release](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-release.yml/badge.svg)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-release.yml)
[![GitHub release](https://img.shields.io/github/v/release/danusha2345/esp32-ntrip-DUO_danusha)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/releases)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

ESP32 NTRIP Duo forwards a GNSS receiver UART over Wi-Fi. It supports two simultaneous NTRIP source connections, an NTRIP client, TCP/UDP server and client modes, a web interface, status LEDs, and optional SD-card logging.

Supported targets: ESP32, ESP32-C3, ESP32-S3, and ESP32-C6. The tested toolchain is ESP-IDF 5.4.4.

## Install a release

1. Download the archive for the exact chip from [Releases](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/releases).
2. Extract it and select `<target>-ntrip-duo-merged.bin`.
3. Flash the merged image at offset `0x0`, for example with [ESPHome Web](https://web.esphome.io/).

Do not flash the application-only `<target>-ntrip-duo.bin` at `0x0`. The individual binaries are included only for tools that use every address listed in the archive README.

After flashing, connect to the open Wi-Fi network `ntrip-DUO_danusha` and open `http://192.168.4.1`.

## Build from source

Install [ESP-IDF 5.4.4](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32/get-started/index.html), then load its environment and build:

```bash
jj git clone https://github.com/danusha2345/esp32-ntrip-DUO_danusha.git
cd esp32-ntrip-DUO_danusha
source "$HOME/esp/esp-idf/export.sh" # adjust to your ESP-IDF location

TARGET=esp32 # esp32, esp32c3, esp32s3, or esp32c6
idf.py set-target "$TARGET"
if [ -f "sdkconfig.$TARGET" ]; then cp "sdkconfig.$TARGET" sdkconfig; fi
idf.py build
idf.py flash
```

Run `./build_all.sh` to build release archives for all four targets.

## Default pinout

UART is configured as 115200 baud, 8N1, with RTS/CTS disabled. Cross TX and RX between the ESP and GNSS receiver.

| Target | UART | ESP TX → GNSS RX | ESP RX ← GNSS TX | RTS | CTS |
|---|---:|---:|---:|---:|---:|
| ESP32-WROOM | UART2 | GPIO17 | GPIO16 | GPIO32 | GPIO33 |
| ESP32-C3 | UART0 | GPIO21 | GPIO20 | GPIO5 | GPIO6 |
| ESP32-S3 | UART0 | GPIO43 | GPIO44 | GPIO16 | GPIO17 |
| ESP32-C6 | UART0 | GPIO16 | GPIO17 | GPIO22 | GPIO23 |

RTS and CTS are assigned only when their hardware-flow-control options are enabled.

### ESP32-WROOM board shown in the wiring diagram

Use the `esp32` firmware and wire the UM980 evaluation/breakout board as follows:

| ESP32-WROOM board | UM980eb/breakout |
|---|---|
| GPIO17 (TX2) | RX |
| GPIO16 (RX2) | TX |
| GND | GND |
| 5V | VIN, only when the carrier accepts 5 V |

The UM980eb evaluation board accepts a 3.2–5.0 V supply. A bare UM980 module accepts only 3.0–3.6 V; never connect its supply pin directly to 5 V. Check the marking and documentation of a different carrier before powering it.

Do not use GPIO1/GPIO3 for this connection: they are UART0 and share the board's USB-to-UART bridge. Do not connect indicator LEDs to UART TX or RX. GPIO6–GPIO11 are connected to the ESP32-WROOM flash and must not be used. On ESP32-WROVER modules GPIO16/GPIO17 are normally used by PSRAM, so remap the UART in the web interface before wiring that module.

Existing installations retain UART settings stored in NVS. After updating, select UART2/GPIO17/GPIO16 in the web interface or erase NVS/factory-reset to receive the new ESP32-WROOM defaults.

### LEDs

The RGB status LED must be common-anode: connect its common pin to 3.3 V and each colour cathode through its own 220 Ω resistor to the GPIO.

| Target | RGB red | RGB green | RGB blue | RSSI | Sleep | Assoc |
|---|---:|---:|---:|---:|---:|---:|
| ESP32 | GPIO21 | GPIO22 | GPIO23 | GPIO18 | GPIO27 | GPIO25 |
| ESP32-C3 | GPIO8 | GPIO9 | GPIO10 | GPIO2 | GPIO3 | GPIO4 |
| ESP32-S3 | GPIO4 | GPIO5 | GPIO6 | GPIO18 | GPIO21 | GPIO47 |
| ESP32-C6 | GPIO4 | GPIO5 | GPIO6 | GPIO18 | GPIO19 | GPIO20 |

### SD card logging

| Target | MISO | MOSI | CLK | CS |
|---|---:|---:|---:|---:|
| ESP32-C3 | GPIO7 | GPIO15 | GPIO14 | GPIO13 |
| ESP32, ESP32-S3, ESP32-C6 | GPIO2 | GPIO15 | GPIO14 | GPIO13 |

All SD pins can be changed under `SD logger configuration` in `menuconfig`. On ESP32, GPIO2 and GPIO15 are strapping pins; avoid external pull resistors that force an invalid boot mode.

## Security defaults

The access point is open, admin authentication is disabled, and the web interface uses unencrypted HTTP by default. Configure Wi-Fi and enable admin authentication before using the device on an untrusted network. HTTP Basic authentication protects access but does not encrypt credentials or traffic.

## License

Licensed under [GPL-3.0](LICENSE). Based on [ESP32 XBee](https://github.com/nebkat/esp32-xbee) by Nebojsa Cvetkovic.
