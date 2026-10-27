/*
 * The cleaning cycle: aim -> spray -> cooldown.
 *
 * The relay lives here and nowhere else. The camera board cannot reach it --
 * it can only send coordinates, and this board decides what happens with
 * them.
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cleaning_cycle.h"
#include "servo_driver.h"
#include "water_sensor.h"

static const char *TAG = "clean";

static int s_relay_pin = -1;
static bool s_relay_active_low = true;
static bool s_enabled = true;
static float s_water_min_pct = 20.0f;
static uint32_t s_spray_ms = 3000;
static uint32_t s_cooldown_ms = 30000;

static clean_state_t s_state = CLEAN_STATE_IDLE;
static uint64_t s_spray_stop_us = 0;
static uint64_t s_cooldown_until_us = 0;
static float s_target_pan = 0.0f;
static float s_target_tilt = 0.0f;
static uint32_t s_spray_count = 0;
static clean_reason_t s_last_reason = CLEAN_REASON_OK;

/* A manual pump test is a single explicit act, not a detection, so it is
 * held here rather than in the cycle state machine. */
static uint64_t s_manual_stop_us = 0;

static uint32_t prv_relay_off_level(void)
{
    return s_relay_active_low ? 1u : 0u;
}

static uint32_t prv_relay_on_level(void)
{
    return s_relay_active_low ? 0u : 1u;
}

static void prv_relay(bool on)
{
    if (s_relay_pin < 0) return;
    gpio_set_level(s_relay_pin, on ? prv_relay_on_level() : prv_relay_off_level());
}

const char *cleaning_cycle_reason_str(clean_reason_t r)
{
    switch (r) {
    case CLEAN_REASON_OK:             return "ok";
    case CLEAN_REASON_DISABLED:       return "disabled";
    case CLEAN_REASON_LOW_WATER:      return "low_water";
    case CLEAN_REASON_SENSOR_INVALID: return "sensor_invalid";
    case CLEAN_REASON_BUSY:           return "busy";
    case CLEAN_REASON_NO_TARGET:      return "no_target";
    case CLEAN_REASON_SOLVE_FAILED:   return "solve_failed";
    default:                          return "unknown";
    }
}

/* The interlock check, in the order the protocol specifies. The FIRST
 * failure wins so the operator is told the real problem. */
static clean_reason_t prv_check_interlocks(void)
{
    if (!s_enabled) return CLEAN_REASON_DISABLED;

    water_reading_t w = water_sensor_read();
    if (!w.valid) {
        /* Fail closed: a board that cannot read its tank must not spray. */
        return CLEAN_REASON_SENSOR_INVALID;
    }
    if (w.float_used && !w.float_ok) {
        /* The float switch overrides the analog reading -- it is the
         * stronger signal. */
        return CLEAN_REASON_LOW_WATER;
    }
    if (w.pct < s_water_min_pct) return CLEAN_REASON_LOW_WATER;

    if (s_state != CLEAN_STATE_IDLE) return CLEAN_REASON_BUSY;

    return CLEAN_REASON_OK;
}

esp_err_t cleaning_cycle_init(int relay_pin, bool relay_active_low,
                              float water_min_pct, uint32_t spray_ms,
                              uint32_t cooldown_ms)
{
    s_relay_pin = relay_pin;
    s_relay_active_low = relay_active_low;
    s_water_min_pct = water_min_pct;
    s_spray_ms = spray_ms;
    s_cooldown_ms = cooldown_ms;

    if (s_relay_pin >= 0) {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << s_relay_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&gc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "relay gpio_config failed: %s", esp_err_to_name(e));
            return e;
        }
        /* Drive the INACTIVE level at boot. On an active-low module a
         * hardcoded 0 would energise the coil the moment the board booted. */
        prv_relay(false);
    }

    xTaskCreatePinnedToCore(cleaning_cycle_task, "clean", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "cleaning cycle ready: relay=%d (%s), water_min=%.0f%%, "
                  "spray=%u ms, cooldown=%u ms",
             s_relay_pin, s_relay_active_low ? "active-low" : "active-high",
             (double)s_water_min_pct, (unsigned)s_spray_ms,
             (unsigned)s_cooldown_ms);
    return ESP_OK;
}

