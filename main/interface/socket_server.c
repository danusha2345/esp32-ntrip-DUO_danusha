/*
 * SPDX-FileCopyrightText: 2024 ESP32 NTRIP DUO Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 * 
 * TCP/UDP Socket Server implementation for ESP32 NTRIP DUO
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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "socket_server.h"
#include "config.h"
#include "uart.h"
#include "status_led.h"

static const char *TAG = "socket_server";

#define MAX_CLIENTS 10
#define SOCKET_BUFFER_SIZE 1024
#define SOCKET_SERVER_STACK_SIZE 4096

static bool server_running = false;
static TaskHandle_t server_task_handle = NULL;
static int tcp_server_socket = -1;
static int udp_server_socket = -1;

typedef struct {
    int socket;
    struct sockaddr_in6 addr;
    bool connected;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    time_t connect_time;
} socket_client_t;

static socket_client_t clients[MAX_CLIENTS];
static SemaphoreHandle_t clients_mutex;

// Forward declarations
static int socket_init(int type, int port);
static int socket_tcp_init(void);
static int socket_udp_init(void);
static void socket_server_task(void *params);
static int socket_tcp_accept(int server_socket);
static int socket_udp_accept(int server_socket);
static void socket_client_close(int client_index);
static void socket_send_to_all_clients(const char *data, size_t length);

static int socket_init(int type, int port) {
    int sock = socket(AF_INET6, type, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return -1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Enable dual stack (IPv4 and IPv6)
    int ipv6only = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only));

    struct sockaddr_in6 dest_addr = {};
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_addr = in6addr_any;
    dest_addr.sin6_port = htons(port);

    int err = bind(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d, port %d", errno, port);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Socket bound to port %d", port);
    return sock;
}

static int socket_tcp_init(void) {
    int sock = socket_init(SOCK_STREAM, get_tcp_server_port());
    if (sock < 0) {
        return -1;
    }

    int err = listen(sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", get_tcp_server_port());
    return sock;
}

static int socket_udp_init(void) {
    int sock = socket_init(SOCK_DGRAM, get_udp_server_port());
    if (sock < 0) {
        return -1;
    }

    ESP_LOGI(TAG, "UDP server listening on port %d", get_udp_server_port());
    return sock;
}

static int socket_tcp_accept(int server_socket) {
    struct sockaddr_in6 source_addr;
    socklen_t addr_len = sizeof(source_addr);
    
    int client_socket = accept(server_socket, (struct sockaddr*)&source_addr, &addr_len);
    if (client_socket < 0) {
        ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
        return -1;
    }

    // Find empty slot for client
    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].connected) {
            clients[i].socket = client_socket;
            clients[i].addr = source_addr;
            clients[i].connected = true;
            clients[i].bytes_sent = 0;
            clients[i].bytes_received = 0;
            clients[i].connect_time = time(NULL);
            
            char addr_str[128];
            inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "TCP client connected from %s, slot %d", addr_str, i);
            
            xSemaphoreGive(clients_mutex);
            return i;
        }
    }
    xSemaphoreGive(clients_mutex);

    ESP_LOGW(TAG, "No free slots for new TCP client, closing connection");
    close(client_socket);
    return -1;
}

static int socket_udp_accept(int server_socket) {
    struct sockaddr_in6 source_addr;
    socklen_t addr_len = sizeof(source_addr);
    char buffer[1];
    
    int len = recvfrom(server_socket, buffer, sizeof(buffer), MSG_PEEK, 
                       (struct sockaddr*)&source_addr, &addr_len);
    if (len < 0) {
        return -1;
    }

    // Check if client already exists
    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected && 
            memcmp(&clients[i].addr, &source_addr, sizeof(source_addr)) == 0) {
            xSemaphoreGive(clients_mutex);
            return i;
        }
    }

    // Find empty slot for new UDP client
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].connected) {
            clients[i].socket = server_socket;  // UDP uses server socket
            clients[i].addr = source_addr;
            clients[i].connected = true;
            clients[i].bytes_sent = 0;
            clients[i].bytes_received = 0;
            clients[i].connect_time = time(NULL);
            
            char addr_str[128];
            inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "UDP client connected from %s, slot %d", addr_str, i);
            
            xSemaphoreGive(clients_mutex);
            return i;
        }
    }
    xSemaphoreGive(clients_mutex);

    ESP_LOGW(TAG, "No free slots for new UDP client");
    return -1;
}

static void socket_client_close(int client_index) {
    if (client_index < 0 || client_index >= MAX_CLIENTS) {
        return;
    }

    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    if (clients[client_index].connected) {
        ESP_LOGI(TAG, "Closing client %d", client_index);
        
        if (clients[client_index].socket != tcp_server_socket && 
            clients[client_index].socket != udp_server_socket) {
            close(clients[client_index].socket);
        }
        
        memset(&clients[client_index], 0, sizeof(socket_client_t));
    }
    xSemaphoreGive(clients_mutex);
}

static void socket_send_to_all_clients(const char *data, size_t length) {
    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            int sent = 0;
            
            if (clients[i].socket == udp_server_socket) {
                // UDP client
                sent = sendto(clients[i].socket, data, length, 0,
                            (struct sockaddr*)&clients[i].addr, sizeof(clients[i].addr));
            } else {
                // TCP client
                sent = send(clients[i].socket, data, length, 0);
            }
            
            if (sent < 0) {
                ESP_LOGE(TAG, "Send failed to client %d: errno %d", i, errno);
                clients[i].connected = false;  // Mark for cleanup
            } else {
                clients[i].bytes_sent += sent;
            }
        }
    }
    
    xSemaphoreGive(clients_mutex);
}

static void socket_server_task(void *params) {
    char buffer[SOCKET_BUFFER_SIZE];
    fd_set read_fds;
    int max_fd = 0;

    ESP_LOGI(TAG, "Socket server task started");

    while (server_running) {
        FD_ZERO(&read_fds);
        max_fd = 0;

        // Add server sockets to select
        if (tcp_server_socket >= 0) {
            FD_SET(tcp_server_socket, &read_fds);
            max_fd = tcp_server_socket;
        }
        if (udp_server_socket >= 0) {
            FD_SET(udp_server_socket, &read_fds);
            if (udp_server_socket > max_fd) {
                max_fd = udp_server_socket;
            }
        }

        // Add client sockets to select
        xSemaphoreTake(clients_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].connected && clients[i].socket != udp_server_socket) {
                FD_SET(clients[i].socket, &read_fds);
                if (clients[i].socket > max_fd) {
                    max_fd = clients[i].socket;
                }
            }
        }
        xSemaphoreGive(clients_mutex);

        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0) {
            ESP_LOGE(TAG, "Select error: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (activity == 0) {
            // Timeout - check for UART data to send
            size_t uart_data_len = uart_read_bytes(UART_NUM_0, (uint8_t*)buffer, 
                                                   sizeof(buffer) - 1, 10 / portTICK_PERIOD_MS);
            if (uart_data_len > 0) {
                socket_send_to_all_clients(buffer, uart_data_len);
            }
            continue;
        }

        // Check for new TCP connections
        if (tcp_server_socket >= 0 && FD_ISSET(tcp_server_socket, &read_fds)) {
            socket_tcp_accept(tcp_server_socket);
        }

        // Check for UDP data
        if (udp_server_socket >= 0 && FD_ISSET(udp_server_socket, &read_fds)) {
            struct sockaddr_in6 source_addr;
            socklen_t addr_len = sizeof(source_addr);
            
            int len = recvfrom(udp_server_socket, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr*)&source_addr, &addr_len);
            if (len > 0) {
                buffer[len] = 0;
                
                // Find or create UDP client
                int client_idx = socket_udp_accept(udp_server_socket);
                if (client_idx >= 0) {
                    clients[client_idx].bytes_received += len;
                }
                
                // Forward to UART
                uart_write_bytes(UART_NUM_0, buffer, len);
                ESP_LOGD(TAG, "UDP data forwarded to UART: %d bytes", len);
            }
        }

        // Check client sockets for data
        xSemaphoreTake(clients_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].connected && 
                clients[i].socket != udp_server_socket &&
                FD_ISSET(clients[i].socket, &read_fds)) {
                
                int len = recv(clients[i].socket, buffer, sizeof(buffer) - 1, 0);
                if (len > 0) {
                    buffer[len] = 0;
                    clients[i].bytes_received += len;
                    
                    // Forward to UART
                    uart_write_bytes(UART_NUM_0, buffer, len);
                    ESP_LOGD(TAG, "TCP client %d data forwarded to UART: %d bytes", i, len);
                } else if (len == 0) {
                    ESP_LOGI(TAG, "TCP client %d disconnected", i);
                    clients[i].connected = false;
                } else {
                    ESP_LOGE(TAG, "TCP client %d recv error: errno %d", i, errno);
                    clients[i].connected = false;
                }
            }
        }
        xSemaphoreGive(clients_mutex);

        // Clean up disconnected clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].connected && clients[i].socket > 0) {
                socket_client_close(i);
            }
        }

        // Check for UART data to send to clients
        size_t uart_data_len = uart_read_bytes(UART_NUM_0, (uint8_t*)buffer, 
                                               sizeof(buffer) - 1, 10 / portTICK_PERIOD_MS);
        if (uart_data_len > 0) {
            socket_send_to_all_clients(buffer, uart_data_len);
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // Cleanup
    for (int i = 0; i < MAX_CLIENTS; i++) {
        socket_client_close(i);
    }

    if (tcp_server_socket >= 0) {
        close(tcp_server_socket);
        tcp_server_socket = -1;
    }
    if (udp_server_socket >= 0) {
        close(udp_server_socket);
        udp_server_socket = -1;
    }

    ESP_LOGI(TAG, "Socket server task finished");
    server_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t socket_server_init(void) {
    if (server_running) {
        ESP_LOGW(TAG, "Socket server already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_socket_server_enabled()) {
        ESP_LOGI(TAG, "Socket server disabled in configuration");
        return ESP_OK;
    }

    // Initialize mutex
    clients_mutex = xSemaphoreCreateMutex();
    if (clients_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create clients mutex");
        return ESP_ERR_NO_MEM;
    }

    // Initialize client array
    memset(clients, 0, sizeof(clients));

    // Initialize TCP server if enabled
    if (is_tcp_server_enabled()) {
        tcp_server_socket = socket_tcp_init();
        if (tcp_server_socket < 0) {
            ESP_LOGE(TAG, "Failed to initialize TCP server");
            vSemaphoreDelete(clients_mutex);
            return ESP_FAIL;
        }
    }

    // Initialize UDP server if enabled
    if (is_udp_server_enabled()) {
        udp_server_socket = socket_udp_init();
        if (udp_server_socket < 0) {
            ESP_LOGE(TAG, "Failed to initialize UDP server");
            if (tcp_server_socket >= 0) {
                close(tcp_server_socket);
                tcp_server_socket = -1;
            }
            vSemaphoreDelete(clients_mutex);
            return ESP_FAIL;
        }
    }

    // Start server task
    server_running = true;
    BaseType_t ret = xTaskCreate(socket_server_task, "socket_server", 
                                SOCKET_SERVER_STACK_SIZE, NULL, 5, &server_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create socket server task");
        socket_server_deinit();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Socket server initialized successfully");
    return ESP_OK;
}

esp_err_t socket_server_deinit(void) {
    if (!server_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping socket server");
    server_running = false;

    // Wait for task to finish
    if (server_task_handle) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if (clients_mutex) {
        vSemaphoreDelete(clients_mutex);
        clients_mutex = NULL;
    }

    return ESP_OK;
}

int socket_server_get_client_count(void) {
    if (!clients_mutex) {
        return 0;
    }

    int count = 0;
    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            count++;
        }
    }
    xSemaphoreGive(clients_mutex);
    
    return count;
}

esp_err_t socket_server_get_client_info(int index, socket_client_info_t *info) {
    if (!info || index < 0 || index >= MAX_CLIENTS || !clients_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(clients_mutex, portMAX_DELAY);
    if (clients[index].connected) {
        info->connected = true;
        info->bytes_sent = clients[index].bytes_sent;
        info->bytes_received = clients[index].bytes_received;
        info->connect_time = clients[index].connect_time;
        
        // Convert address to string
        inet6_ntoa_r(clients[index].addr.sin6_addr, info->address, 
                     sizeof(info->address) - 1);
        info->port = ntohs(clients[index].addr.sin6_port);
        
        xSemaphoreGive(clients_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(clients_mutex);
    
    return ESP_ERR_NOT_FOUND;
}