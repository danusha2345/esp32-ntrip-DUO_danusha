/*
 * SPDX-FileCopyrightText: 2024 ESP32 NTRIP DUO Project  
 * SPDX-License-Identifier: GPL-3.0-or-later
 * 
 * TCP/UDP Socket Server header for ESP32 NTRIP DUO
 */

#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#include "esp_err.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Socket client information structure
 */
typedef struct {
    bool connected;              /*!< Client connection status */
    char address[128];           /*!< Client IP address string */
    uint16_t port;              /*!< Client port number */
    uint32_t bytes_sent;        /*!< Total bytes sent to client */
    uint32_t bytes_received;    /*!< Total bytes received from client */
    time_t connect_time;        /*!< Connection timestamp */
} socket_client_info_t;

/**
 * @brief Initialize socket server
 * 
 * Creates TCP and/or UDP servers based on configuration settings.
 * Starts background task to handle client connections and data forwarding.
 * 
 * @return
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_STATE: Server already running  
 *         - ESP_ERR_NO_MEM: Memory allocation failed
 *         - ESP_FAIL: Socket initialization failed
 */
esp_err_t socket_server_init(void);

/**
 * @brief Deinitialize socket server
 * 
 * Stops server task and closes all client connections.
 * 
 * @return
 *         - ESP_OK: Success
 */
esp_err_t socket_server_deinit(void);

/**
 * @brief Get number of connected clients
 * 
 * @return Number of active client connections
 */
int socket_server_get_client_count(void);

/**
 * @brief Get information about specific client
 * 
 * @param index Client index (0 to MAX_CLIENTS-1)
 * @param info Pointer to structure to fill with client info
 * 
 * @return
 *         - ESP_OK: Success
 *         - ESP_ERR_INVALID_ARG: Invalid parameters
 *         - ESP_ERR_NOT_FOUND: Client not connected
 */
esp_err_t socket_server_get_client_info(int index, socket_client_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* SOCKET_SERVER_H */