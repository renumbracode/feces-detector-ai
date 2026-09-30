#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "spray_controller.h"

static const char *TAG = "spray";

static int s_relay_pin = -1;
static int s_led_pin = -1;
static int s_buzzer_pin = -1;
static float s_threshold = 0.60f;
static uint32_t s_cooldown_ms = 300000u;
static uint32_t s_spray_ms = 5000u;

static bool s_spraying = false;
static bool s_cooldown_active = false;
static float s_last_confidence = 0.0f;
static uint64_t s_last_spray_end_us = 0;
static uint64_t s_spray_stop_us = 0;

/* Drive the buzzer with a ~2kHz square wave rather than a plain GPIO level.
 * A passive buzzer on a bare GPIO only clicks silently; 2kHz covers both
 * passive (sounds) and active (ignores the modulation, sounds its own tone)
 * parts, so an unknown buzzer from a kit works either way. The camera owns
 * LEDC_TIMER_0/CHANNEL_0, so this uses TIMER_1/CHANNEL_1. */
static void prv_buzzer(bool on)
{
    if (s_buzzer_pin < 0) return;
    if (on) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 512); /* 50% of 1023 */
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ledc_timer_resume(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1);
    }
}

static void prv_relay(bool on)
{
    if (s_relay_pin >= 0)  gpio_set_level(s_relay_pin,  on ? 1 : 0);
    if (s_led_pin >= 0)    gpio_set_level(s_led_pin,    on ? 1 : 0);
    prv_buzzer(on);
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

void spray_controller_init(int relay_pin, int led_pin, int buzzer_pin,
                           float threshold,
                           uint32_t cooldown_ms, uint32_t spray_ms)
{
    s_relay_pin = relay_pin;
    s_led_pin = led_pin;
    s_buzzer_pin = buzzer_pin;
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

    if (s_buzzer_pin >= 0) {
        ledc_timer_config_t ledc_cfg = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num       = LEDC_TIMER_1,   /* camera uses TIMER_0 */
            .freq_hz         = 2000,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        /* A buzzer failure must not take down the spray controller: the LED
         * and relay still give a visible/tangible alarm, so log and continue. */
        if (ledc_timer_config(&ledc_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "buzzer timer config failed; continuing without it");
            s_buzzer_pin = -1;
        }
        ledc_channel_config_t ch_cfg = {
            .gpio_num   = s_buzzer_pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_1,
            .timer_sel  = LEDC_TIMER_1,
            .duty       = 0,
            .hpoint     = 0,
        };
        if (ledc_channel_config(&ch_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "buzzer channel config failed; continuing without it");
            s_buzzer_pin = -1;
        }
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
