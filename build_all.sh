#!/bin/bash

# ESP32 NTRIP DUO - Multi-target Build Script
# Builds firmware for ESP32, ESP32-S3, and ESP32-C6

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Build configuration
TARGETS=("esp32" "esp32c3" "esp32s3" "esp32c6")
BUILD_DIR="build_output"
DATE=$(date +%Y%m%d)

echo -e "${BLUE}ESP32 NTRIP DUO Multi-target Build Script${NC}"
echo -e "${BLUE}=======================================${NC}"

# Check ESP-IDF environment
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF environment not set. Run 'source \$IDF_PATH/export.sh' first.${NC}"
    exit 1
fi

echo -e "${GREEN}ESP-IDF Path: $IDF_PATH${NC}"
echo -e "${GREEN}Build Date: $DATE${NC}"
echo

# Create build output directory
mkdir -p $BUILD_DIR
rm -rf $BUILD_DIR/*

# Function to build for a specific target
build_target() {
    local target=$1
    local target_upper=$(echo $target | tr '[:lower:]' '[:upper:]')
    
    echo -e "${YELLOW}Building for $target_upper...${NC}"
    echo -e "${YELLOW}=========================${NC}"
    
    # Clean previous build
    if [ -d "build/" ]; then
        echo -e "${BLUE}Cleaning build directory...${NC}"
        chmod -R 755 build/ 2>/dev/null || true
        rm -rf build/ || {
            echo -e "${YELLOW}Warning: Could not remove build directory completely. Continuing...${NC}"
            # Try alternative cleanup
            find build/ -type f -exec chmod 644 {} \; 2>/dev/null || true
            find build/ -type d -exec chmod 755 {} \; 2>/dev/null || true
            rm -rf build/* 2>/dev/null || true
        }
    fi
    
    # Set target
    echo -e "${BLUE}Setting target to $target...${NC}"
    idf.py set-target $target
    
    # Copy target-specific sdkconfig if exists
    if [ -f "sdkconfig.$target" ]; then
        echo -e "${BLUE}Using target-specific configuration...${NC}"
        cp "sdkconfig.$target" sdkconfig
    fi
    
    # Build
    echo -e "${BLUE}Building firmware...${NC}"
    if idf.py build; then
        echo -e "${GREEN}✓ Build successful for $target_upper${NC}"
        
        # Create target directory
        mkdir -p "$BUILD_DIR/$target"
        
        # Copy binaries (project name changes based on target)
        cp build/bootloader/bootloader.bin "$BUILD_DIR/$target/"
        cp build/partition_table/partition-table.bin "$BUILD_DIR/$target/"
        
        # Find the main firmware files (project name varies by target)
        MAIN_BIN=$(find build/ -maxdepth 1 -name "$target-ntrip-duo.bin" -o -name "esp32-ntrip-duo.bin" | head -1)
        MAIN_ELF=$(find build/ -maxdepth 1 -name "$target-ntrip-duo.elf" -o -name "esp32-ntrip-duo.elf" | head -1)
        
        if [ -n "$MAIN_BIN" ]; then
            cp "$MAIN_BIN" "$BUILD_DIR/$target/"
            echo -e "${GREEN}  Copied main firmware: $(basename $MAIN_BIN)${NC}"
        else
            echo -e "${RED}  Warning: Main firmware .bin file not found${NC}"
        fi
        
        if [ -n "$MAIN_ELF" ]; then
            cp "$MAIN_ELF" "$BUILD_DIR/$target/"
            echo -e "${GREEN}  Copied ELF file: $(basename $MAIN_ELF)${NC}"
        else
            echo -e "${RED}  Warning: Main firmware .elf file not found${NC}"
        fi
        
        # Copy www.bin if exists
        if [ -f "build/www.bin" ]; then
            cp build/www.bin "$BUILD_DIR/$target/"
        fi
        
        # Generate merged firmware (single file)
        echo -e "${BLUE}Generating merged firmware...${NC}"
        if idf.py merge-bin --format hex; then
            MERGED_HEX=$(find build/ -name "*-merged.hex" | head -1)
            if [ -n "$MERGED_HEX" ]; then
                cp "$MERGED_HEX" "$BUILD_DIR/$target/"
                echo -e "${GREEN}  Copied merged hex: $(basename $MERGED_HEX)${NC}"
            fi
        fi
        
        if idf.py merge-bin --format bin; then
            MERGED_BIN=$(find build/ -name "*-merged.bin" | head -1)
            if [ -n "$MERGED_BIN" ]; then
                cp "$MERGED_BIN" "$BUILD_DIR/$target/"
                echo -e "${GREEN}  Copied merged bin: $(basename $MERGED_BIN)${NC}"
            fi
        fi
        
        # Generate flash script
        cat > "$BUILD_DIR/$target/flash.sh" << EOF
#!/bin/bash
# Flash script for ESP32 NTRIP DUO - $target_upper

PORT=\${1:-/dev/ttyUSB0}
BAUD=\${2:-460800}

echo "Flashing ESP32 NTRIP DUO firmware for $target_upper..."
echo "Port: \$PORT, Baud: \$BAUD"

python -m esptool \\
    --chip $target \\
    --port \$PORT \\
    --baud \$BAUD \\
    --before default_reset \\
    --after hard_reset \\
    write_flash \\
    --flash_mode dio \\
    --flash_size detect \\
    --flash_freq 40m \\
    0x1000 bootloader.bin \\
    0x8000 partition-table.bin \\
    0x10000 esp32-xbee.bin \\
    0x210000 www.bin

echo "Flash complete! Connect to ESP32_NTRIP WiFi network (password: 12345678)"
echo "Then navigate to http://192.168.4.1 to configure."
EOF
        
        chmod +x "$BUILD_DIR/$target/flash.sh"
        
        # Generate Windows batch file
        cat > "$BUILD_DIR/$target/flash.bat" << 'EOF'
@echo off
set PORT=%1
set BAUD=%2

if "%PORT%"=="" set PORT=COM3
if "%BAUD%"=="" set BAUD=460800

echo Flashing ESP32 NTRIP DUO firmware...
echo Port: %PORT%, Baud: %BAUD%

python -m esptool ^
    --chip TARGET_CHIP ^
    --port %PORT% ^
    --baud %BAUD% ^
    --before default_reset ^
    --after hard_reset ^
    write_flash ^
    --flash_mode dio ^
    --flash_size detect ^
    --flash_freq 40m ^
    0x1000 bootloader.bin ^
    0x8000 partition-table.bin ^
    0x10000 esp32-xbee.bin ^
    0x210000 www.bin

echo Flash complete! Connect to ESP32_NTRIP WiFi network (password: 12345678)
echo Then navigate to http://192.168.4.1 to configure.
pause
EOF
        
        # Replace TARGET_CHIP with actual target
        sed -i "s/TARGET_CHIP/$target/g" "$BUILD_DIR/$target/flash.bat"
        
        # Generate README
        cat > "$BUILD_DIR/$target/README.md" << EOF
# ESP32 NTRIP DUO - $target_upper Firmware

## Files
- \`bootloader.bin\` - ESP32 bootloader
- \`partition-table.bin\` - Partition table  
- \`esp32-xbee.bin\` - Main application firmware
- \`www.bin\` - Web interface files
- \`esp32-xbee.elf\` - ELF file for debugging
- \`flash.sh\` - Linux/Mac flash script
- \`flash.bat\` - Windows flash script

## Hardware Setup ($target_upper)
EOF

        # Add target-specific pin mappings
        case $target in
            "esp32")
                cat >> "$BUILD_DIR/$target/README.md" << 'EOF'

### GPIO Pin Assignment
- **Status LEDs:** Red=GPIO21, Green=GPIO22, Blue=GPIO23
- **Additional LEDs:** RSSI=GPIO18, Sleep=GPIO27, Assoc=GPIO25  
- **UART:** TX=GPIO1, RX=GPIO3, RTS=GPIO14, CTS=GPIO33
- **Button:** GPIO0

EOF
                ;;
            "esp32c3")
                cat >> "$BUILD_DIR/$target/README.md" << 'EOF'

### GPIO Pin Assignment
- **Status LEDs:** Red=GPIO8, Green=GPIO9, Blue=GPIO10
- **Additional LEDs:** RSSI=GPIO2, Sleep=GPIO3, Assoc=GPIO4
- **UART:** TX=GPIO21, RX=GPIO20, RTS=GPIO5, CTS=GPIO6
- **Button:** GPIO0

### ESP32-C3 Features
- RISC-V single-core processor
- USB Serial JTAG for debugging
- Enhanced security features
- Lower power consumption

EOF
                ;;
            "esp32s3")
                cat >> "$BUILD_DIR/$target/README.md" << 'EOF'

### GPIO Pin Assignment  
- **Status LEDs:** Red=GPIO4, Green=GPIO5, Blue=GPIO6
- **Additional LEDs:** RSSI=GPIO18, Sleep=GPIO21, Assoc=GPIO47
- **UART:** TX=GPIO43, RX=GPIO44, RTS=GPIO16, CTS=GPIO15
- **Button:** GPIO0

### ESP32-S3 Features
- PSRAM support enabled
- USB Console available
- Enhanced WiFi performance

EOF
                ;;
            "esp32c6")
                cat >> "$BUILD_DIR/$target/README.md" << 'EOF'

### GPIO Pin Assignment
- **Status LEDs:** Red=GPIO4, Green=GPIO5, Blue=GPIO6  
- **Additional LEDs:** RSSI=GPIO18, Sleep=GPIO19, Assoc=GPIO20
- **UART:** TX=GPIO16, RX=GPIO17, RTS=GPIO4, CTS=GPIO5
- **Button:** GPIO9

### ESP32-C6 Features
- WiFi 6 support
- Enhanced security features
- Lower power consumption

EOF
                ;;
        esac
        
        cat >> "$BUILD_DIR/$target/README.md" << 'EOF'
## Flash Instructions

### Linux/Mac
```bash
./flash.sh /dev/ttyUSB0 460800
```

### Windows  
```cmd
flash.bat COM3 460800
```

### Manual (esptool.py)
```bash
pip install esptool
python -m esptool --chip TARGET --port PORT -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 esp32-xbee.bin 0x210000 www.bin
```

## First Time Setup
1. Flash the firmware using one of the methods above
2. Connect to **ESP32_NTRIP** WiFi network (password: `12345678`)
3. Open browser and navigate to http://192.168.4.1  
4. Configure WiFi, NTRIP servers, and other settings
5. Connect GNSS receiver to configured UART pins

## Features
- Dual NTRIP server support
- Serial Commands for GNSS configuration
- SD Card data logging  
- Web-based configuration
- Status LED indicators
- Over-the-air updates

For more information visit: https://github.com/danusha2345/esp32-ntrip-DUO_danusha
EOF
        
        # Replace TARGET placeholder with actual target
        sed -i "s/TARGET/$target/g" "$BUILD_DIR/$target/README.md"
        
        # Get build size info
        APP_SIZE=$(stat -f%z "build/esp32-xbee.bin" 2>/dev/null || stat -c%s "build/esp32-xbee.bin")
        BOOT_SIZE=$(stat -f%z "build/bootloader/bootloader.bin" 2>/dev/null || stat -c%s "build/bootloader/bootloader.bin")
        
        echo -e "${GREEN}  Application size: $(($APP_SIZE / 1024)) KB${NC}"
        echo -e "${GREEN}  Bootloader size: $(($BOOT_SIZE / 1024)) KB${NC}"
        echo
        
    else
        echo -e "${RED}✗ Build failed for $target_upper${NC}"
        return 1
    fi
}

# Build for all targets
success_count=0
total_count=${#TARGETS[@]}

for target in "${TARGETS[@]}"; do
    echo
    if build_target $target; then
        ((success_count++))
    fi
done

# Create combined archive  
echo -e "${BLUE}Creating release archives...${NC}"
cd $BUILD_DIR

for target in "${TARGETS[@]}"; do
    if [ -d "$target" ]; then
        tar -czf "esp32-ntrip-duo-$target-$DATE.tar.gz" "$target/"
        echo -e "${GREEN}✓ Created esp32-ntrip-duo-$target-$DATE.tar.gz${NC}"
    fi
done

# Create combined archive with all targets
tar -czf "esp32-ntrip-duo-all-$DATE.tar.gz" */
echo -e "${GREEN}✓ Created esp32-ntrip-duo-all-$DATE.tar.gz${NC}"

