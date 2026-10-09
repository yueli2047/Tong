#include <stdbool.h>
#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define POLL_INTERVAL_MS  10
#define DEBOUNCE_COUNT    3     // 3 x 10 ms = 30 ms stable
#define QUEUE_LEN         8

static const char *TAG = "buttons";

typedef struct {
    int gpio;
    button_t id;
    bool pressed;   // debounced state
    uint8_t count;  // consecutive samples that disagree with debounced state
} button_state_t;

static button_state_t s_btn[2];
static QueueHandle_t s_queue = NULL;

static void poll_task(void *arg)
{
    while (1) {
        for (int i = 0; i < 2; i++) {
            bool raw = (gpio_get_level(s_btn[i].gpio) == 0);  // active low
            if (raw != s_btn[i].pressed) {
                if (++s_btn[i].count >= DEBOUNCE_COUNT) {
                    s_btn[i].pressed = raw;
                    s_btn[i].count = 0;
                    if (raw) {  // released -> pressed
                        button_t ev = s_btn[i].id;
                        xQueueSend(s_queue, &ev, 0);
                    }
                }
            } else {
                s_btn[i].count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t buttons_init(int yes_gpio, int no_gpio)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << yes_gpio) | (1ULL << no_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_btn[0] = (button_state_t){ .gpio = yes_gpio, .id = BUTTON_YES };
    s_btn[1] = (button_state_t){ .gpio = no_gpio,  .id = BUTTON_NO };

    s_queue = xQueueCreate(QUEUE_LEN, sizeof(button_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(poll_task, "buttons", 2048, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init ok (yes=%d, no=%d)", yes_gpio, no_gpio);
    return ESP_OK;
}

button_t buttons_wait(TickType_t timeout)
{
    button_t ev = BUTTON_NONE;
    if (s_queue != NULL) {
        xQueueReceive(s_queue, &ev, timeout);
    }
    return ev;
}

void buttons_flush(void)
{
    if (s_queue != NULL) {
        xQueueReset(s_queue);
    }
}