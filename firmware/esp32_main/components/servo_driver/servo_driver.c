/*
 * Two hobby servos on LEDC at 50 Hz.
 *
 * A servo is driven by the WIDTH of a pulse, repeated every 20 ms. 1500 us
 * is the centre of a typical hobby servo, 500 us one end and 2400 us the
 * other. The exact numbers vary by model, which is why they are configuration
 * rather than constants.
 */
#include <math.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "servo_driver.h"

static const char *TAG = "servo";

#define SERVO_FREQ_HZ 50
#define LEDC_RES      LEDC_TIMER_14_BIT
#define LEDC_MAX_DUTY  (1 << 14)

static const ledc_channel_t s_channel[SERVO_COUNT] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1,
};
static int s_pin[SERVO_COUNT] = { -1, -1 };

static servo_limits_t s_limits[SERVO_COUNT];
static float s_angle[SERVO_COUNT] = { 0.0f, 0.0f };

static float prv_clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint32_t servo_driver_angle_to_pulse(servo_id_t id, float angle_deg)
{
    if (id >= SERVO_COUNT) return 0;
    const servo_limits_t *l = &s_limits[id];
    float a = prv_clamp(angle_deg, l->angle_min_deg, l->angle_max_deg);
    float span = l->angle_max_deg - l->angle_min_deg;
    if (span <= 0.0f) return l->pulse_min_us;
    float frac = (a - l->angle_min_deg) / span;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (uint32_t)lroundf(l->pulse_min_us +
                             frac * (float)(l->pulse_max_us - l->pulse_min_us));
}

esp_err_t servo_driver_init(int pan_pin, int tilt_pin,
                             const servo_limits_t *pan,
                             const servo_limits_t *tilt)
{
    s_pin[SERVO_PAN] = pan_pin;
    s_pin[SERVO_TILT] = tilt_pin;
    s_limits[SERVO_PAN] = *pan;
    s_limits[SERVO_TILT] = *tilt;

    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&tc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(e));
        return e;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (s_pin[i] < 0) continue;
        ledc_channel_config_t cc = {
            .gpio_num = s_pin[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = s_channel[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        e = ledc_channel_config(&cc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ledc_channel_config(%d) failed: %s",
                     s_pin[i], esp_err_to_name(e));
            return e;
        }
        /* Park at the minimum angle so the arm does not snap to an undefined
         * position at boot. */
        s_angle[i] = s_limits[i].angle_min_deg;
        servo_driver_set_pulse_us((servo_id_t)i, s_limits[i].pulse_min_us);
    }

    ESP_LOGI(TAG, "servos ready: pan pin=%d (%u..%u us), tilt pin=%d (%u..%u us)",
             s_pin[SERVO_PAN],
             (unsigned)s_limits[SERVO_PAN].pulse_min_us,
             (unsigned)s_limits[SERVO_PAN].pulse_max_us,
             s_pin[SERVO_TILT],
             (unsigned)s_limits[SERVO_TILT].pulse_min_us,
             (unsigned)s_limits[SERVO_TILT].pulse_max_us);
    return ESP_OK;
}

esp_err_t servo_driver_set_pulse_us(servo_id_t id, uint32_t pulse_us)
{
    if (id >= SERVO_COUNT || s_pin[id] < 0) return ESP_ERR_INVALID_ARG;
    if (pulse_us > 20000u) pulse_us = 20000u;

    /* duty is the fraction of the 20 ms period the pulse is high. */
    uint32_t duty = (uint32_t)((uint64_t)pulse_us * LEDC_MAX_DUTY / 20000u);
    esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE, s_channel[id], duty);
    if (e != ESP_OK) return e;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, s_channel[id]);
}

esp_err_t servo_driver_set_angle(servo_id_t id, float angle_deg)
{
    if (id >= SERVO_COUNT || s_pin[id] < 0) return ESP_ERR_INVALID_ARG;
    float a = prv_clamp(angle_deg, s_limits[id].angle_min_deg,
                        s_limits[id].angle_max_deg);
    s_angle[id] = a;
    uint32_t pulse = servo_driver_angle_to_pulse(id, a);
    ESP_LOGI(TAG, "%s -> %.1f deg (%u us)",
             id == SERVO_PAN ? "pan" : "tilt", (double)a, (unsigned)pulse);
    return servo_driver_set_pulse_us(id, pulse);
}

float servo_driver_angle(servo_id_t id)
{
    if (id >= SERVO_COUNT) return 0.0f;
    return s_angle[id];
}

esp_err_t servo_driver_park(servo_id_t id)
{
    if (id >= SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    return servo_driver_set_angle(id, s_limits[id].angle_min_deg);
}
