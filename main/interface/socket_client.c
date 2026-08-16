/*
 * SPDX-FileCopyrightText: 2024 ESP32 NTRIP DUO Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 * 
 * TCP/UDP Socket Client implementation for ESP32 NTRIP DUO
 * Based on ESP32-XBee project by MichaelEFlip
 */

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <driver/uart.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "socket_client.h"
#include "config.h"
#include "uart.h"
#include "status_led.h"
#include "wifi.h"
#include "util.h"

static const char *TAG = "socket_client";

#define SOCKET_BUFFER_SIZE 1024
#define SOCKET_CLIENT_STACK_SIZE 4096
#define RECONNECT_DELAY_MS 5000
#define MAX_RECONNECT_DELAY_MS 60000

static volatile bool client_running = false;
static TaskHandle_t client_task_handle = NULL;
static int client_socket = -1;
static socket_client_stats_t client_stats = {0};
static bool connected = false;
static StreamBufferHandle_t uart_stream = NULL;
static status_led_handle_t status_led = NULL;

// Forward declarations
static void socket_client_task(void *params);
static esp_err_t socket_client_connect(void);
static void socket_client_disconnect(void);
static esp_err_t socket_client_send_data(const char *data, size_t length);

static void socket_client_uart_handler(void *handler_args, esp_event_base_t base,
                                       int32_t length, void *data) {
    if (!client_running || !uart_stream || length <= 0) return;

    size_t queued = xStreamBufferSend(uart_stream, data, (size_t) length, 0);
    if (queued != (size_t) length) {
        ESP_LOGW(TAG, "UART network buffer full, dropped %u bytes",
                 (unsigned) ((size_t) length - queued));
    }
}

static esp_err_t socket_client_connect(void) {
    int reconnect_delay = RECONNECT_DELAY_MS;

    // Wait for WiFi connection
    wifi_sta_status_t wifi_status;
    wifi_sta_status(&wifi_status);
    while (!wifi_status.connected) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        wifi_sta_status(&wifi_status);
        
        if (!client_running) {
            return ESP_FAIL;
        }
    }

    while (client_running && !connected) {
        const char *host = get_socket_client_host();
        int port = get_socket_client_port();
        ESP_LOGI(TAG, "Attempting to connect to %s:%d", 
                 host, port);

        int sock_type = is_socket_client_tcp() ? SOCK_STREAM : SOCK_DGRAM;
        client_socket = connect_socket(host, port, sock_type);
        if (client_socket < 0) {
            ESP_LOGE(TAG, "Unable to connect socket: %d", client_socket);
            vTaskDelay(reconnect_delay / portTICK_PERIOD_MS);
            reconnect_delay = (reconnect_delay * 2 > MAX_RECONNECT_DELAY_MS) ?
                              MAX_RECONNECT_DELAY_MS : reconnect_delay * 2;
            continue;
        }

        // Set socket timeout
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        connected = true;
        client_stats.connection_count++;
        client_stats.last_connect_time = time(NULL);
        reconnect_delay = RECONNECT_DELAY_MS;  // Reset delay on successful connection
        
        ESP_LOGI(TAG, "Successfully connected to %s:%d", 
                 host, port);

        // Send connection message if configured
        const char *connect_msg = get_socket_client_connect_message();
        if (connect_msg && strlen(connect_msg) > 0) {
            socket_client_send_data(connect_msg, strlen(connect_msg));
            socket_client_send_data("\r\n", 2);
        }

        if (status_led) status_led->active = true;
        
        return ESP_OK;
    }

    return ESP_FAIL;
}

static void socket_client_disconnect(void) {
    if (client_socket >= 0) {
        ESP_LOGI(TAG, "Disconnecting from server");
        close(client_socket);
        client_socket = -1;
    }
    
    connected = false;
    client_stats.last_disconnect_time = time(NULL);
    
    if (status_led) status_led->active = false;
}

