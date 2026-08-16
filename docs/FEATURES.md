# ESP32 NTRIP DUO - Features Documentation

## Core Features

### 🌐 NTRIP Protocol Support
- **Dual NTRIP Server**: Simultaneous connection to two different NTRIP casters
- **NTRIP Client**: Receive correction data from NTRIP servers
- **Protocol Modes**: NTRIP 2.0-style GET for the rover client and legacy `SOURCE` upload for base-station streams
- **Automatic Reconnection**: Robust connection management with retry logic

### 📡 GNSS Integration  
- **Serial Commands**: Remote configuration of GNSS receivers via web interface
- **UART Interface**: Configurable baud rates, parity, and flow control
- **Data Forwarding**: Transparent data bridge between GNSS and network
- **Status Monitoring**: Real-time connection and data flow indicators

### 💾 Data Logging
- **SD Card Support**: Automatic logging of RTCM correction data
- **Daily Rotation**: New log files created daily (YYYYMMDD.rtcm format)
- **Web Control**: Enable/disable logging via web interface
- **Buffered Writes**: UART events are queued before SD writes so network forwarding is not blocked

### 🌐 Network Connectivity
- **WiFi Station**: Connect to existing WiFi networks
- **WiFi Access Point**: Create an open `ntrip-DUO_danusha` hotspot for initial configuration
- **Dual Mode**: Simultaneous AP and STA operation
- **Static IP**: Support for static IP configuration

### 🎛️ Web Interface
- **Configuration Panel**: Complete device setup via web browser
- **Real-time Status**: Live connection status and data statistics
- **Serial Terminal**: Send commands directly to GNSS receiver
- **Network Scanner**: WiFi network discovery and connection
- **Firmware Updates**: Flash over serial or with a compatible web flasher; OTA upload is not implemented

### 💡 Status Indication
- **RGB LED**: Multi-color status indication
- **Individual LEDs**: RSSI strength, sleep mode, association status  
- **Configurable Colors**: Custom color schemes for different states
- **Brightness Control**: PWM-based intensity control

## Advanced Features

### 🔧 Configuration Management
- **Non-volatile Storage**: Settings preserved across reboots
- **Factory Reset**: Return to default configuration
- **Parameter Validation**: Input validation and error handling

### 🛡️ Security & Authentication
- **Web Authentication**: Configurable username/password protection
- **NTRIP Authentication**: Support for caster authentication
- **Transport Security**: The built-in UI uses HTTP and NTRIP/socket connections are plaintext; deploy only on a trusted network
- **Access Control**: Optional HTTP Basic authentication for the web UI

### 📊 Monitoring & Diagnostics
- **Stream Statistics**: Data throughput and connection metrics
- **Error Reporting**: Detailed error logs and status codes
- **Core Dump**: Crash dump analysis for debugging
- **Memory Monitor**: Heap usage and memory leak detection

### 🔄 Multi-target Support
- **ESP32**: Original ESP32 with proven stability
- **ESP32-S3**: Enhanced performance with additional GPIO
- **ESP32-C6**: Latest generation with WiFi 6 support
- **Conditional Compilation**: Target-specific optimizations

## Use Cases

### 📍 RTK Base Station
- Receive GNSS data from rover
- Forward corrections to NTRIP caster
- Log raw data for post-processing
- Status monitoring and remote control

### 📱 RTK Rover Support  
- Connect to NTRIP servers for corrections
- Forward corrections to GNSS receiver
- Real-time position accuracy improvement
- Mobile hotspot for field operations

### 🗂️ Data Collection
- Long-term GNSS data logging
- Automated file management
- Remote monitoring and control
- Integration with survey workflows

### 🔧 GNSS Configuration
- Remote receiver setup and testing
- Configuration backup and restore
- Multi-receiver management
- Firmware update coordination

## Technical Specifications

### Performance
- **Data Throughput**: Depends on UART baud rate, WiFi quality and the number of enabled outputs
- **Concurrent Connections**: Multiple NTRIP sessions
- **Memory Usage**: Optimized for embedded constraints

### Compatibility
- **GNSS Receivers**: Universal UART interface
- **NTRIP Casters**: Standard NTRIP 2.0 protocol
- **Browsers**: Modern web browser compatibility
- **Mobile Devices**: Responsive web interface

### Limitations
- **WiFi Range**: Standard 802.11 limitations
- **SD Card**: FAT32 file system requirement  
- **Concurrent Users**: Limited by available memory
- **Encryption**: HTTPS/TLS is not implemented by this firmware
