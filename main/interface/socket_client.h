/*
 * SPDX-FileCopyrightText: 2024 ESP32 NTRIP DUO Project
 * SPDX-License-Identifier: GPL-3.0-or-later
 * 
 * TCP/UDP Socket Client header for ESP32 NTRIP DUO
 */

#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Socket client statistics structure
 */
typedef struct {
    time_t start_time;           /*!< Client start time */
    time_t last_connect_time;    /*!< Last successful connection time */
    time_t last_disconnect_time; /*!< Last disconnection time */
    uint32_t connection_count;   /*!< Total number of connections made */
    uint32_t bytes_sent;         /*!< Total bytes sent to server */
    uint32_t bytes_received;     /*!< Total bytes received from server */
} socket_client_stats_t;

/**
 * @brief Initialize socket client
 * 
 * Creates socket client based on configuration settings.
 * Starts background task to handle server connection and data forwarding.
 * 
 * @return
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Client already running
 *         - ESP_ERR_INVALID_ARG: Invalid configuration
 *         - ESP_ERR_NO_MEM: Memory allocation failed
 */
esp_err_t socket_client_init(void);

/**
 * @brief Deinitialize socket client
 * 
 * Stops client task and closes connection.
 * 
 * @return
 *         - ESP_OK: Success
 */
esp_err_t socket_client_deinit(void);

/**
 * @brief Check if client is connected to server
 * 
 * @return true if connected, false otherwise
 */
bool socket_client_is_connected(void);

/**
 * @brief Get client statistics
 * 
 * @param stats Pointer to structure to fill with statistics
 * 
 * @return
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_ARG: Invalid parameter
 */
esp_err_t socket_client_get_stats(socket_client_stats_t *stats);

/**
 * @brief Send UART data to server
 * 
 * @param data Pointer to data buffer
 * @param length Data length in bytes
 * 
 * @return
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Client not connected
 *         - ESP_FAIL: Send failed
 */
esp_err_t socket_client_send_uart_data(const char *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_CLIENT_H */