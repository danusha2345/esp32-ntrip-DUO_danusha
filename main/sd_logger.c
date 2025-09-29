#include "sd_logger.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "config.h"
#include <time.h>
#include <sys/stat.h>
#include <string.h>

static const char *TAG = "SD_LOGGER";

static FILE *log_file = NULL;
static char current_date[16] = {0};
static bool logging_enabled = false;
static sdmmc_card_t *card = NULL;

esp_err_t sd_logger_init(void) {
    esp_err_t ret;
    
    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "Initializing SD card");

    // Use SPI mode
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    
    // Create logs directory
    struct stat st = {0};
    if (stat(MOUNT_POINT "/logs", &st) == -1) {
        mkdir(MOUNT_POINT "/logs", 0700);
    }

    return ESP_OK;
}

esp_err_t sd_logger_enable(bool enable) {
    logging_enabled = enable;
    
    if (enable) {
        ESP_LOGI(TAG, "SD logging enabled");
        return sd_logger_check_date();
    } else {
        ESP_LOGI(TAG, "SD logging disabled");
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
        return ESP_OK;
    }
}

bool sd_logger_is_enabled(void) {
    return logging_enabled;
}

esp_err_t sd_logger_check_date(void) {
    if (!logging_enabled) return ESP_OK;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char new_date[16];
    strftime(new_date, sizeof(new_date), "%Y%m%d", &timeinfo);

    // Check if we need to open a new file
    if (strcmp(current_date, new_date) != 0) {
        // Close current file if open
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }

        // Update current date
        strcpy(current_date, new_date);

        // Open new file
        char filename[64];
        snprintf(filename, sizeof(filename), MOUNT_POINT "/logs/%s.rtcm", current_date);
        
        log_file = fopen(filename, "a");
        if (!log_file) {
            ESP_LOGE(TAG, "Failed to open log file: %s", filename);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Opened log file: %s", filename);
    }

    return ESP_OK;
}

esp_err_t sd_logger_write(const uint8_t *data, size_t len) {
    if (!logging_enabled || !log_file) {
        return ESP_OK;
    }

    // Check if we need to rotate file (new day)
    sd_logger_check_date();

    if (log_file) {
        size_t written = fwrite(data, 1, len, log_file);
        if (written != len) {
            ESP_LOGE(TAG, "Failed to write all data to SD card");
            return ESP_FAIL;
        }
        fflush(log_file);
        return ESP_OK;
    }

    return ESP_FAIL;
}

void sd_logger_deinit(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_LOGI(TAG, "SD card unmounted");
}