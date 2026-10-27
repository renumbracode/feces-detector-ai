#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Water level, read two ways.
 *
 * The ANALOG reading is the level: a float switch only says "there is some
 * water", and the dashboard wants a number it can show as a gauge.
 *
 * The FLOAT SWITCH is an optional independent interlock: a float that drops
 * when the tank empties, wired to a GPIO. When fitted and enabled it
 * OVERRIDES the analog reading -- a float switch cannot drift, cannot read
 * falsely, and cannot be fooled by a shorted sensor. That makes it the
 * stronger safety signal, so it wins.
 *
 * The failure posture is fail-CLOSED: an invalid or missing reading is
 * treated as "not enough water", never as "fine". A board that cannot read
 * its tank must not spray.
 */

typedef struct {
    float    pct;        /* 0..100 */
    bool     valid;      /* false when the reading cannot be trusted */
    bool     float_ok;   /* float switch state: true = water present */
    bool     float_used; /* the float switch is fitted and in use */
    uint32_t raw;        /* raw ADC value, for diagnosing a drifting sensor */
} water_reading_t;

/* Set up the ADC and, optionally, the float switch input. */
esp_err_t water_sensor_init(int analog_pin, int float_pin, bool use_float);

/* Read the level. Never fails: an unusable reading comes back with
 * valid == false and pct == 0. */
water_reading_t water_sensor_read(void);

/* The raw ADC value without the scaling, for calibration (fill the tank, read
 * the raw value, put that in /config as the "full" calibration). */
uint32_t water_sensor_raw(void);