static esp_err_t socket_client_send_data(const char *data, size_t length) {
    if (!connected || client_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent;
    if (is_socket_client_tcp()) {
        sent = write_all(client_socket, data, length) == ESP_OK ? (int) length : -1;
    } else {
        sent = send(client_socket, data, length, 0);
    }
    if (sent < 0 || (size_t) sent != length) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        socket_client_disconnect();
        return ESP_FAIL;
    }

    client_stats.bytes_sent += sent;
    ESP_LOGD(TAG, "Sent %d bytes to server", sent);
    return ESP_OK;
}

static void socket_client_task(void *params) {
    char buffer[SOCKET_BUFFER_SIZE];
    
    ESP_LOGI(TAG, "Socket client task started");

    while (client_running) {
        // Connect to server
        if (!connected) {
            if (socket_client_connect() != ESP_OK) {
                continue;
            }
        }

        // Read data from server
        int len = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "Receive failed: errno %d", errno);
                socket_client_disconnect();
                continue;
            }
        } else if (len == 0 && is_socket_client_tcp()) {
            ESP_LOGI(TAG, "Server disconnected");
            socket_client_disconnect();
            continue;
        } else if (len > 0) {
            client_stats.bytes_received += len;
            uart_write(buffer, len);
            ESP_LOGD(TAG, "Received %d bytes from server, forwarded to UART", len);
        }

        size_t uart_data_len;
        while ((uart_data_len = xStreamBufferReceive(uart_stream, buffer,
                                                     sizeof(buffer), 0)) > 0) {
            if (socket_client_send_data(buffer, uart_data_len) != ESP_OK) break;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // Cleanup
    socket_client_disconnect();
    
    ESP_LOGI(TAG, "Socket client task finished");
    client_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t socket_client_init(void) {
    if (client_running) {
        ESP_LOGW(TAG, "Socket client already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_socket_client_enabled()) {
        ESP_LOGI(TAG, "Socket client disabled in configuration");
        return ESP_OK;
    }

    // Validate configuration
    const char *host = get_socket_client_host();
    if (!host || strlen(host) == 0) {
        ESP_LOGE(TAG, "Socket client host not configured");
        return ESP_ERR_INVALID_ARG;
    }

    int port = get_socket_client_port();
    if (port <= 0 || port > 65535) {
        ESP_LOGE(TAG, "Socket client port invalid: %d", port);
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize statistics
    memset(&client_stats, 0, sizeof(client_stats));
    client_stats.start_time = time(NULL);

    uart_stream = xStreamBufferCreate(4096, 1);
    if (!uart_stream) {
        ESP_LOGE(TAG, "Failed to create UART stream buffer");
        return ESP_ERR_NO_MEM;
    }

    status_led = status_led_add(0x00FF0055, STATUS_LED_STATIC, 0, 0, 0);
    if (status_led) status_led->active = false;

    // Start client task
    client_running = true;
    uart_register_read_handler(socket_client_uart_handler);
    BaseType_t ret = xTaskCreate(socket_client_task, "socket_client", 
                                SOCKET_CLIENT_STACK_SIZE, NULL, 5, &client_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create socket client task");
        client_running = false;
        uart_unregister_read_handler(socket_client_uart_handler);
        vStreamBufferDelete(uart_stream);
        uart_stream = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Socket client initialized successfully");
    return ESP_OK;
}

esp_err_t socket_client_deinit(void) {
    if (!client_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping socket client");
    client_running = false;
    if (client_socket >= 0) shutdown(client_socket, SHUT_RDWR);

    for (int i = 0; client_task_handle && i < 200; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (client_task_handle) {
        ESP_LOGE(TAG, "Socket client task did not stop in time");
        return ESP_ERR_TIMEOUT;
    }

    uart_unregister_read_handler(socket_client_uart_handler);
    vStreamBufferDelete(uart_stream);
    uart_stream = NULL;

    return ESP_OK;
}

bool socket_client_is_connected(void) {
    return connected;
}

esp_err_t socket_client_get_stats(socket_client_stats_t *stats) {
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    *stats = client_stats;
    return ESP_OK;
}

esp_err_t socket_client_send_uart_data(const char *data, size_t length) {
    if (!uart_stream || !data || length == 0) return ESP_ERR_INVALID_ARG;
    return xStreamBufferSend(uart_stream, data, length, 0) == length
           ? ESP_OK : ESP_ERR_TIMEOUT;
}
