#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// SD card pinout is configurable in menuconfig. ESP32-C3 defaults MISO to
// GPIO7 because GPIO2 is reserved for its RSSI status LED.
#define PIN_NUM_MISO    CONFIG_SD_LOGGER_MISO_GPIO
#define PIN_NUM_MOSI    CONFIG_SD_LOGGER_MOSI_GPIO
#define PIN_NUM_CLK     CONFIG_SD_LOGGER_CLK_GPIO
#define PIN_NUM_CS      CONFIG_SD_LOGGER_CS_GPIO

#define MOUNT_POINT "/sdcard"

/**
 * Initialize SD card and file system
 * @return ESP_OK on success
 */
esp_err_t sd_logger_init(void);

/**
 * Enable or disable SD logging
 * @param enable true to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t sd_logger_enable(bool enable);

/**
 * Check if SD logging is enabled
 * @return true if enabled, false otherwise
 */
bool sd_logger_is_enabled(void);

/**
 * Check if a new log file needs to be created (new day)
 * @return ESP_OK on success
 */
esp_err_t sd_logger_check_date(void);

/**
 * Write data to SD card log file
 * @param data pointer to data
 * @param len length of data
 * @return ESP_OK on success
 */
esp_err_t sd_logger_write(const uint8_t *data, size_t len);

/**
 * Deinitialize SD card logger
 */
void sd_logger_deinit(void);

#endif // SD_LOGGER_H
