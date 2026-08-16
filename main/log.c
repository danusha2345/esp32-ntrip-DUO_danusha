/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <string.h>
#include <uart.h>
#include "log.h"

#define INITIAL_MAGIC "@@@@\n"

static const char *TAG = "LOG";

static RingbufHandle_t ringbuf_handle;

esp_err_t log_init() {
    ringbuf_handle = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    if (ringbuf_handle == NULL) {
        ESP_LOGE(TAG, "Could not create log ring buffer");
        return ESP_FAIL;
    }

    // Magic string to let web log know that ESP32 has restart (to reset line counter)
    xRingbufferSend(ringbuf_handle, INITIAL_MAGIC, strlen(INITIAL_MAGIC), 0);

    return ESP_OK;
}

int log_vprintf(const char * format, va_list arg) {
    char buffer[512];
    int formatted = vsnprintf(buffer, sizeof(buffer), format, arg);
    if (formatted < 0) return formatted;

    size_t length = strnlen(buffer, sizeof(buffer) - 1);
    const char *start = buffer;
    const size_t color_prefix_len = strlen(LOG_COLOR_E);
    const size_t color_suffix_len = strlen(LOG_RESET_COLOR);

    // ESP-IDF logs normally contain ANSI color wrappers, but custom log calls may not.
    if (length >= color_prefix_len && memcmp(start, LOG_COLOR_E, color_prefix_len) == 0) {
        start += color_prefix_len;
        length -= color_prefix_len;
    }
    if (length >= color_suffix_len &&
        memcmp(start + length - color_suffix_len, LOG_RESET_COLOR, color_suffix_len) == 0) {
        length -= color_suffix_len;
    }
    if (length > 0 && start[length - 1] == '\n') length--;

    xRingbufferSend(ringbuf_handle, start, length, 0);
    xRingbufferSend(ringbuf_handle, "\n", 1, 0);

    uart_log(buffer, strnlen(buffer, sizeof(buffer)));

    return formatted;
}

void *log_receive(size_t *length, TickType_t ticksToWait) {
    return xRingbufferReceive(ringbuf_handle, length, ticksToWait);
}

void log_return(void *item) {
    vRingbufferReturnItem(ringbuf_handle, item);
}
