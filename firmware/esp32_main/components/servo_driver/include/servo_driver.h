#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Two hobby servos (pan, tilt) driven by LEDC at 50 Hz.
 *
 * The angle->pulse mapping is deliberately not "pulse = angle * 11.1". It goes
 * through the servo's own pulse range so a servo with a different travel can
 * be used by changing two numbers, and so the mechanical limits and the
 * electrical limits are separate things: the solver clamps to the MECHANICAL
 * limits (what the arm can do), and the driver maps the result into the
 * ELECTRICAL range (what the servo accepts).
 */

typedef enum {
    SERVO_PAN = 0,
    SERVO_TILT = 1,
    SERVO_COUNT = 2,
} servo_id_t;

typedef struct {
    uint32_t pulse_min_us;   /* pulse for the minimum angle */
    uint32_t pulse_max_us;   /* pulse for the maximum angle */
    float    angle_min_deg;  /* mechanical limit */
    float    angle_max_deg;
} servo_limits_t;

/* Set up LEDC and park both servos at their minimum angle. The servos are
 * driven to a known position at boot so they do not jump to an undefined
 * angle when the board powers up. */
esp_err_t servo_driver_init(int pan_pin, int tilt_pin,
                             const servo_limits_t *pan,
                             const servo_limits_t *tilt);

/* Move one servo to an angle, clamped to its mechanical limits. */
esp_err_t servo_driver_set_angle(servo_id_t id, float angle_deg);

/* Move one servo to a pulse width directly, in microseconds. This is the
 * escape hatch for a servo whose angles are not what the calibration
 * assumes. */
esp_err_t servo_driver_set_pulse_us(servo_id_t id, uint32_t pulse_us);

/* Where the servo is commanded to be right now, in degrees. */
float servo_driver_angle(servo_id_t id);

/* Park a servo at its minimum angle. Used on shutdown and by the manual
 * "centre" test. */
esp_err_t servo_driver_park(servo_id_t id);

/* Map an angle to a pulse width without moving anything. Exposed so the
 * /status endpoint can report the commanded pulse, which is what a person
 * with a multimeter needs when a servo is not reaching its mark. */
uint32_t servo_driver_angle_to_pulse(servo_id_t id, float angle_deg);
