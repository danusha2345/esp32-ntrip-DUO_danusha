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
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "socket_client.h"
#include "config.h"
#include "uart.h"
#include "status_led.h"
#include "wifi.h"

static const char *TAG = "socket_client";

#define SOCKET_BUFFER_SIZE 1024
#define SOCKET_CLIENT_STACK_SIZE 4096
#define RECONNECT_DELAY_MS 5000
#define MAX_RECONNECT_DELAY_MS 60000

static bool client_running = false;
static TaskHandle_t client_task_handle = NULL;
static int client_socket = -1;
static socket_client_stats_t client_stats = {0};
static bool connected = false;

// Forward declarations
static void socket_client_task(void *params);
static esp_err_t socket_client_connect(void);
static void socket_client_disconnect(void);
static esp_err_t socket_client_send_data(const char *data, size_t length);

static esp_err_t socket_client_connect(void) {
    struct sockaddr_in dest_addr = {0};
    struct hostent *host_entry;
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
        ESP_LOGI(TAG, "Attempting to connect to %s:%d", 
                 get_socket_client_host(), get_socket_client_port());

        // Resolve hostname
        host_entry = gethostbyname(get_socket_client_host());
        if (host_entry == NULL) {
            ESP_LOGE(TAG, "Failed to resolve hostname: %s", get_socket_client_host());
            vTaskDelay(reconnect_delay / portTICK_PERIOD_MS);
            
            // Exponential backoff
            reconnect_delay = (reconnect_delay * 2 > MAX_RECONNECT_DELAY_MS) ? 
                             MAX_RECONNECT_DELAY_MS : reconnect_delay * 2;
            continue;
        }

        // Create socket
        int sock_type = is_socket_client_tcp() ? SOCK_STREAM : SOCK_DGRAM;
        client_socket = socket(AF_INET, sock_type, 0);
        if (client_socket < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(reconnect_delay / portTICK_PERIOD_MS);
            continue;
        }

        // Set socket timeout
        struct timeval timeout = {
            .tv_sec = 10,
            .tv_usec = 0,
        };
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // Setup destination address
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(get_socket_client_port());
        memcpy(&dest_addr.sin_addr, host_entry->h_addr, host_entry->h_length);

        // Connect to server
        int err = connect(client_socket, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            close(client_socket);
            client_socket = -1;
            
            vTaskDelay(reconnect_delay / portTICK_PERIOD_MS);
            reconnect_delay = (reconnect_delay * 2 > MAX_RECONNECT_DELAY_MS) ? 
                             MAX_RECONNECT_DELAY_MS : reconnect_delay * 2;
            continue;
        }

        connected = true;
        client_stats.connection_count++;
        client_stats.last_connect_time = time(NULL);
        reconnect_delay = RECONNECT_DELAY_MS;  // Reset delay on successful connection
        
        ESP_LOGI(TAG, "Successfully connected to %s:%d", 
                 get_socket_client_host(), get_socket_client_port());

        // Send connection message if configured
        const char *connect_msg = get_socket_client_connect_message();
        if (connect_msg && strlen(connect_msg) > 0) {
            socket_client_send_data(connect_msg, strlen(connect_msg));
            socket_client_send_data("\r\n", 2);
        }

        // Update status LED - green for connected
        status_led_add(0x00FF0000, STATUS_LED_STATIC, 0, 0, 0);
        
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
    
    // Update status LED - red for disconnected  
    status_led_add(0xFF000000, STATUS_LED_STATIC, 0, 0, 0);
}

static esp_err_t socket_client_send_data(const char *data, size_t length) {
    if (!connected || client_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = send(client_socket, data, length, 0);
    if (sent < 0) {
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - check for UART data to send
                // Используем номер UART из конфигурации вместо хардкода
                uart_port_t uart_port = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_NUM));
                size_t uart_data_len = uart_read_bytes(uart_port, (uint8_t*)buffer, 
                                                       sizeof(buffer) - 1, 10 / portTICK_PERIOD_MS);
                if (uart_data_len > 0) {
                    socket_client_send_data(buffer, uart_data_len);
                }
                continue;
            } else {
                ESP_LOGE(TAG, "Receive failed: errno %d", errno);
                socket_client_disconnect();
                continue;
            }
        } else if (len == 0) {
            ESP_LOGI(TAG, "Server disconnected");
            socket_client_disconnect();
            continue;
        }

        // Forward received data to UART
        buffer[len] = 0;
        client_stats.bytes_received += len;
        uart_write_bytes(UART_NUM_0, buffer, len);
        ESP_LOGD(TAG, "Received %d bytes from server, forwarded to UART", len);

        // Check for UART data to send to server
        size_t uart_data_len = uart_read_bytes(UART_NUM_0, (uint8_t*)buffer, 
                                               sizeof(buffer) - 1, 10 / portTICK_PERIOD_MS);
        if (uart_data_len > 0) {
            socket_client_send_data(buffer, uart_data_len);
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

    // Start client task
    client_running = true;
    BaseType_t ret = xTaskCreate(socket_client_task, "socket_client", 
                                SOCKET_CLIENT_STACK_SIZE, NULL, 5, &client_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create socket client task");
        client_running = false;
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

    // Wait for task to finish
    if (client_task_handle) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

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
    return socket_client_send_data(data, length);
}