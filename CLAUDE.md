# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview
ESP32 NTRIP Duo is a firmware for ESP32 microcontrollers that forwards UART data to NTRIP servers over WiFi. The main feature is dual NTRIP server support, allowing simultaneous connections to two different services (e.g., Onocoy and RTK Direct).

## Build System
This project uses ESP-IDF (ESP32 IoT Development Framework) version 5.4:
- `idf.py build` - Build the project
- `idf.py flash` - Flash firmware to device  
- `idf.py monitor` - Monitor device output
- `idf.py flash monitor` - Flash and monitor in one command
- `idf.py menuconfig` - Configure project settings

## Architecture
The project follows ESP-IDF component structure with dual NTRIP server implementation:

### Core Components
- **main/**: Main application code with dual NTRIP servers
  - `ntrip_server.c` - Primary NTRIP server implementation
  - `ntrip_server_2.c` - Secondary NTRIP server for dual functionality
  - `config.c` - Configuration management using NVS storage
  - `wifi.c` - WiFi station and hotspot functionality
  - `uart.c` - UART communication handling
  - `web_server.c` - HTTP server for web interface

- **button/**: Custom component for GPIO button handling
- **www/**: Web interface files (HTML, CSS, JS)

### Key Features
- Dual NTRIP server support (primary difference from original ESP32-XBee)
- Web-based configuration interface
- WiFi station and hotspot modes
- UART data forwarding
- Status LED indicators
- Configuration persistence via NVS

### Hardware Support
- ESP32 WROOM (default UART0: TX=GPIO1, RX=GPIO3)
- ESP32-S3 (same UART, different LED pins: Red=GPIO4, Green=GPIO5, Blue=GPIO6)
- ESP32-C3, ESP32-S2 variants

### Pin Configuration
- **ESP32**: LED - Red: GPIO21, Green: GPIO22, Blue: GPIO23
- **ESP32-S3**: LED - Red: GPIO4, Green: GPIO5, Blue: GPIO6
- **UART**: TX: GPIO1, RX: GPIO3 (all variants)

## Development Notes
- Configuration stored in NVS (Non-Volatile Storage)
- Status LED provides visual feedback for connection states
- Button on GPIO0 for factory reset functionality
- Two independent NTRIP server tasks can run simultaneously
- Web interface accessible when in hotspot mode