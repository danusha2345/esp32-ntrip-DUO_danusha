# GPIO Pin Mapping for Different ESP32 Variants

This document describes the GPIO pin assignments for different ESP32 chip variants supported by ESP32 NTRIP DUO.

## ESP32 (Original)

### Status LEDs
- **Red LED**: GPIO 21
- **Green LED**: GPIO 22  
- **Blue LED**: GPIO 23
- **RSSI LED**: GPIO 18
- **Sleep LED**: GPIO 27
- **Association LED**: GPIO 25

### UART Interface (Default)
- **TX**: GPIO 1
- **RX**: GPIO 3
- **RTS**: GPIO 14 (optional)
- **CTS**: GPIO 33 (optional)

### Control
- **Reset Button**: GPIO 0

---

## ESP32-C3

### Status LEDs
- **Red LED**: GPIO 8
- **Green LED**: GPIO 9
- **Blue LED**: GPIO 10
- **RSSI LED**: GPIO 2
- **Sleep LED**: GPIO 3
- **Association LED**: GPIO 4

### UART Interface (Default)
- **TX**: GPIO 21
- **RX**: GPIO 20
- **RTS**: GPIO 5 (optional)
- **CTS**: GPIO 6 (optional)

### Control
- **Reset Button**: GPIO 0

---

## ESP32-S3

### Status LEDs
- **Red LED**: GPIO 4
- **Green LED**: GPIO 5
- **Blue LED**: GPIO 6
- **RSSI LED**: GPIO 18
- **Sleep LED**: GPIO 21
- **Association LED**: GPIO 47

### UART Interface (Default)
- **TX**: GPIO 43
- **RX**: GPIO 44
- **RTS**: GPIO 16 (optional)
- **CTS**: GPIO 15 (optional)

### Control
- **Reset Button**: GPIO 0

---

## ESP32-C6

### Status LEDs
- **Red LED**: GPIO 4
- **Green LED**: GPIO 5
- **Blue LED**: GPIO 6
- **RSSI LED**: GPIO 18
- **Sleep LED**: GPIO 19
- **Association LED**: GPIO 20

### UART Interface (Default)
- **TX**: GPIO 16
- **RX**: GPIO 17
- **RTS**: GPIO 4 (optional)
- **CTS**: GPIO 5 (optional)

### Control
- **Reset Button**: GPIO 9

---

## Notes

1. **LED Polarity**: All LEDs are configured as active-low (common anode)
2. **UART Configuration**: Can be changed via web interface
3. **Flow Control**: RTS/CTS pins are optional and can be disabled
4. **Power Requirements**: 3.3V logic levels for all GPIO
5. **Current Limits**: LEDs should be connected through appropriate current-limiting resistors (typically 220-330Ω)

## Schematic Recommendations

### LED Connection Example
```
ESP32_GPIO ----[220Ω]----[LED]----GND
```

### UART Connection to GNSS
```
ESP32_TX  -----> GNSS_RX
ESP32_RX  <----- GNSS_TX  
ESP32_RTS -----> GNSS_CTS (optional)
ESP32_CTS <----- GNSS_RTS (optional)
GND       <----> GND
```
