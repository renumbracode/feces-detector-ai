/*
 * Water level: an analog reading for the gauge, plus an optional float switch
 * that overrides it.
 *
 * The float switch is the stronger signal and wins when it is fitted: it
 * cannot drift and it cannot read falsely, which an analog float sensor in
 * conductive water absolutely can.
 *
 * The ADC uses the IDF 5.x oneshot driver (components/esp_adc). The legacy
 * driver/adc.h no longer exists in IDF 5.5.
 */
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "water_sensor.h"

static const char *TAG = "water";

#define ADC_UNIT       ADC_UNIT_1
#define ADC_CHANNEL    ADC_CHANNEL_0
#define ADC_ATTEN      ADC_ATTEN_DB_12

/* A raw reading outside this range means the sensor is disconnected or
 * shorted, not that the tank is empty or overflowing. */
#define RAW_MIN_VALID  100
#define RAW_MAX_VALID  4000

/* With 12-bit attenuation the raw reading spans roughly 0..4095 over the
 * ADC's input range. */
#define RAW_FULL_SCALE 4095

/* The float switch is wired to GND when the water is present (the usual
 * "pulled up by the input, pulled low by the water" arrangement), so a LOW
 * reading means water. */
#define FLOAT_WATER_PRESENT_LEVEL  0

static int s_analog_pin = -1;
static int s_float_pin = -1;
static bool s_use_float = false;
static adc_oneshot_unit_handle_t s_unit = NULL;

static bool prv_read_float_ok(void)
{
    if (!s_use_float || s_float_pin < 0) return true;
    int level = gpio_get_level(s_float_pin);
    return (level == FLOAT_WATER_PRESENT_LEVEL);
}

esp_err_t water_sensor_init(int analog_pin, int float_pin, bool use_float)
{
    s_analog_pin = analog_pin;
    s_float_pin = float_pin;
    s_use_float = use_float;

    if (analog_pin >= 0) {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = ADC_UNIT,
        };
        esp_err_t e = adc_oneshot_new_unit(&unit_cfg, &s_unit);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(e));
            return e;
        }
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        e = adc_oneshot_config_channel(s_unit, ADC_CHANNEL, &chan_cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s",
                     esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(TAG, "water level on ADC%d channel %d (GPIO%d), float switch %s",
                 (int)ADC_UNIT + 1, (int)ADC_CHANNEL, analog_pin,
                 use_float ? "enabled" : "not fitted");
    } else {
        ESP_LOGW(TAG, "no analog water sensor configured");
    }

    if (use_float && float_pin >= 0) {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << float_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&gc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "float switch gpio_config failed: %s",
                     esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

uint32_t water_sensor_raw(void)
{
    if (s_unit == NULL) return 0;
    int raw = 0;
    if (adc_oneshot_read(s_unit, ADC_CHANNEL, &raw) != ESP_OK) return 0;
    return (uint32_t)raw;
}

water_reading_t water_sensor_read(void)
{
    water_reading_t r = { 0 };

    r.float_used = s_use_float;
    r.float_ok = prv_read_float_ok();

    uint32_t raw = water_sensor_raw();
    r.raw = raw;

    if (s_analog_pin < 0) {
        /* No analog sensor: the float switch alone decides. */
        r.valid = s_use_float;
        r.pct = r.float_ok ? 100.0f : 0.0f;
        return r;
    }

    if (raw < RAW_MIN_VALID || raw > RAW_MAX_VALID) {
        /* Disconnected or shorted. Fail closed: the caller must treat this
         * as "not enough water". */
        ESP_LOGW(TAG, "water reading out of range (raw=%u); treating as invalid",
                 (unsigned)raw);
        r.valid = false;
        r.pct = 0.0f;
        return r;
    }

    float pct = (float)raw * 100.0f / (float)RAW_FULL_SCALE;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    r.pct = pct;
    r.valid = true;
    return r;
}
