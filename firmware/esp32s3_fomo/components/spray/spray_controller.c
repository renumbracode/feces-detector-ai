#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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

/* Alert-only state: LED + buzzer without the relay, used when a cooldown is
 * holding the pump back so a detection is never silently dropped. */
static bool s_alerting = false;
static uint64_t s_alert_stop_us = 0;

/* How long the alert-only LED/buzzer pulse runs. Long enough to be noticed,
 * short enough not to overlap the next inference. */
#define ALERT_MS 3000u

/* Relay IN polarity. Most single-channel modules energise the coil when IN is
 * pulled LOW (active-low, fail-safe). Only the relay is polarity-sensitive:
 * the LED and buzzer are driven active-high regardless. */
static bool s_relay_active_low = true;

/* Operator kill switch for automatic sprays (NVS-backed via app_config; the
 * boot path applies the stored value). Manual force_spray() is never gated --
 * the switch must not take water control away from an explicit operator act. */
static bool s_enabled = true;

/* The GPIO level that leaves the relay de-energised, derived from polarity.
 * Used at init and after a polarity change so the pump is never left on. */
static uint32_t prv_relay_off_level(void)
{
    return s_relay_active_low ? 1u : 0u;
}

static uint32_t prv_relay_on_level(void)
{
    return s_relay_active_low ? 0u : 1u;
}

/* Drive an ACTIVE buzzer with a steady level, not a tone.
 *
 * This was previously 2 kHz PWM at 50% duty, chosen to cover both active and
 * passive parts. That was wrong for the fitted buzzer: an active buzzer has its
 * own oscillator and expects continuous DC, and a duty-cycled 3.3 V pin gave it
 * roughly 1.65 V average with constant edge transitions -- too quiet to hear
 * through the series resistor, so it never sounded despite the firmware
 * correctly driving the pin.
 *
 * An active buzzer is therefore driven exactly like the LED and the relay: a
 * plain GPIO level. That also removes the last reason the buzzer could not
 * share a pin with the relay (two outputs on one pad still fight, so the
 * collision guard below stays), and frees LEDC TIMER_1/CHANNEL_1 -- the camera
 * keeps TIMER_0/CHANNEL_0 and is untouched.
 *
 * Tradeoff: this suits an ACTIVE buzzer only. A passive buzzer needs a tone and
 * will only click silently; LEDC would have to come back for that part, so
 * check which type is fitted before swapping it. */
static void prv_buzzer(bool on)
{
    if (s_buzzer_pin < 0) return;
    gpio_set_level(s_buzzer_pin, on ? 1 : 0);
}

/* Relay IN is the only polarity-aware line. The LED and the active buzzer are
 * plain active-high loads on their own dedicated pins (see APP_PIN_* in
 * app_config.h -- none of them may share a pin, or two outputs fight for it). */
static void prv_led(bool on)
{
    if (s_led_pin >= 0) gpio_set_level(s_led_pin, on ? 1 : 0);
}

static void prv_relay(bool on)
{
    if (s_relay_pin >= 0)
        gpio_set_level(s_relay_pin, on ? prv_relay_on_level()
                                        : prv_relay_off_level());
    prv_led(on);
    prv_buzzer(on);
}

static void prv_turn_on(uint32_t duration_ms)
{
    s_spraying = true;
    s_cooldown_active = true;
    s_spray_stop_us = esp_timer_get_time() + (uint64_t)duration_ms * 1000u;
    prv_relay(true);
    ESP_LOGI(TAG, "Spray ON (%u ms) relay=%d level=%u (%s) led=%d buzzer=%d",
             (unsigned)duration_ms, s_relay_pin, (unsigned)prv_relay_on_level(),
             s_relay_active_low ? "active-low" : "active-high",
             s_led_pin, s_buzzer_pin);
}

/* LED + buzzer without touching the relay. Used when a cooldown is holding the
 * pump back: the operator must still be told a detection happened, otherwise a
 * detection during cooldown is completely silent and looks like a miss. The
 * relay stays de-energised, so this can never pump water. */
static void prv_alert_on(uint32_t duration_ms)
{
    if (s_alerting) return;
    s_alerting = true;
    s_alert_stop_us = esp_timer_get_time() + (uint64_t)duration_ms * 1000u;
    prv_led(true);
    prv_buzzer(true);
    ESP_LOGI(TAG, "Detection during cooldown: alert only (led=%d buzzer=%d, "
                  "relay held off for %u ms more)",
             s_led_pin, s_buzzer_pin, (unsigned)spray_controller_cooldown_remaining_ms());
}