cd ..

# Summary
echo
echo -e "${BLUE}Build Summary${NC}"
echo -e "${BLUE}=============${NC}"
echo -e "${GREEN}Successful builds: $success_count/$total_count${NC}"

if [ $success_count -eq $total_count ]; then
    echo -e "${GREEN}✓ All builds completed successfully!${NC}"
    echo -e "${GREEN}✓ Release files created in $BUILD_DIR/${NC}"
else
    echo -e "${YELLOW}⚠ Some builds failed. Check the output above.${NC}"
fi

echo
# Copy firmware directories to firmware_releases directory
echo -e "${BLUE}Copying firmware builds to firmware_releases...${NC}"
mkdir -p firmware_releases

# Copy individual target builds to firmware_releases
for target in "${TARGETS[@]}"; do
    if [ -d "$BUILD_DIR/$target" ]; then
        echo -e "${BLUE}Copying $target firmware to firmware_releases...${NC}"
        rm -rf "firmware_releases/$target" 2>/dev/null || true
        cp -r "$BUILD_DIR/$target" "firmware_releases/"
        echo -e "${GREEN}✓ Copied $target firmware to firmware_releases/$target/${NC}"
    fi
done

# Also copy archive files for releases
cp -f $BUILD_DIR/*.tar.gz firmware_releases/ 2>/dev/null || true
echo -e "${GREEN}✓ All firmware builds copied to firmware_releases/${NC}"

echo -e "${BLUE}Release files:${NC}"
ls -lh $BUILD_DIR/*.tar.gz

echo
echo -e "${BLUE}Firmware files in firmware_releases:${NC}"
ls -lh firmware_releases/

echo
echo -e "${BLUE}Next steps:${NC}"
echo "1. Test the firmware on your hardware"  
echo "2. Create a git tag: git tag v$(date +%Y.%m.%d)"
echo "3. Push tag to trigger release: git push origin --tags"
echo "4. GitHub Actions will automatically create release with binaries"