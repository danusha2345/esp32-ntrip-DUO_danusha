# ESP32 NTRIP Duo

[![Build Test](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-test.yml/badge.svg)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-test.yml)
[![Build and Release](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-release.yml/badge.svg)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-release.yml)
[![GitHub release](https://img.shields.io/github/v/release/danusha2345/esp32-ntrip-DUO_danusha)](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/releases)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

This is modified version of ESP32 xbee.

Main difference is that this have two NTRIP servers that can be running at the same time to be able to feed Onocoy and RTK Direct!!!

ESP32 NTRIP Duo is made with [ESP-IDF](https://github.com/espressif/esp-idf). Its main function is to forward the UART of the ESP32 to a variety of protocols over WiFi.

In this version Installation is simplified, with just a single bin file. You can use ESPHome web flasher https://web.esphome.io/ just connect your ESP32 dev board to PC, select connect, cho0se correct COM port, connect and install. On popup choose bin file and click install.

This software can run on ESP32 WROOM type. Now added ESP32S3, ESP32C3, ESP32S2  Just choose correct bin file!
For ESP32S2 if ESPHome is not connecting (it didn't with S2mini) You can use https://tasmota.github.io/install/ to install with single bin file.
## Features
- **WiFi Station & Hotspot** - Connect to existing WiFi network or create your own access point
- **Web Interface** - Complete configuration through browser-based interface
- **Dual NTRIP Servers** - Simultaneous connection to two NTRIP services (e.g., Onocoy and RTK Direct)
- **TCP/UDP Socket Server** - Host socket services for client connections
- **TCP/UDP Socket Client** - Connect to external socket servers
- **UART Configuration** - Full control over serial communication parameters
- **SD Card Logging** - Log RTCM data to SD card with daily file rotation
- **Status LED Control** - Visual feedback with RGB LED support
- **Serial Commands** - Send commands directly through web interface
- **Multi-platform Support** - ESP32, ESP32-S3, ESP32-C3, ESP32-C6


## Help
Now It can be compiled using ESP-IDF 5.4. (with some depreciation comments)

To install the latest firmware use ESPHome web Flasher https://web.esphome.io/

Here is installation video https://youtu.be/33Mu5EV7fOE?si=J6kwCt6bbmIu7HnS

It is still work in progress!!!

## Pinout
By default it is set for UART0 TX gpio1, RX gpio3 including ESP32S3

LED with common positive and low output:

ESP32: gpio21 Red, gpio22 Green, gpio23 Blue

ESP32S3: gpio4 Red, gpio5 Green, gpio6 Blue
![IMG_20250212_000825](https://github.com/user-attachments/assets/f17d28dc-4bc7-4647-8311-7a1c44526d17)

## Quick Start

### 📥 Download Pre-built Firmware
1. Go to [Releases](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/releases)
2. Download the appropriate firmware for your ESP32 variant:
   - `esp32-ntrip-duo-esp32-*.tar.gz` for ESP32
   - `esp32-ntrip-duo-esp32s3-*.tar.gz` for ESP32-S3  
   - `esp32-ntrip-duo-esp32c6-*.tar.gz` for ESP32-C6
3. Extract and follow the included README for flashing instructions

### 🔧 Build from Source
```bash
git clone https://github.com/danusha2345/esp32-ntrip-DUO_danusha.git
cd esp32-ntrip-DUO_danusha
git submodule update --init --recursive

# Setup ESP-IDF (if not already done)
. $HOME/esp/esp-idf/export.sh

# Build for your target
idf.py set-target esp32     # or esp32s3, esp32c6
idf.py build
idf.py flash
```

### 🌐 First Time Configuration
1. Connect to **ESP32_NTRIP** WiFi network (password: `12345678`)
2. Open browser and navigate to http://192.168.4.1
3. Configure your WiFi network and NTRIP settings
4. Connect your GNSS receiver to the configured UART pins

## 🔌 Hardware Setup

### Supported Boards
- **ESP32**: Original ESP32 development boards
- **ESP32-C3**: ESP32-C3 with RISC-V core and USB JTAG
- **ESP32-S3**: ESP32-S3 based boards with enhanced GPIO
- **ESP32-C6**: Latest ESP32-C6 with WiFi 6 support

### Pin Connections

**UART (Default):**
- TX: GPIO1, RX: GPIO3 (all variants)

**Status LED (Common Anode RGB):**
- **ESP32**: Red=GPIO21, Green=GPIO22, Blue=GPIO23
- **ESP32-S3**: Red=GPIO4, Green=GPIO5, Blue=GPIO6

**SD Card (Optional for logging):**
- MISO: GPIO2, MOSI: GPIO15, CLK: GPIO14, CS: GPIO13

**Basic Connection:**
- Connect GNSS TX to ESP32 RX pin
- Connect GNSS RX to ESP32 TX pin  
- Connect GND to GND
- Optional: Status LEDs on configured GPIO pins
- Optional: SD card for data logging

## ⚙️ Configuration

### Web Interface Features
- **Network Configuration** - WiFi Station and Hotspot settings
- **NTRIP Servers** - Configure dual NTRIP server connections  
- **Socket Services** - TCP/UDP Server and Client configuration
- **UART Settings** - Baud rate, data bits, parity, stop bits
- **Serial Commands** - Send AT commands or custom data
- **SD Card Logging** - Enable/disable data logging with status display
- **Admin Panel** - Security and access control
- **Status Monitoring** - Real-time connection and data flow status

### Socket Server/Client
The firmware includes full TCP/UDP socket functionality:
- **Socket Server**: Host services on configurable ports (TCP/UDP)
- **Socket Client**: Connect to remote socket servers
- **Bidirectional Data Flow**: UART ↔ Socket data forwarding
- **Multiple Clients**: Server supports multiple concurrent connections
- **IPv6 Support**: Dual-stack IPv4/IPv6 compatibility

## 🔌 Hardware Setup

### Supported Boards
- **ESP32**: Original ESP32 development boards
- **ESP32-C3**: ESP32-C3 with RISC-V core and USB JTAG
- **ESP32-S3**: ESP32-S3 based boards with enhanced GPIO
- **ESP32-C6**: Latest ESP32-C6 with WiFi 6 support

### Pin Connections
See [PIN_MAPPING.md](docs/PIN_MAPPING.md) for detailed GPIO assignments.

**Basic Connection:**
- Connect GNSS TX to ESP32 RX pin
- Connect GNSS RX to ESP32 TX pin  
- Connect GND to GND
- Optional: Status LEDs on configured GPIO pins
- Optional: SD card for data logging

## 🔌 Hardware Setup

### Supported Boards
- **ESP32**: Original ESP32 development boards
- **ESP32-C3**: ESP32-C3 with RISC-V core and USB JTAG
- **ESP32-S3**: ESP32-S3 based boards with enhanced GPIO
- **ESP32-C6**: Latest ESP32-C6 with WiFi 6 support

### Pin Connections
See [PIN_MAPPING.md](docs/PIN_MAPPING.md) for detailed GPIO assignments.

**Basic Connection:**
- Connect GNSS TX to ESP32 RX pin
- Connect GNSS RX to ESP32 TX pin  
- Connect GND to GND
- Optional: Status LEDs on configured GPIO pins
- Optional: SD card for data logging

## 🔌 Hardware Setup

### Supported Boards
- **ESP32**: Original ESP32 development boards
- **ESP32-C3**: ESP32-C3 with RISC-V core and USB JTAG
- **ESP32-S3**: ESP32-S3 based boards with enhanced GPIO
- **ESP32-C6**: Latest ESP32-C6 with WiFi 6 support

### Pin Connections
See [PIN_MAPPING.md](docs/PIN_MAPPING.md) for detailed GPIO assignments.

**Basic Connection:**
- Connect GNSS TX to ESP32 RX pin
- Connect GNSS RX to ESP32 TX pin  
- Connect GND to GND
- Optional: Status LEDs on configured GPIO pins
- Optional: SD card for data logging
