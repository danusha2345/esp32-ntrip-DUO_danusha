/*
 * ESP32 NTRIP Duo - Первичный NTRIP сервер
 * Основан на ESP32-XBee (https://github.com/nebkat/esp32-xbee)
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * Реализует клиент NTRIP протокола для передачи данных RTK коррекций
 * от базовой станции через UART на NTRIP кастер в Интернете.
 * 
 * Основные функции:
 * - Подключение к NTRIP кастеру как источник данных (SOURCE)
 * - Автоматическое переподключение при обрывах связи
 * - Контроль наличия данных от UART
 * - Статистика передачи данных
 * - Статусный светодиод состояния подключения
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

static const char *TAG = "NTRIP_SERVER";         // Тег для логирования первичного NTRIP сервера

#define BUFFER_SIZE 512                             // Размер буфера для сетевых операций (байт)

// Биты состояния для синхронизации между задачами
static const int CASTER_READY_BIT = BIT0;           // Кастер готов принимать данные
static const int DATA_READY_BIT = BIT1;             // Данные доступны от UART
static const int DATA_SENT_BIT = BIT2;              // Данные были отправлены хотя бы раз

static int sock = -1;                               // Сокет соединения с NTRIP кастером
static SemaphoreHandle_t sock_mutex = NULL;         // Мьютекс для защиты сокета от race conditions

static int data_keep_alive;                         // Счётчик времени без данных (мс)
static EventGroupHandle_t server_event_group;       // Группа событий для синхронизации

static status_led_handle_t status_led = NULL;       // Дескриптор статусного светодиода
static stream_stats_handle_t stream_stats = NULL;   // Дескриптор статистики потока

static TaskHandle_t server_task = NULL;             // Дескриптор основной задачи сервера
static TaskHandle_t sleep_task = NULL;              // Дескриптор задачи контроля keep-alive

/// Обработчик данных от UART - передача на NTRIP кастер
/// Вызывается при поступлении данных RTK коррекций с базовой станции
/// @param handler_args Аргументы обработчика (не используются)
/// @param base База событий ESP (не используется)
/// @param length Длина полученных данных (байт)
/// @param buffer Буфер с данными RTK коррекций
static void ntrip_server_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    EventBits_t event_bits = xEventGroupGetBits(server_event_group);

    // Установка бита готовности данных при первом поступлении
    if ((event_bits & DATA_READY_BIT) == 0) {
        xEventGroupSetBits(server_event_group, DATA_READY_BIT);

        // Уведомление о возобновлении данных после перерыва
        if (event_bits & DATA_SENT_BIT)
            ESP_LOGI(TAG, "Data received by UART, will now reconnect to caster if disconnected");
    }
    data_keep_alive = 0;                                // Сброс счётчика keep-alive

    // Игнорирование данных если кастер не готов к приёму
    if ((event_bits & CASTER_READY_BIT) == 0) return;

    // Установка флага успешной отправки при первой передаче
    if ((event_bits & DATA_SENT_BIT) == 0) xEventGroupSetBits(server_event_group, DATA_SENT_BIT);

    // Защита сокета мьютексом от одновременного доступа
    if (xSemaphoreTake(sock_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (sock >= 0) {
            // Отправка RTK данных в сокет NTRIP кастера
            int sent = write(sock, buffer, length);
            if (sent < 0) {
                // При ошибке отправки - закрытие сокета и перезапуск соединения
                destroy_socket(&sock);
                xSemaphoreGive(sock_mutex);
                vTaskResume(server_task);                   // Пробуждение основной задачи для переподключения
                return;
            } else {
                // Обновление статистики переданных данных
                stream_stats_increment(stream_stats, 0, sent);
            }
        }
        xSemaphoreGive(sock_mutex);
    }
}

/// Задача контроля времени жизни соединения (keep-alive)
/// Отслеживает время отсутствия данных от UART и сбрасывает флаг готовности
/// при превышении порогового времени ожидания
/// @param ctx Контекст задачи (не используется)
static void ntrip_server_sleep_task(void *ctx) {
    vTaskSuspend(NULL);                                 // Начальное приостановление до активации

    while (true) {
        // Проверка превышения времени ожидания данных от UART
        if (data_keep_alive == NTRIP_KEEP_ALIVE_THRESHOLD) {
            xEventGroupClearBits(server_event_group, DATA_READY_BIT);  // Сброс флага готовности данных
            ESP_LOGW(TAG, "No data received by UART in %d seconds, will not reconnect to caster if disconnected", NTRIP_KEEP_ALIVE_THRESHOLD / 1000);
        }
        // Инкремент счётчика времени без данных (шаг = 1/10 от порогового значения)
        data_keep_alive += NTRIP_KEEP_ALIVE_THRESHOLD / 10;
        vTaskDelay(pdMS_TO_TICKS(NTRIP_KEEP_ALIVE_THRESHOLD / 10));  // Пауза между проверками
    }
}

/// Основная задача NTRIP сервера - управление подключением к кастеру
/// Выполняет последовательность: ожидание данных -> подключение -> передача -> переподключение
/// @param ctx Контекст задачи (не используется)
static void ntrip_server_task(void *ctx) {
    // Инициализация группы событий для синхронизации
    server_event_group = xEventGroupCreate();
    // Создание мьютекса для защиты сокета
    sock_mutex = xSemaphoreCreateMutex();
    if (sock_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create socket mutex");
        vTaskDelete(NULL);
        return;
    }
    // Регистрация обработчика данных от UART
    uart_register_read_handler(ntrip_server_uart_handler);
    // Создание задачи контроля keep-alive
    xTaskCreate(ntrip_server_sleep_task, "ntrip_server_sleep_task", 2048, NULL, TASK_PRIORITY_INTERFACE, &sleep_task);

    // Настройка статусного светодиода из конфигурации
    config_color_t status_led_color = config_get_color(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_COLOR));
    if (status_led_color.rgba != 0) status_led = status_led_add(status_led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led != NULL) status_led->active = false;  // Начально неактивный светодиод

    // Инициализация статистики потока данных
    stream_stats = stream_stats_new("ntrip_server");

    // Инициализация механизма повторных подключений с экспоненциальной задержкой
    retry_delay_handle_t delay_handle = retry_init(true, 5, 2000, 0);  // Макс 5 попыток, старт 2с

    // Выделение буфера один раз вне цикла для оптимизации памяти
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }

    /* Основной цикл подключения к NTRIP кастеру */
    while (true) {
        // Применение задержки между попытками подключения (экспоненциальная задержка)
        retry_delay(delay_handle);

        /* Ожидание наличия данных от UART перед попыткой подключения */
        if ((xEventGroupGetBits(server_event_group) & DATA_READY_BIT) == 0) {
            ESP_LOGI(TAG, "Waiting for UART input to connect to caster");
            uart_nmea("$PESP,NTRIP,SRV,WAITING");
            // Блокирующее ожидание появления данных от базовой станции
            xEventGroupWaitBits(server_event_group, DATA_READY_BIT, true, false, portMAX_DELAY);
        }

        // Активация задачи контроля keep-alive после появления данных
        vTaskResume(sleep_task);

        // Ожидание получения IP адреса (WiFi подключение)
        wait_for_ip();

        /* Загрузка параметров подключения из конфигурации NVS */
        char *host = NULL, *mountpoint = NULL, *password = NULL;
        uint16_t port = config_get_u16(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_PORT));
        config_get_primitive(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_PORT), &port);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_HOST), (void **) &host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_PASSWORD), (void **) &password);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_MOUNTPOINT), (void **) &mountpoint);

        // Проверка успешного выделения памяти для конфигурационных строк
        if (!host || !password || !mountpoint) {
            ESP_LOGE(TAG, "Failed to allocate memory for configuration strings");
            goto _error;
        }

        /* Установка TCP соединения с NTRIP кастером */
        ESP_LOGI(TAG, "Connecting to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV,CONNECTING,%s:%d,%s", host, port, mountpoint);
        sock = connect_socket(host, port, SOCK_STREAM);   // TCP соединение
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_RESOLVE, goto _error, "Could not resolve host");
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_CONNECT, goto _error, "Could not connect to host");

        /* Формирование SOURCE запроса согласно NTRIP протоколу v1.0/2.0 */
        snprintf(buffer, BUFFER_SIZE, "SOURCE %s /%s" NEWLINE \
                "Source-Agent: NTRIP %s/%s" NEWLINE \
                NEWLINE, password, mountpoint, NTRIP_SERVER_NAME, &esp_app_get_description()->version[1]);

        /* Отправка SOURCE запроса на кастер */
        int err = write(sock, buffer, strlen(buffer));
        ERROR_ACTION(TAG, err < 0, goto _error, "Could not send request to caster: %d %s", errno, strerror(errno));

        /* Получение и проверка ответа кастера */
        int len = read(sock, buffer, BUFFER_SIZE - 1);
        ERROR_ACTION(TAG, len <= 0, goto _error, "Could not receive response from caster: %d %s", errno, strerror(errno));
        buffer[len] = '\0';                               // Завершение строки

        /* Парсинг HTTP статуса ответа (должен быть 200 OK) */
        char *status = extract_http_header(buffer, "");
        ERROR_ACTION(TAG, status == NULL || !ntrip_response_ok(status), free(status); goto _error,
                "Could not connect to mountpoint: %s", status == NULL ? "HTTP response malformed" : status);
        free(status);

        /* Успешное подключение к кастеру - переход в режим передачи данных */
        ESP_LOGI(TAG, "Successfully connected to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV,CONNECTED,%s:%d,%s", host, port, mountpoint);

        retry_reset(delay_handle);                        // Сброс счётчика попыток подключения

        if (status_led != NULL) status_led->active = true; // Включение статусного светодиода

        /* Установка флага готовности кастера к приёму данных */
        xEventGroupSetBits(server_event_group, CASTER_READY_BIT);

        /* Приостановка задачи до отключения от обработчика UART (при ошибке передачи) */
        vTaskSuspend(NULL);

        /* Обработка отключения от кастера */
        xEventGroupClearBits(server_event_group, CASTER_READY_BIT | DATA_SENT_BIT);

        if (status_led != NULL) status_led->active = false; // Отключение статусного светодиода

        ESP_LOGW(TAG, "Disconnected from %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV,DISCONNECTED,%s:%d,%s", host, port, mountpoint);

        /* Обработка ошибок и освобождение ресурсов */
        _error:
        vTaskSuspend(sleep_task);                         // Приостановка задачи keep-alive

        destroy_socket(&sock);                            // Закрытие сокета

        // Освобождение выделенной памяти для конфигурационных строк
        if (host) free(host);
        if (mountpoint) free(mountpoint);
        if (password) free(password);
    }
    
    // Освобождение буфера при выходе из задачи (никогда не должно произойти)
    free(buffer);
}

/// Инициализация первичного NTRIP сервера
/// Создаёт основную задачу сервера только если он активен в конфигурации
void ntrip_server_init() {
    // Проверка активности сервера в конфигурации NVS
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_ACTIVE))) return;

    // Создание основной задачи NTRIP сервера с приоритетом интерфейса
    xTaskCreate(ntrip_server_task, "ntrip_server_task", 4096, NULL, TASK_PRIORITY_INTERFACE, &server_task);
}