/* True while the pump is being held back after a previous spray. Derived from
 * elapsed time rather than a latched flag, so it clears on its own once the
 * cooldown expires -- a latched flag kept reporting cooldownActive:true long
 * after the window had passed, which reads as "stuck". */
bool spray_controller_cooldown_active(void)
{
    if (s_spraying) return true;
    if (!s_cooldown_active) return false;
    if (s_cooldown_ms == 0) return false;
    return (esp_timer_get_time() - s_last_spray_end_us) <
           (uint64_t)s_cooldown_ms * 1000u;
}

uint32_t spray_controller_cooldown_remaining_ms(void)
{
    if (!spray_controller_cooldown_active() || s_cooldown_ms == 0) return 0;
    uint64_t elapsed_ms = (esp_timer_get_time() - s_last_spray_end_us) / 1000u;
    if (elapsed_ms >= s_cooldown_ms) return 0;
    return (uint32_t)(s_cooldown_ms - elapsed_ms);
}

static void spray_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = esp_timer_get_time();

        if (s_spraying && now >= s_spray_stop_us) {
            s_spraying = false;
            s_last_spray_end_us = now;
            prv_relay(false);
            ESP_LOGI(TAG, "Spray OFF");
        }

        /* Alert-only path (cooldown holding the pump). Stop it without ever
         * touching the relay, which stays at its inactive level. */
        if (s_alerting && now >= s_alert_stop_us && !s_spraying) {
            s_alerting = false;
            prv_led(false);
            prv_buzzer(false);
            ESP_LOGI(TAG, "Alert OFF");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void spray_controller_init(int relay_pin, int led_pin, int buzzer_pin,
                           float threshold,
                           uint32_t cooldown_ms, uint32_t spray_ms,
                           bool relay_active_low)
{
    s_relay_pin = relay_pin;
    s_led_pin = led_pin;
    s_buzzer_pin = buzzer_pin;
    s_threshold = threshold;
    s_cooldown_ms = cooldown_ms;
    s_spray_ms = spray_ms;
    s_relay_active_low = relay_active_low;

    /* Each load needs its own pin. Two GPIO outputs on one pad drive the line
     * against each other, so a shared pin gives a relay that never latches
     * cleanly while /status and the serial log both report "Spray ON". Fail
     * loudly rather than silently half-working. */
    if (s_relay_pin >= 0 && s_relay_pin == s_buzzer_pin) {
        ESP_LOGE(TAG, "relay pin %d == buzzer pin %d; the relay will never "
                      "hold. Move the buzzer to a free pin.", s_relay_pin);
    }
    if (s_led_pin >= 0 && (s_led_pin == s_buzzer_pin || s_led_pin == s_relay_pin)) {
        ESP_LOGE(TAG, "LED pin %d collides with relay/buzzer; it will not "
                      "be independently controllable.", s_led_pin);
    }

    if (s_relay_pin >= 0) {
        gpio_reset_pin(s_relay_pin);
        gpio_set_direction(s_relay_pin, GPIO_MODE_OUTPUT);
        /* Drive the *inactive* level, not a hardcoded 0. On an active-low
         * module a hardcoded 0 would energise the coil at boot -- the pump
         * would run from reset until the first detection report. */
        gpio_set_level(s_relay_pin, prv_relay_off_level());
    }
    if (s_led_pin >= 0) {
        gpio_reset_pin(s_led_pin);
        gpio_set_direction(s_led_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(s_led_pin, 0);
    }

    /* ACTIVE buzzer: a plain GPIO level, exactly like the LED and the relay.
     * No LEDC and no tone -- see prv_buzzer() for why, and for the active-only
     * limitation. The direction call is mandatory here: a pad left as an input
     * has its output driver disabled, so it would never reach the buzzer while
     * /status and the serial log happily reported buzzer=21. The log records the
     * command, not the electrical result, so silence there is never proof the
     * pin was driven. */
    if (s_buzzer_pin >= 0) {
        gpio_reset_pin(s_buzzer_pin);
        gpio_set_direction(s_buzzer_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(s_buzzer_pin, 0);   /* start from a known-silent state */
    }

    xTaskCreatePinnedToCore(spray_task, "spray", 2048, NULL, 5, NULL, 0);
}

/* Forward decls: prv_alert_on reports the remaining cooldown, which is derived
 * from time rather than a latched flag. */
static void prv_alert_on(uint32_t duration_ms);
bool spray_controller_cooldown_active(void);
uint32_t spray_controller_cooldown_remaining_ms(void);

bool spray_controller_trigger_external(float confidence, float min_confidence,
                                       uint32_t duration_ms)
{
    /* Applies the CALLER's confidence gate, not s_threshold. The server-side
     * YOLOv8 score is on a different scale from the on-device FOMO score, so
     * comparing it against the FOMO dial would be meaningless. */
    if (confidence < min_confidence) return false;

    /* Kill switch: same alert-only refusal as the FOMO path. Without this the
     * server leg would keep spraying while the operator believes the pump is
     * off -- the switch gates BOTH detector paths, not just the local one. */
    if (!s_enabled) {
        prv_alert_on(ALERT_MS);
        return false;
    }

    if (s_spraying) return false;

    /* Shares the cooldown with the FOMO path on purpose: both models looking at
     * the same pen must not produce two sprays for one detection episode. */
    if (spray_controller_cooldown_active()) {
        prv_alert_on(ALERT_MS);
        return false;
    }

    s_cooldown_active = false;
    prv_turn_on(duration_ms ? duration_ms : s_spray_ms);
    return true;
}

bool spray_controller_on_detection(float confidence)
{
    s_last_confidence = confidence;

    /* Below the confidence dial this is not an actionable detection. */
    if (confidence < s_threshold) return false;

    /* Kill switch: an actionable detection still ALERTS (LED + buzzer) and is
     * still logged upstream -- it simply never energises the relay. This mirrors
     * the cooldown path: the pump is rate-limited/disabled, the operator is
     * still told about the feces. */
    if (!s_enabled) {
        prv_alert_on(ALERT_MS);
        return false;
    }

    /* A spray is already running: that running spray *is* this detection's
     * response, and the LED and buzzer are already lit. Re-triggering here
     * would only restart the timer and keep the relay closed indefinitely. */
    if (s_spraying) return false;

    /* Cooldown only gates the relay. With the default of 0 it never engages,
     * so every detection above the dial sprays. When an operator deliberately
     * sets a gap (to limit water use), the pump is held back but the LED and
     * buzzer still fire so the detection is never silently lost. */
    if (spray_controller_cooldown_active()) {
        prv_alert_on(ALERT_MS);
        return false;
    }

    s_cooldown_active = false;
    prv_turn_on(s_spray_ms);
    return true;
}

bool spray_controller_force_spray(uint32_t duration_ms)
{
    /* Manual override by an explicit operator act: NOT gated by s_enabled.
     * The kill switch refuses automatic sprays, not requested ones. */
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
    /* Derived from elapsed time, so it clears itself when the window expires
     * instead of latching on until the next detection happened to clear it. */
    out->cooldown_active = spray_controller_cooldown_active();
    out->cooldown_until_ms =
        out->cooldown_active
            ? (s_last_spray_end_us + (uint64_t)s_cooldown_ms * 1000u) / 1000u
            : 0;
}

void spray_controller_set_config(float threshold, uint32_t cooldown_ms, uint32_t spray_ms)
{
    s_threshold = threshold;
    s_cooldown_ms = cooldown_ms;
    s_spray_ms = spray_ms;
}

void spray_controller_set_relay_polarity(bool active_low)
{
    if (active_low == s_relay_active_low) return;
    s_relay_active_low = active_low;
    /* Re-assert the off state under the NEW mapping. Writing the old level
     * would be wrong under the new one: for an active-low -> active-high flip
     * that is a 1, which is exactly the energised level. */
    if (s_relay_pin >= 0) gpio_set_level(s_relay_pin, prv_relay_off_level());
    ESP_LOGI(TAG, "relay IN polarity: %s", s_relay_active_low ? "active-low" : "active-high");
}

void spray_controller_set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;

    if (!on) {
        ESP_LOGW(TAG, "spray DISABLED: automatic sprays refused (manual override still works)");
        /* Kill switch means water stops NOW: a spray already running is cut
         * mid-cycle exactly the way spray_task ends one normally. */
        if (s_spraying) {
            s_spraying = false;
            s_spray_stop_us = 0;
            s_last_spray_end_us = esp_timer_get_time();
            prv_relay(false);
            ESP_LOGW(TAG, "in-progress spray CUT by the switch; cooldown window started");
        }
    } else {
        ESP_LOGI(TAG, "spray ENABLED: automatic sprays resume");
    }
}

bool spray_controller_is_enabled(void)
{
    return s_enabled;
}
