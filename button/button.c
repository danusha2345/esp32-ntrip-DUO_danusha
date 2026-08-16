#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "button.h"

#define TAG "BUTTON"

typedef struct {
	uint8_t pin;
    bool inverted;
	uint16_t history;
    uint64_t down_time;
} debounce_t;

int pin_count = -1;
debounce_t * debounce;
QueueHandle_t queue;

static void update_button(debounce_t *d) {
    d->history = (d->history << 1) | gpio_get_level(d->pin);
}

#define MASK   0b1111000000111111
static bool button_rose(debounce_t *d) {
    if ((d->history & MASK) == 0b0000000000111111) {
        d->history = 0xffff;
        return 1;
    }
    return 0;
}
static bool button_fell(debounce_t *d) {
    if ((d->history & MASK) == 0b1111000000000000) {
        d->history = 0x0000;
        return 1;
    }
    return 0;
}
static bool button_down(debounce_t *d) {
    if (d->inverted) return button_fell(d);
    return button_rose(d);
}
static bool button_up(debounce_t *d) {
    if (d->inverted) return button_rose(d);
    return button_fell(d);
}

#define LONG_PRESS_DURATION (2000)

static uint32_t millis() {
    return esp_timer_get_time() / 1000;
}

static void send_event(debounce_t db, int ev) {
    button_event_t event = {
        .pin = db.pin,
        .event = ev,
        .duration = millis() - db.down_time,
    };
    xQueueSend(queue, &event, portMAX_DELAY);
}

static void button_task(void *pvParameter)
{
    while (1) {
        for (int idx=0; idx<pin_count; idx++) {
            update_button(&debounce[idx]);
            if (debounce[idx].down_time && (millis() - debounce[idx].down_time > LONG_PRESS_DURATION)) {
                ESP_LOGI(TAG, "%d LONG", debounce[idx].pin);
                int i=0;
                while (!button_up(&debounce[idx])) {
                    if (!i) send_event(debounce[idx], BUTTON_DOWN);
                    i++;
                    if (i>=10) i=0;
                    vTaskDelay(10/portTICK_PERIOD_MS);
                    update_button(&debounce[idx]);
                }
                ESP_LOGI(TAG, "%d UP", debounce[idx].pin);
                send_event(debounce[idx], BUTTON_UP);

                debounce[idx].down_time = 0;
            } else if (button_down(&debounce[idx])) {
                debounce[idx].down_time = millis();
                send_event(debounce[idx], BUTTON_DOWN);
            } else if (button_up(&debounce[idx])) {
                send_event(debounce[idx], BUTTON_UP);
                debounce[idx].down_time = 0;
            }
        }
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

QueueHandle_t button_init(unsigned long long pin_select) {
    if (pin_count != -1) {
        ESP_LOGI(TAG, "Already initialized");
        return NULL;
    }

    // Configure the pins
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = pin_select,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIOs");
        return NULL;
    }

    // Scan the pin map to determine number of pins
    pin_count = 0;
    for (int pin=0; pin<SOC_GPIO_PIN_COUNT; pin++) {
        if ((1ULL<<pin) & pin_select) {
            pin_count++;
        }
    }

    // Initialize global state and queue
    debounce = calloc(pin_count, sizeof(debounce_t));
    queue = xQueueCreate(4, sizeof(button_event_t));
    if (pin_count == 0 || debounce == NULL || queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate button resources");
        free(debounce);
        debounce = NULL;
        if (queue != NULL) vQueueDelete(queue);
        queue = NULL;
        pin_count = -1;
        return NULL;
    }

    // Scan the pin map to determine each pin number, populate the state
    uint32_t idx = 0;
    for (int pin=0; pin<SOC_GPIO_PIN_COUNT; pin++) {
        if ((1ULL<<pin) & pin_select) {
            ESP_LOGD(TAG, "Registering button input: %d", pin);
            debounce[idx].pin = pin;
            debounce[idx].down_time = 0;
            debounce[idx].inverted = true;
            if (debounce[idx].inverted) debounce[idx].history = 0xffff;
            idx++;
        }
    }

    // Spawn a task to monitor the pins
    if (xTaskCreate(&button_task, "button_task", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        free(debounce);
        debounce = NULL;
        vQueueDelete(queue);
        queue = NULL;
        pin_count = -1;
        return NULL;
    }

    return queue;
}