void cleaning_cycle_set_config(bool enabled, float water_min_pct,
                                uint32_t spray_ms, uint32_t cooldown_ms,
                                bool relay_active_low)
{
    bool polarity_changed = (relay_active_low != s_relay_active_low);
    s_enabled = enabled;
    s_water_min_pct = water_min_pct;
    s_spray_ms = spray_ms;
    s_cooldown_ms = cooldown_ms;

    if (polarity_changed) {
        s_relay_active_low = relay_active_low;
        /* Re-assert the off state under the NEW mapping. Writing the old
         * level would be wrong: for an active-low to active-high flip that
         * is a 1, which is exactly the energised level. */
        prv_relay(false);
        ESP_LOGI(TAG, "relay polarity now %s",
                 s_relay_active_low ? "active-low" : "active-high");
    }

    if (!s_enabled) {
        /* The kill switch means water stops NOW. */
        cleaning_cycle_stop("disabled by operator");
    }
}

clean_reason_t cleaning_cycle_start(float pan_deg, float tilt_deg)
{
    clean_reason_t why = prv_check_interlocks();
    if (why != CLEAN_REASON_OK) {
        s_last_reason = why;
        ESP_LOGW(TAG, "cycle refused: %s", cleaning_cycle_reason_str(why));
        return why;
    }

    s_target_pan = pan_deg;
    s_target_tilt = tilt_deg;

    /* Move the nozzle to the target before the water starts. A spray from the
     * wrong place is worse than no spray. */
    servo_driver_set_angle(SERVO_PAN, pan_deg);
    servo_driver_set_angle(SERVO_TILT, tilt_deg);

    s_state = CLEAN_STATE_AIMING;
    s_spray_stop_us = esp_timer_get_time() + (uint64_t)s_spray_ms * 1000u;
    s_last_reason = CLEAN_REASON_OK;

    ESP_LOGI(TAG, "cycle start: pan=%.1f tilt=%.1f, spray %u ms",
             (double)pan_deg, (double)tilt_deg, (unsigned)s_spray_ms);
    return CLEAN_REASON_OK;
}

clean_reason_t cleaning_cycle_manual_pump(uint32_t duration_ms)
{
    /* A manual test is an explicit operator act, so the automatic-enable
     * switch does not apply. The WATER checks still do: asking for water is
     * not a reason to run the pump dry. */
    if (duration_ms == 0) duration_ms = s_spray_ms;
    if (duration_ms > 10000u) duration_ms = 10000u;   /* hard cap */

    water_reading_t w = water_sensor_read();
    if (!w.valid) {
        s_last_reason = CLEAN_REASON_SENSOR_INVALID;
        ESP_LOGW(TAG, "manual pump refused: sensor invalid");
        return CLEAN_REASON_SENSOR_INVALID;
    }
    if (w.float_used && !w.float_ok) {
        s_last_reason = CLEAN_REASON_LOW_WATER;
        ESP_LOGW(TAG, "manual pump refused: low water");
        return CLEAN_REASON_LOW_WATER;
    }
    if (w.pct < s_water_min_pct) {
        s_last_reason = CLEAN_REASON_LOW_WATER;
        ESP_LOGW(TAG, "manual pump refused: low water (%.0f%% < %.0f%%)",
                 (double)w.pct, (double)s_water_min_pct);
        return CLEAN_REASON_LOW_WATER;
    }
    if (s_state == CLEAN_STATE_SPRAYING || s_manual_stop_us != 0) {
        s_last_reason = CLEAN_REASON_BUSY;
        return CLEAN_REASON_BUSY;
    }

    s_manual_stop_us = esp_timer_get_time() + (uint64_t)duration_ms * 1000u;
    s_last_reason = CLEAN_REASON_OK;
    ESP_LOGI(TAG, "manual pump: %u ms", (unsigned)duration_ms);
    return CLEAN_REASON_OK;
}

