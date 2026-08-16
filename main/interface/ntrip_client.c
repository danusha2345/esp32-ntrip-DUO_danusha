/*
 * NTRIP correction client derived from nebkat/esp32-xbee.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <status_led.h>
#include <stream_stats.h>
#include <sys/socket.h>
#include <tasks.h>
#include <wifi.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "interface/ntrip.h"
#include "retry.h"
#include "uart.h"
#include "util.h"

#define BUFFER_SIZE 1024
#define GGA_INTERVAL_US (15LL * 1000 * 1000)

static const char *TAG = "NTRIP_CLIENT";
static int sock = -1;
static status_led_handle_t status_led = NULL;
static stream_stats_handle_t stream_stats = NULL;

static portMUX_TYPE gga_mux = portMUX_INITIALIZER_UNLOCKED;
static char gga_line[128];
static size_t gga_line_length = 0;
static char latest_gga[128];

static void ntrip_client_uart_handler(void *handler_args, esp_event_base_t base,
                                      int32_t length, void *data) {
    const uint8_t *bytes = data;
    if (length <= 0) return;

    portENTER_CRITICAL(&gga_mux);
    for (int32_t i = 0; i < length; i++) {
        char ch = (char) bytes[i];
        if (gga_line_length < sizeof(gga_line) - 1) {
            gga_line[gga_line_length++] = ch;
        } else {
            gga_line_length = 0;
        }

        if (ch == '\n') {
            gga_line[gga_line_length] = '\0';
            if (strncmp(gga_line, "$GPGGA", 6) == 0 ||
                strncmp(gga_line, "$GNGGA", 6) == 0) {
                memcpy(latest_gga, gga_line, gga_line_length + 1);
            }
            gga_line_length = 0;
        }
    }
    portEXIT_CRITICAL(&gga_mux);
}

static size_t copy_latest_gga(char *destination, size_t destination_size) {
    portENTER_CRITICAL(&gga_mux);
    size_t length = strnlen(latest_gga, sizeof(latest_gga));
    if (length >= destination_size) length = destination_size - 1;
    memcpy(destination, latest_gga, length);
    destination[length] = '\0';
    portEXIT_CRITICAL(&gga_mux);
    return length;
}

static void ntrip_client_task(void *ctx) {
    retry_delay_handle_t retry = retry_init(true, 5, 2000, 0);
    char *buffer = malloc(BUFFER_SIZE);
    if (!retry || !buffer) {
        ESP_LOGE(TAG, "Failed to allocate NTRIP client resources");
        free(buffer);
        vTaskDelete(NULL);
        return;
    }

    uart_register_read_handler(ntrip_client_uart_handler);

    config_color_t color = config_get_color(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_COLOR));
    if (color.rgba != 0) status_led = status_led_add(color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led) status_led->active = false;
    stream_stats = stream_stats_new("ntrip_client");

    while (true) {
        char *host = NULL;
        char *mountpoint = NULL;
        char *username = NULL;
        char *password = NULL;

        retry_delay(retry);
        wait_for_ip();

        uint16_t port = config_get_u16(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_PORT));
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_HOST), (void **) &host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_MOUNTPOINT), (void **) &mountpoint);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_PASSWORD), (void **) &password);

        if (!host || !mountpoint || !username || !password || !*host || !*mountpoint) {
            ESP_LOGE(TAG, "Host and mountpoint must be configured");
            goto cleanup;
        }
        const char *mountpoint_path = mountpoint;
        while (*mountpoint_path == '/') mountpoint_path++;

        ESP_LOGI(TAG, "Connecting to %s:%u/%s", host, port, mountpoint_path);
        sock = connect_socket(host, port, SOCK_STREAM);
        ERROR_ACTION(TAG, sock < 0, goto cleanup, "Could not connect to caster");

        char *authorization = NULL;
        if (*username || *password) authorization = http_auth_basic_header(username, password);
        int request_length;
        if (authorization) {
            request_length = snprintf(buffer, BUFFER_SIZE,
                    "GET /%s HTTP/1.1" NEWLINE
                    "Host: %s:%u" NEWLINE
                    "User-Agent: NTRIP %s/%s" NEWLINE
                    "Ntrip-Version: Ntrip/2.0" NEWLINE
                    "Authorization: %s" NEWLINE NEWLINE,
                    mountpoint_path, host, port, NTRIP_CLIENT_NAME,
                    esp_app_get_description()->version, authorization);
        } else {
            request_length = snprintf(buffer, BUFFER_SIZE,
                    "GET /%s HTTP/1.1" NEWLINE
                    "Host: %s:%u" NEWLINE
                    "User-Agent: NTRIP %s/%s" NEWLINE
                    "Ntrip-Version: Ntrip/2.0" NEWLINE NEWLINE,
                    mountpoint_path, host, port, NTRIP_CLIENT_NAME,
                    esp_app_get_description()->version);
        }
        free(authorization);
        ERROR_ACTION(TAG, request_length <= 0 || (size_t) request_length >= BUFFER_SIZE,
                     goto cleanup, "NTRIP request is too large");
        ERROR_ACTION(TAG, write_all(sock, buffer, (size_t) request_length) != ESP_OK,
                     goto cleanup, "Could not send NTRIP request");

        int len = read(sock, buffer, BUFFER_SIZE - 1);
        ERROR_ACTION(TAG, len <= 0, goto cleanup, "Could not receive caster response");
        buffer[len] = '\0';

        char *status = extract_http_header(buffer, "");
        bool response_ok = status && ntrip_response_ok(status);
        if (!response_ok) {
            ESP_LOGE(TAG, "Caster rejected connection: %s", status ? status : "malformed response");
            free(status);
            goto cleanup;
        }
        free(status);

        retry_reset(retry);
        if (status_led) status_led->active = true;
        ESP_LOGI(TAG, "Connected to %s:%u/%s", host, port, mountpoint_path);

        // A caster may append the first RTCM bytes to the HTTP response packet.
        char *payload = memmem(buffer, (size_t) len, NEWLINE NEWLINE,
                               2 * NEWLINE_LENGTH);
        if (payload) {
            payload += 2 * NEWLINE_LENGTH;
        } else if (strncmp(buffer, "ICY ", 4) == 0 || strncmp(buffer, "OK", 2) == 0) {
            payload = memmem(buffer, (size_t) len, NEWLINE, NEWLINE_LENGTH);
            if (payload) payload += NEWLINE_LENGTH;
        }
        if (payload) {
            size_t payload_length = (size_t) (buffer + len - payload);
            if (payload_length > 0) {
                uart_write(payload, payload_length);
                stream_stats_increment(stream_stats, (int32_t) payload_length, 0);
            }
        }

        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int64_t last_gga_send = 0;

        while (sock >= 0) {
            len = read(sock, buffer, BUFFER_SIZE);
            if (len > 0) {
                uart_write(buffer, (size_t) len);
                stream_stats_increment(stream_stats, len, 0);
            } else if (len == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }

            int64_t now = esp_timer_get_time();
            if (now - last_gga_send >= GGA_INTERVAL_US) {
                char gga[sizeof(latest_gga)];
                size_t gga_length = copy_latest_gga(gga, sizeof(gga));
                if (gga_length > 0) {
                    if (write_all(sock, gga, gga_length) != ESP_OK) break;
                    stream_stats_increment(stream_stats, 0, gga_length);
                }
                last_gga_send = now;
            }
        }

        ESP_LOGW(TAG, "Disconnected from %s:%u/%s", host, port, mountpoint_path);

cleanup:
        if (status_led) status_led->active = false;
        destroy_socket(&sock);
        free(host);
        free(mountpoint);
        free(username);
        free(password);
    }
}

void ntrip_client_init(void) {
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_ACTIVE))) return;
    if (xTaskCreate(ntrip_client_task, "ntrip_client", 6144, NULL,
                    TASK_PRIORITY_INTERFACE, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NTRIP client task");
    }
}
