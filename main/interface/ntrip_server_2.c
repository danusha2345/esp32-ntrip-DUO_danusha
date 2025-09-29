/*
 * ESP32 NTRIP Duo - Вторичный NTRIP сервер
 * Основан на ESP32-XBee (https://github.com/nebkat/esp32-xbee)
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * Второй клиент NTRIP протокола для одновременной передачи RTK коррекций
 * на два независимых NTRIP кастера (например, Onocoy и RTK Direct).
 * 
 * Особенности вторичного сервера:
 * - Независимое подключение к другому кастеру
 * - Отдельная конфигурация (хост, порт, mountpoint, пароль)
 * - Отдельный статусный светодиод и статистика
 * - Общие данные от UART (оба сервера получают одинаковые данные)
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

#include <stdbool.h>
#include <esp_log.h>
#include <esp_event_base.h>
#include <sys/socket.h>
#include <wifi.h>
#include <tasks.h>
#include <status_led.h>
#include <retry.h>
#include <stream_stats.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_ota_ops.h>
#include "interface/ntrip.h"
#include "config.h"
#include "util.h"
#include "uart.h"

static const char *TAG = "NTRIP_SERVER_2";       // Тег для логирования вторичного NTRIP сервера

#define BUFFER_SIZE 512                             // Размер буфера для сетевых операций (байт)

// Биты состояния для синхронизации между задачами
static const int CASTER_READY_BIT = BIT0;           // Кастер готов принимать данные
static const int DATA_READY_BIT = BIT1;             // Данные доступны от UART
static const int DATA_SENT_BIT = BIT2;              // Данные были отправлены хотя бы раз

static int sock = -1;                               // Сокет соединения с вторым NTRIP кастером
static SemaphoreHandle_t sock_mutex = NULL;         // Мьютекс для защиты сокета от race conditions

static int data_keep_alive;                         // Счётчик времени без данных (мс)
static EventGroupHandle_t server_event_group;       // Группа событий для синхронизации

static status_led_handle_t status_led = NULL;       // Дескриптор статусного светодиода второго сервера
static stream_stats_handle_t stream_stats = NULL;   // Дескриптор статистики потока второго сервера

static TaskHandle_t server_task = NULL;             // Дескриптор основной задачи второго сервера
static TaskHandle_t sleep_task = NULL;              // Дескриптор задачи контроля keep-alive второго сервера

/// Обработчик данных от UART для вторичного NTRIP сервера
/// Аналогичен первичному серверу, но отправляет на второй кастер
/// Оба сервера получают одинаковые RTK данные от одного UART
static void ntrip_server_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    EventBits_t event_bits = xEventGroupGetBits(server_event_group);

    // Установка флага готовности данных при первом поступлении
    if ((event_bits & DATA_READY_BIT) == 0) {
        xEventGroupSetBits(server_event_group, DATA_READY_BIT);

        if (event_bits & DATA_SENT_BIT)
            ESP_LOGI(TAG, "Data received by UART, will now reconnect to caster if disconnected");
    }
    data_keep_alive = 0;                                // Сброс счётчика keep-alive

    // Игнорирование данных если второй кастер не готов
    if ((event_bits & CASTER_READY_BIT) == 0) return;

    // Установка флага успешной отправки при первой передаче
    if ((event_bits & DATA_SENT_BIT) == 0) xEventGroupSetBits(server_event_group, DATA_SENT_BIT);

    // Защита сокета мьютексом от одновременного доступа
    if (xSemaphoreTake(sock_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (sock >= 0) {
            // Отправка RTK данных во второй NTRIP кастер
            int sent = write(sock, buffer, length);
            if (sent < 0) {
                // При ошибке - закрытие сокета и переподключение
                destroy_socket(&sock);
                xSemaphoreGive(sock_mutex);
                vTaskResume(server_task);
                return;
            } else {
                // Обновление статистики для второго сервера
                stream_stats_increment(stream_stats, 0, sent);
            }
        }
        xSemaphoreGive(sock_mutex);
    }
}

/// Задача контроля keep-alive для вторичного NTRIP сервера
/// Независимо от первичного сервера контролирует время жизни соединения
static void ntrip_server_sleep_task(void *ctx) {
    vTaskSuspend(NULL);                                 // Начальная приостановка до активации

    while (true) {
        // Проверка превышения времени ожидания данных от UART
        if (data_keep_alive == NTRIP_KEEP_ALIVE_THRESHOLD) {
            xEventGroupClearBits(server_event_group, DATA_READY_BIT);
            ESP_LOGW(TAG, "No data received by UART in %d seconds, will not reconnect to caster if disconnected", NTRIP_KEEP_ALIVE_THRESHOLD / 1000);
        }
        data_keep_alive += NTRIP_KEEP_ALIVE_THRESHOLD / 10;
        vTaskDelay(pdMS_TO_TICKS(NTRIP_KEEP_ALIVE_THRESHOLD / 10));
    }
}

/// Основная задача вторичного NTRIP сервера
/// Полностью независима от первичного сервера, использует отдельную конфигурацию
/// Подключается ко второму кастеру с собственными параметрами подключения
static void ntrip_server_task(void *ctx) {
    // Инициализация независимой группы событий для второго сервера
    server_event_group = xEventGroupCreate();
    // Создание мьютекса для защиты сокета
    sock_mutex = xSemaphoreCreateMutex();
    if (sock_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create socket mutex");
        vTaskDelete(NULL);
        return;
    }
    // Регистрация обработчика UART (оба сервера получают одинаковые данные)
    uart_register_read_handler(ntrip_server_uart_handler);
    // Создание независимой задачи keep-alive для второго сервера
    xTaskCreate(ntrip_server_sleep_task, "ntrip_server_sleep_task", 2048, NULL, TASK_PRIORITY_INTERFACE, &sleep_task);

    // Настройка отдельного статусного светодиода для второго сервера
    config_color_t status_led_color = config_get_color(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_COLOR));
    if (status_led_color.rgba != 0) status_led = status_led_add(status_led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led != NULL) status_led->active = false;

    // Создание отдельной статистики для второго сервера
    stream_stats = stream_stats_new("ntrip_server_2");

    // Независимый механизм повторных подключений для второго сервера
    retry_delay_handle_t delay_handle = retry_init(true, 5, 2000, 0);

    // Выделение буфера один раз вне цикла для оптимизации памяти
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        retry_delay(delay_handle);

        /* Ожидание наличия данных от UART для второго сервера */
        if ((xEventGroupGetBits(server_event_group) & DATA_READY_BIT) == 0) {
            ESP_LOGI(TAG, "Waiting for UART input to connect to caster");
            uart_nmea("$PESP,NTRIP,SRV2,WAITING");        // NMEA сообщение для второго сервера
            xEventGroupWaitBits(server_event_group, DATA_READY_BIT, true, false, portMAX_DELAY);
        }

        vTaskResume(sleep_task);                                // Активация keep-alive контроля

        wait_for_ip();                                          // Ожидание WiFi подключения

        /* Загрузка отдельной конфигурации для второго NTRIP кастера */
        char *host = NULL, *mountpoint = NULL, *password = NULL;
        uint16_t port = config_get_u16(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_PORT));
        config_get_primitive(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_PORT), &port);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_HOST), (void **) &host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_PASSWORD), (void **) &password);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT), (void **) &mountpoint);

        // Проверка успешного выделения памяти для конфигурационных строк
        if (!host || !password || !mountpoint) {
            ESP_LOGE(TAG, "Failed to allocate memory for configuration strings");
            goto _error;
        }

        ESP_LOGI(TAG, "Connecting to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV2,CONNECTING,%s:%d,%s", host, port, mountpoint);
        sock = connect_socket(host, port, SOCK_STREAM);
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_RESOLVE, goto _error, "Could not resolve host");
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_CONNECT, goto _error, "Could not connect to host");

        snprintf(buffer, BUFFER_SIZE, "SOURCE %s /%s" NEWLINE \
                "Source-Agent: NTRIP %s/%s" NEWLINE \
                NEWLINE, password, mountpoint, NTRIP_SERVER_NAME, &esp_app_get_description()->version[1]);

        int err = write(sock, buffer, strlen(buffer));
        ERROR_ACTION(TAG, err < 0, goto _error, "Could not send request to caster: %d %s", errno, strerror(errno));

        int len = read(sock, buffer, BUFFER_SIZE - 1);
        ERROR_ACTION(TAG, len <= 0, goto _error, "Could not receive response from caster: %d %s", errno, strerror(errno));
        buffer[len] = '\0';

        char *status = extract_http_header(buffer, "");
        ERROR_ACTION(TAG, status == NULL || !ntrip_response_ok(status), free(status); goto _error,
                "Could not connect to mountpoint: %s", status == NULL ? "HTTP response malformed" : status);
        free(status);

        /* Успешное подключение ко второму кастеру */
        ESP_LOGI(TAG, "Successfully connected to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV2,CONNECTED,%s:%d,%s", host, port, mountpoint);  // SRV2 для второго сервера

        retry_reset(delay_handle);                              // Сброс счётчика попыток

        if (status_led != NULL) status_led->active = true;     // Включение светодиода второго сервера

        /* Установка готовности второго кастера */
        xEventGroupSetBits(server_event_group, CASTER_READY_BIT);

        /* Приостановка до отключения */
        vTaskSuspend(NULL);

        /* Обработка отключения от второго кастера */
        xEventGroupClearBits(server_event_group, CASTER_READY_BIT | DATA_SENT_BIT);

        if (status_led != NULL) status_led->active = false;    // Отключение светодиода

        ESP_LOGW(TAG, "Disconnected from %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV2,DISCONNECTED,%s:%d,%s", host, port, mountpoint);  // SRV2 для второго сервера

        _error:
        vTaskSuspend(sleep_task);

        destroy_socket(&sock);

        // Освобождение выделенной памяти для конфигурационных строк
        if (host) free(host);
        if (mountpoint) free(mountpoint);
        if (password) free(password);
    }
    
    // Освобождение буфера при выходе из задачи (никогда не должно произойти)
    free(buffer);
}

/// Инициализация вторичного NTRIP сервера
/// Создаёт независимую задачу сервера только если он активен в конфигурации
/// Работает параллельно с первичным сервером, подключаясь ко второму кастеру
void ntrip_server_2_init() {
    // Проверка активности второго сервера в конфигурации NVS
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_ACTIVE))) return;

    // Создание задачи второго NTRIP сервера с тем же приоритетом что и у первого
    xTaskCreate(ntrip_server_task, "ntrip_server_2_task", 4096, NULL, TASK_PRIORITY_INTERFACE, &server_task);
}