void cleaning_cycle_stop(const char *why)
{
    if (s_state == CLEAN_STATE_SPRAYING) {
        prv_relay(false);
        ESP_LOGW(TAG, "spray CUT (%s)", why ? why : "stopped");
    }
    if (s_manual_stop_us != 0) {
        prv_relay(false);
        s_manual_stop_us = 0;
        ESP_LOGW(TAG, "manual pump CUT (%s)", why ? why : "stopped");
    }
    s_state = CLEAN_STATE_IDLE;
    s_spray_stop_us = 0;
    s_cooldown_until_us = esp_timer_get_time() + (uint64_t)s_cooldown_ms * 1000u;
}

clean_state_t cleaning_cycle_state(void) { return s_state; }
bool cleaning_cycle_is_busy(void) { return s_state != CLEAN_STATE_IDLE; }
float cleaning_cycle_target_pan(void) { return s_target_pan; }
float cleaning_cycle_target_tilt(void) { return s_target_tilt; }
uint32_t cleaning_cycle_spray_count(void) { return s_spray_count; }
clean_reason_t cleaning_cycle_last_reason_code(void) { return s_last_reason; }

uint32_t cleaning_cycle_cooldown_remaining_ms(void)
{
    if (s_state != CLEAN_STATE_IDLE) return 0;
    uint64_t now = esp_timer_get_time();
    if (now >= s_cooldown_until_us) return 0;
    return (uint32_t)((s_cooldown_until_us - now) / 1000u);
}

/* The cycle task. Runs every 50 ms: it starts the spray after the aim, ends
 * it, and -- importantly -- keeps checking the water while the spray runs. */
void cleaning_cycle_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = esp_timer_get_time();

        /* Manual pump window. */
        if (s_manual_stop_us != 0 && now >= s_manual_stop_us) {
            prv_relay(false);
            s_manual_stop_us = 0;
            ESP_LOGI(TAG, "manual pump OFF");
        }

        switch (s_state) {
        case CLEAN_STATE_IDLE:
            break;

        case CLEAN_STATE_AIMING:
            /* The servos have been commanded; give them a moment to get
             * there before the water starts. */
            if (now >= s_spray_stop_us - (uint64_t)s_spray_ms * 1000u +
                        400000u) {
                /* One last water check right before the pump opens. */
                water_reading_t w = water_sensor_read();
                if (!w.valid || (w.float_used && !w.float_ok) ||
                    w.pct < s_water_min_pct) {
                    ESP_LOGW(TAG, "water changed before spray; refusing");
                    s_state = CLEAN_STATE_IDLE;
                    s_cooldown_until_us = now + (uint64_t)s_cooldown_ms * 1000u;
                    s_last_reason = w.valid ? CLEAN_REASON_LOW_WATER
                                            : CLEAN_REASON_SENSOR_INVALID;
                    break;
                }
                prv_relay(true);
                s_state = CLEAN_STATE_SPRAYING;
                s_spray_count++;
                ESP_LOGI(TAG, "spray ON");
            }
            break;

        case CLEAN_STATE_SPRAYING:
            /* Re-check the water DURING the spray. A tank that empties
             * mid-spray must stop the pump, not run it dry. */
            {
                water_reading_t w = water_sensor_read();
                if (!w.valid || (w.float_used && !w.float_ok) ||
                    w.pct < s_water_min_pct) {
                    ESP_LOGE(TAG, "water lost during spray; pump OFF");
                    prv_relay(false);
                    s_state = CLEAN_STATE_IDLE;
                    s_cooldown_until_us = now + (uint64_t)s_cooldown_ms * 1000u;
                    s_last_reason = w.valid ? CLEAN_REASON_LOW_WATER
                                            : CLEAN_REASON_SENSOR_INVALID;
                    break;
                }
            }
            if (now >= s_spray_stop_us) {
                prv_relay(false);
                s_state = CLEAN_STATE_COOLDOWN;
                s_cooldown_until_us = now + (uint64_t)s_cooldown_ms * 1000u;
                ESP_LOGI(TAG, "spray OFF");
            }
            break;

        case CLEAN_STATE_COOLDOWN:
            if (now >= s_cooldown_until_us) s_state = CLEAN_STATE_IDLE;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
