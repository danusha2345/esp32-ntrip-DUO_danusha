/*
 * ESP32 NTRIP Duo - Главный файл приложения
 * Основан на ESP32-XBee (https://github.com/nebkat/esp32-xbee)
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * Двойной NTRIP клиент/сервер для ESP32, обеспечивающий:
 * - Пересылку UART данных на два независимых NTRIP сервера
 * - Веб-интерфейс для конфигурации
 * - WiFi Station/AP режимы
 * - TCP/UDP Socket сервер/клиент
 * - SD карта для логирования
 * - Статусные светодиоды и кнопка сброса
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

#include <web_server.h>
#include <log.h>
#include <status_led.h>
#include "interface/socket_server.h"
#include "interface/socket_client.h"
#include "sd_logger.h"

#include <esp_sntp.h>
#include <core_dump.h>
#include <esp_ota_ops.h>
#include <stream_stats.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "button.h"

#include "config.h"
#include "wifi.h"

#include "uart.h"
#include "interface/ntrip.h"
#include "tasks.h"

// GPIO пины кнопки сброса для различных типов чипов ESP32
#ifdef CONFIG_IDF_TARGET_ESP32
#define BUTTON_GPIO GPIO_NUM_0      // Стандартный ESP32: GPIO0 (boot кнопка)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define BUTTON_GPIO GPIO_NUM_0      // ESP32-C3: GPIO0 (boot кнопка)
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define BUTTON_GPIO GPIO_NUM_0      // ESP32-S3: GPIO0 (boot кнопка)
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#define BUTTON_GPIO GPIO_NUM_9      // ESP32-C6: GPIO9 (boot кнопка)
#else
// Резервное значение для других чипов
#define BUTTON_GPIO GPIO_NUM_0
#endif

static const char *TAG = "MAIN";  // Тег для логирования в основном модуле

/// Преобразует код причины сброса в читаемую строку
static char *reset_reason_name(esp_reset_reason_t reason);

/// Задача обработки кнопки сброса настроек
/// Сбрасывает конфигурацию при удержании кнопки более 5 секунд
static void reset_button_task(void *pvParameters) {
    // Инициализация кнопки и очереди событий
    QueueHandle_t button_queue = button_init(PIN_BIT(BUTTON_GPIO));
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY); // Подтягивающий резистор к VCC
    
    while (true) {
        button_event_t button_ev;
        // Ожидание события кнопки с таймаутом 1 секунда
        if (xQueueReceive(button_queue, &button_ev, 1000 / portTICK_PERIOD_MS)) {
            // Проверка длительного нажатия (>5 сек) для сброса конфигурации
            if (button_ev.event == BUTTON_DOWN && button_ev.duration > 5000) {
                config_reset();                                // Очистка настроек в NVS
                vTaskDelay(2000 / portTICK_PERIOD_MS);        // Пауза 2 секунды
                esp_restart();                                // Перезагрузка ESP32
            }
        }
    }
}

/// Обработчик синхронизации времени SNTP
/// Вызывается при успешной синхронизации времени с NTP сервером
static void sntp_time_set_handler(struct timeval *tv) {
    ESP_LOGI(TAG, "Synced time from SNTP");
}

/// Главная функция приложения ESP32 NTRIP Duo
/// Инициализирует все модули и запускает основные сервисы
void app_main()
{
    // Инициализация системы статусных светодиодов
    status_led_init();
    // Добавление белого светодиода с плавным затуханием (старт системы)
    status_led_handle_t status_led = status_led_add(0xFFFFFF33, STATUS_LED_FADE, 250, 2500, 0);

    // Инициализация системы логирования
    log_init();
    esp_log_set_vprintf(log_vprintf);                         // Перенаправление логов
    // Установка уровней логирования (подавление избыточных сообщений)
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("system_api", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    // Проверка core dump после возможного краха системы
    core_dump_check();

    // Создание задачи обработки кнопки сброса (приоритет определён в tasks.h)
    xTaskCreate(reset_button_task, "reset_button", 4096, NULL, TASK_PRIORITY_RESET_BUTTON, NULL);

    // Инициализация статистики потоков данных
    stream_stats_init();

    // Инициализация системы конфигурации (NVS) и UART
    config_init();
    uart_init();

    // Получение причины последнего сброса ESP32
    esp_reset_reason_t reset_reason = esp_reset_reason();

    // Получение информации о прошивке и её хеша
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char elf_buffer[17];                                      // Буфер для SHA256 хеша (16 байт + \0)
    esp_app_get_elf_sha256(elf_buffer, sizeof(elf_buffer));

    // Отправка NMEA сообщения о начале инициализации в UART
    uart_nmea("$PESP,INIT,START,%s,%s", app_desc->version, reset_reason_name(reset_reason));

    // Вывод красивого баннера с информацией о прошивке в лог
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║ ESP32 XBee %-33s "                          "║", app_desc->version);
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Compiled: %8s %-25s "                       "║", app_desc->time, app_desc->date);
    ESP_LOGI(TAG, "║ ELF SHA256: %-32s "                         "║", elf_buffer);
    ESP_LOGI(TAG, "║ ESP-IDF: %-35s "                            "║", app_desc->idf_ver);
    ESP_LOGI(TAG, "╟──────────────────────────────────────────────╢");
    ESP_LOGI(TAG, "║ Reset reason: %-30s "                       "║", reset_reason_name(reset_reason));
    ESP_LOGI(TAG, "╟──────────────────────────────────────────────╢");
    ESP_LOGI(TAG, "║ Author: Nebojša Cvetković                    ║");
    ESP_LOGI(TAG, "║ Source: https://github.com/nebkat/esp32-xbee ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");

    // Создание основного цикла событий ESP-IDF
    esp_event_loop_create_default();

    // Пауза 2.5 секунды, затем переключение светодиода в режим мигания
    vTaskDelay(pdMS_TO_TICKS(2500));
    status_led->interval = 100;                               // Интервал между миганиями: 100мс
    status_led->duration = 1000;                              // Длительность цикла: 1сек
    status_led->flashing_mode = STATUS_LED_BLINK;             // Режим мигания

    // Обработка аварийных перезагрузок - показ красного светодиода
    if (reset_reason != ESP_RST_POWERON && reset_reason != ESP_RST_SW && reset_reason != ESP_RST_WDT) {
        status_led->active = false;                           // Отключение основного светодиода
        // Красный мигающий светодиод для индикации ошибки (10 секунд)
        status_led_handle_t error_led = status_led_add(0xFF000033, STATUS_LED_BLINK, 50, 10000, 0);

        vTaskDelay(pdMS_TO_TICKS(10000));                     // Показ ошибки 10 секунд

        status_led_remove(error_led);                         // Удаление индикатора ошибки
        status_led->active = true;                            // Восстановление основного светодиода
    }

    // Инициализация сетевого стека ESP32
    esp_netif_init();

    // Инициализация WiFi (Station/AP режимы)
    wifi_init();

    // Запуск веб-сервера для конфигурации
    web_server_init();

    // Инициализация основных NTRIP серверов (двойной режим)
    ntrip_server_init();                                      // Первичный NTRIP сервер
    ntrip_server_2_init();                                    // Вторичный NTRIP сервер

    // Инициализация дополнительных сетевых сервисов
    socket_server_init();                                     // TCP/UDP Socket сервер
    socket_client_init();                                     // TCP/UDP Socket клиент

    // Инициализация логирования на SD карту
    sd_logger_init();

    // NMEA сообщение о завершении инициализации
    uart_nmea("$PESP,INIT,COMPLETE");

    // Ожидание получения IP адреса (WiFi подключение)
    wait_for_ip();

    // Настройка и запуск SNTP клиента для синхронизации времени
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);             // Режим периодического опроса
    esp_sntp_setservername(0, "pool.ntp.org");               // Публичный пул NTP серверов
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);           // Плавная синхронизация времени
    esp_sntp_set_time_sync_notification_cb(sntp_time_set_handler);  // Callback синхронизации
    esp_sntp_init();                                          // Запуск SNTP клиента

#ifdef DEBUG_HEAP
    // Режим отладки heap памяти - периодический вывод статистики использования
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));                      // Пауза 2 секунды

        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);        // Получение информации о heap

        // Отправка NMEA сообщения со статистикой памяти
        uart_nmea("$PESP,HEAP,FREE,%d/%d,%d%%", info.total_free_bytes,
                info.total_allocated_bytes + info.total_free_bytes,
                100 * info.total_free_bytes / (info.total_allocated_bytes + info.total_free_bytes));
    }
#endif
}

/// Преобразует код причины сброса ESP32 в читаемое текстовое описание
/// @param reason Код причины сброса из esp_reset_reason_t
/// @return Строка с описанием причины сброса
static char *reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        default:
        case ESP_RST_UNKNOWN:
            return "UNKNOWN";                                 // Неизвестная причина
        case ESP_RST_POWERON:
            return "POWERON";                                 // Включение питания
        case ESP_RST_EXT:
            return "EXTERNAL";                                // Внешний сброс
        case ESP_RST_SW:
            return "SOFTWARE";                                // Программный сброс
        case ESP_RST_PANIC:
            return "PANIC";                                   // Паника системы
        case ESP_RST_INT_WDT:
            return "INTERRUPT_WATCHDOG";                      // Watchdog прерываний
        case ESP_RST_TASK_WDT:
            return "TASK_WATCHDOG";                           // Watchdog задач
        case ESP_RST_WDT:
            return "OTHER_WATCHDOG";                          // Другой watchdog
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";                               // Выход из глубокого сна
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";                                // Просадка напряжения
        case ESP_RST_SDIO:
            return "SDIO";                                    // Сброс через SDIO
    }
}