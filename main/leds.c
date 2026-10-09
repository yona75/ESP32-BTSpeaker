/*
 * WS2812 effects that follow the audio: green pulsing during the hiss,
 * blinding white flicker during the explosion. The audio phase/envelope is
 * sampled into a ring buffer and played back delayed by the Bluetooth latency.
 */
#include "sdkconfig.h"
#include "leds.h"

#if CONFIG_CREEPER_LED_ENABLE

#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "led_strip.h"

#include "creeper_audio.h"

static const char *TAG = "leds";

#define FRAME_MS   20
#define HISTORY    64 /* 1.28 s of history, enough for any latency setting */
#define DELAY_FRAMES (CONFIG_CREEPER_LED_LATENCY_MS / FRAME_MS)

typedef struct {
    uint8_t phase;
    float level;
} snap_t;

static led_strip_handle_t s_strip;

static inline uint8_t scale(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 1.0f) {
        v = 1.0f;
    }
    return (uint8_t)(v * CONFIG_CREEPER_LED_MAX_BRIGHTNESS);
}

static void draw_hiss(float level, uint32_t frame, uint32_t *pulse)
{
    /* Pulse speeds up as the hiss swells (level 0.1 -> 1). */
    *pulse += 1 + (uint32_t)(level * 7.0f);
    float wave = 0.5f + 0.5f * sinf(*pulse * 0.08f);
    float g = 0.15f + 0.85f * level * wave;

    for (int i = 0; i < CONFIG_CREEPER_LED_COUNT; i++) {
        /* Slight per-LED variation so the strip looks alive. */
        float j = 0.85f + 0.15f * sinf(i * 0.9f + frame * 0.3f);
        led_strip_set_pixel(s_strip, i, scale(g * 0.15f * j), scale(g * j), scale(g * 0.1f * j));
    }
}

static void draw_explosion(float level)
{
    for (int i = 0; i < CONFIG_CREEPER_LED_COUNT; i++) {
        /* Full white, randomly dropping out; flicker gets sparser as it fades. */
        uint32_t r = esp_random();
        float on = (r & 0xff) < (uint32_t)(80 + 175 * level) ? 1.0f : 0.25f;
        float v = fminf(1.0f, level * 1.6f) * on;
        /* Warm the tail slightly toward orange as it dies down. */
        float warm = 1.0f - level;
        led_strip_set_pixel(s_strip, i, scale(v), scale(v * (1.0f - 0.3f * warm)), scale(v * (1.0f - 0.8f * warm)));
    }
}

static void leds_task(void *arg)
{
    static snap_t hist[HISTORY];
    uint32_t frame = 0, pulse = 0;
    uint8_t prev_phase = CREEPER_IDLE;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        hist[frame % HISTORY] = (snap_t){ .phase = creeper_phase(), .level = creeper_level() };
        snap_t s = frame >= DELAY_FRAMES ? hist[(frame - DELAY_FRAMES) % HISTORY] : (snap_t){ 0 };

        switch (s.phase) {
        case CREEPER_HISS:
            draw_hiss(s.level, frame, &pulse);
            break;
        case CREEPER_EXPLODE:
            draw_explosion(s.level);
            break;
        default:
            pulse = 0;
            led_strip_clear(s_strip);
            break;
        }
        /* Idle frames only need one refresh to blank the strip. */
        if (s.phase != CREEPER_IDLE || prev_phase != CREEPER_IDLE) {
            led_strip_refresh(s_strip);
        }
        prev_phase = s.phase;

        frame++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

void leds_start(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_CREEPER_LED_GPIO,
        .max_leds = CONFIG_CREEPER_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "%d LEDs on GPIO%d", CONFIG_CREEPER_LED_COUNT, CONFIG_CREEPER_LED_GPIO);
    xTaskCreatePinnedToCore(leds_task, "leds", 3072, NULL, 3, NULL, 1);
}

#else

void leds_start(void)
{
}

#endif
