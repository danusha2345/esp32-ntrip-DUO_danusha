#include "sd_logger.h"

#include "config.h"
#include "uart.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "SD_LOGGER";

#define SD_STREAM_BUFFER_SIZE (16 * 1024)
#define SD_WRITE_BUFFER_SIZE 4096

static FILE *log_file = NULL;
static char current_date[16] = {0};
static volatile bool logging_enabled = false;
static volatile bool logger_task_running = false;
static bool mounted = false;
static bool uart_handler_registered = false;
static sdmmc_card_t *card = NULL;
static spi_host_device_t spi_host = SPI2_HOST;
static SemaphoreHandle_t logger_mutex = NULL;
static StreamBufferHandle_t logger_stream = NULL;
static TaskHandle_t logger_task_handle = NULL;

static esp_err_t sd_logger_check_date_locked(void) {
    if (!logging_enabled || !mounted) return ESP_OK;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char new_date[16];
    if (timeinfo.tm_year + 1900 < 2020) {
        strlcpy(new_date, "unsynced", sizeof(new_date));
    } else {
        strftime(new_date, sizeof(new_date), "%Y%m%d", &timeinfo);
    }

    if (strcmp(current_date, new_date) == 0) return ESP_OK;

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), MOUNT_POINT "/logs/%s.rtcm", new_date);
    log_file = fopen(filename, "ab");
    if (!log_file) {
        ESP_LOGE(TAG, "Failed to open log file: %s", filename);
        current_date[0] = '\0';
        return ESP_FAIL;
    }

    strlcpy(current_date, new_date, sizeof(current_date));
    ESP_LOGI(TAG, "Opened log file: %s", filename);
    return ESP_OK;
}

static void sd_logger_uart_handler(void *handler_args, esp_event_base_t base,
                                   int32_t length, void *data) {
    if (!logging_enabled || !logger_stream || length <= 0) return;

    size_t queued = xStreamBufferSend(logger_stream, data, (size_t) length, 0);
    if (queued != (size_t) length) {
        ESP_LOGW(TAG, "SD buffer full, dropped %u bytes",
                 (unsigned) ((size_t) length - queued));
    }
}

static void sd_logger_task(void *ctx) {
    uint8_t buffer[SD_WRITE_BUFFER_SIZE];

    while (logger_task_running) {
        size_t length = xStreamBufferReceive(logger_stream, buffer, sizeof(buffer),
                                             pdMS_TO_TICKS(250));
        if (length > 0) sd_logger_write(buffer, length);
    }

    logger_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t sd_logger_init(void) {
    if (mounted) return sd_logger_enable(true);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_host = host.slot;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_WRITE_BUFFER_SIZE,
    };

    ESP_LOGI(TAG, "Initializing SD card");
    esp_err_t ret = spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = spi_host;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        spi_bus_free(spi_host);
        card = NULL;
        return ret;
    }
    mounted = true;

    struct stat st;
    if (stat(MOUNT_POINT "/logs", &st) != 0 && mkdir(MOUNT_POINT "/logs", 0700) != 0) {
        ESP_LOGE(TAG, "Failed to create SD log directory");
        sd_logger_deinit();
        return ESP_FAIL;
    }

    logger_mutex = xSemaphoreCreateMutex();
    logger_stream = xStreamBufferCreate(SD_STREAM_BUFFER_SIZE, 1);
    if (!logger_mutex || !logger_stream) {
        ESP_LOGE(TAG, "Failed to allocate SD logger synchronization objects");
        sd_logger_deinit();
        return ESP_ERR_NO_MEM;
    }

    logger_task_running = true;
    if (xTaskCreate(sd_logger_task, "sd_logger", 4096, NULL, 4,
                    &logger_task_handle) != pdPASS) {
        logger_task_running = false;
        ESP_LOGE(TAG, "Failed to create SD logger task");
        sd_logger_deinit();
        return ESP_ERR_NO_MEM;
    }

    uart_register_read_handler(sd_logger_uart_handler);
    uart_handler_registered = true;
    ESP_LOGI(TAG, "SD card mounted successfully");
    return sd_logger_enable(true);
}

esp_err_t sd_logger_enable(bool enable) {
    if (enable && !mounted) return ESP_ERR_INVALID_STATE;

    if (!logger_mutex) {
        logging_enabled = false;
        return enable ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    xSemaphoreTake(logger_mutex, portMAX_DELAY);
    logging_enabled = enable;
    esp_err_t ret = ESP_OK;
    if (enable) {
        ret = sd_logger_check_date_locked();
        if (ret != ESP_OK) logging_enabled = false;
    } else if (log_file) {
        fclose(log_file);
        log_file = NULL;
        current_date[0] = '\0';
    }
    xSemaphoreGive(logger_mutex);

    ESP_LOGI(TAG, "SD logging %s", enable ? "enabled" : "disabled");
    return ret;
}

bool sd_logger_is_enabled(void) {
    return logging_enabled;
}

esp_err_t sd_logger_check_date(void) {
    if (!logger_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(logger_mutex, portMAX_DELAY);
    esp_err_t ret = sd_logger_check_date_locked();
    xSemaphoreGive(logger_mutex);
    return ret;
}

esp_err_t sd_logger_write(const uint8_t *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!logging_enabled || !logger_mutex) return ESP_OK;

    xSemaphoreTake(logger_mutex, portMAX_DELAY);
    esp_err_t ret = sd_logger_check_date_locked();
    if (ret == ESP_OK && log_file) {
        size_t written = fwrite(data, 1, len, log_file);
        if (written != len || fflush(log_file) != 0) {
            ESP_LOGE(TAG, "Failed to write RTCM data to SD card");
            ret = ESP_FAIL;
        }
    }
    xSemaphoreGive(logger_mutex);
    return ret;
}

void sd_logger_deinit(void) {
    logging_enabled = false;

    if (uart_handler_registered) {
        uart_unregister_read_handler(sd_logger_uart_handler);
        uart_handler_registered = false;
    }

    logger_task_running = false;
    for (int i = 0; logger_task_handle && i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (logger_task_handle) {
        ESP_LOGE(TAG, "SD logger task did not stop; keeping resources allocated");
        return;
    }

    if (logger_mutex) xSemaphoreTake(logger_mutex, portMAX_DELAY);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    if (logger_mutex) xSemaphoreGive(logger_mutex);

    if (logger_stream) {
        vStreamBufferDelete(logger_stream);
        logger_stream = NULL;
    }
    if (logger_mutex) {
        vSemaphoreDelete(logger_mutex);
        logger_mutex = NULL;
    }

    if (mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(spi_host);
        mounted = false;
        card = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
    current_date[0] = '\0';
}
