#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "spray_controller.h"

static const char *TAG = "spray";

static int s_relay_pin = -1;
static int s_led_pin = -1;
static float s_threshold = 0.60f;
static uint32_t s_cooldown_ms = 300000u;
static uint32_t s_spray_ms = 5000u;

static bool s_spraying = false;
static bool s_cooldown_active = false;
static float s_last_confidence = 0.0f;
static uint64_t s_last_spray_end_us = 0;
static uint64_t s_spray_stop_us = 0;

static void prv_relay(bool on)
{
    if (s_relay_pin >= 0) gpio_set_level(s_relay_pin, on ? 1 : 0);
    if (s_led_pin >= 0)   gpio_set_level(s_led_pin,   on ? 1 : 0);
}

static void prv_turn_on(uint32_t duration_ms)
{
    s_spraying = true;
    s_cooldown_active = true;
    s_spray_stop_us = esp_timer_get_time() + (uint64_t)duration_ms * 1000u;
    prv_relay(true);
    ESP_LOGI(TAG, "Spray ON (%u ms)", (unsigned)duration_ms);
}

static void spray_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_spraying && esp_timer_get_time() >= s_spray_stop_us) {
            s_spraying = false;
            s_last_spray_end_us = esp_timer_get_time();
            prv_relay(false);
            ESP_LOGI(TAG, "Spray OFF");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void spray_controller_init(int relay_pin, int led_pin, float threshold,
                           uint32_t cooldown_ms, uint32_t spray_ms)
{
    s_relay_pin = relay_pin;
    s_led_pin = led_pin;
    s_threshold = threshold;
    s_cooldown_ms = cooldown_ms;
    s_spray_ms = spray_ms;

    if (s_relay_pin >= 0) {
        gpio_reset_pin(s_relay_pin);
        gpio_set_direction(s_relay_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(s_relay_pin, 0);
    }
    if (s_led_pin >= 0) {
        gpio_reset_pin(s_led_pin);
        gpio_set_direction(s_led_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(s_led_pin, 0);
    }

    xTaskCreatePinnedToCore(spray_task, "spray", 2048, NULL, 5, NULL, 0);
}

bool spray_controller_on_detection(float confidence)
{
    s_last_confidence = confidence;

    if (confidence < s_threshold) return false;
    if (s_spraying) return false;

    /* Respect cooldown */
    uint64_t now = esp_timer_get_time();
    if (s_cooldown_active) {
        if (now - s_last_spray_end_us < (uint64_t)s_cooldown_ms * 1000u) {
            return false; /* in cooldown -> detection recorded but no spray */
        }
        s_cooldown_active = false;
    }

    prv_turn_on(s_spray_ms);
    return true;
}

bool spray_controller_force_spray(uint32_t duration_ms)
{
    if (duration_ms == 0) duration_ms = s_spray_ms;
    if (s_spraying) return false;
    prv_turn_on(duration_ms);
    return true;
}

bool spray_controller_is_spraying(void)
{
    return s_spraying;
}

void spray_controller_status(spray_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->spraying = s_spraying;
    uint64_t now = esp_timer_get_time();
    out->last_spray_ago_ms = (now - s_last_spray_end_us) / 1000u;
    out->last_confidence = s_last_confidence;
    out->cooldown_active = s_cooldown_active;
    out->cooldown_until_ms = (s_last_spray_end_us + (uint64_t)s_cooldown_ms * 1000u) / 1000u;
}

void spray_controller_set_config(float threshold, uint32_t cooldown_ms, uint32_t spray_ms)
{
    s_threshold = threshold;
    s_cooldown_ms = cooldown_ms;
    s_spray_ms = spray_ms;
}
