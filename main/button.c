#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "bt_a2dp.h"
#include "button.h"
#include "creeper_audio.h"

static const char *TAG = "button";

#define POLL_MS     10
#define DEBOUNCE_MS 30

static void button_task(void *arg)
{
    int stable = 1, last = 1, same_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int level = gpio_get_level(CONFIG_CREEPER_BUTTON_GPIO);
        same_ms = level == last ? same_ms + POLL_MS : 0;
        last = level;

        if (same_ms >= DEBOUNCE_MS && level != stable) {
            stable = level;
            if (stable == 0) { /* pressed (active low) */
                ESP_LOGI(TAG, "pressed");
                if (!bt_a2dp_streaming()) {
                    ESP_LOGW(TAG, "not connected to the speaker yet");
                } else if (!creeper_trigger()) {
                    ESP_LOGI(TAG, "already exploding");
                }
            }
        }
    }
}

void button_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CREEPER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    xTaskCreatePinnedToCore(button_task, "button", 2560, NULL, 4, NULL, 1);
}
