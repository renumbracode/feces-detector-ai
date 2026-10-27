#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The cleaning cycle: aim, spray, wait.
 *
 * This component owns the RELAY. Nothing else may energise it. That is the
 * whole point of the split: the camera board detects and publishes, and this
 * board decides whether water actually moves.
 *
 * The interlocks are checked in a fixed order and the FIRST failure is the
 * reason reported back, so the operator is told the real problem rather than
 * the last one checked. The order matters: water is checked before "busy", so
 * a dry tank is reported as low_water rather than as busy.
 */

typedef enum {
    CLEAN_REASON_OK = 0,
    CLEAN_REASON_DISABLED,        /* operator switch off */
    CLEAN_REASON_LOW_WATER,       /* level below the minimum */
    CLEAN_REASON_SENSOR_INVALID,  /* reading unusable -> fail closed */
    CLEAN_REASON_BUSY,            /* a cycle is already running */
    CLEAN_REASON_NO_TARGET,       /* nothing to aim at */
    CLEAN_REASON_SOLVE_FAILED,    /* the coordinates did not solve to an angle */
} clean_reason_t;

typedef enum {
    CLEAN_STATE_IDLE = 0,
    CLEAN_STATE_AIMING,
    CLEAN_STATE_SPRAYING,
    CLEAN_STATE_COOLDOWN,
} clean_state_t;

const char *cleaning_cycle_reason_str(clean_reason_t r);

/* Set up the relay pin and the water threshold. */
esp_err_t cleaning_cycle_init(int relay_pin, bool relay_active_low,
                              float water_min_pct, uint32_t spray_ms,
                              uint32_t cooldown_ms);

/* Apply changed settings without restarting the board. */
void cleaning_cycle_set_config(bool enabled, float water_min_pct,
                                uint32_t spray_ms, uint32_t cooldown_ms,
                                bool relay_active_low);

/* Start a cleaning cycle at the given angles.
 *
 * Returns CLEAN_REASON_OK when the cycle was accepted and started. Any other
 * value means it was REFUSED and nothing moved and no water ran.
 *
 * The water level is checked again DURING the spray, so a tank that empties
 * mid-spray stops the pump rather than running it dry. */
clean_reason_t cleaning_cycle_start(float pan_deg, float tilt_deg);

/* Manual pump test. This is an explicit operator act, so it is NOT gated by
 * the automatic-enable switch -- but it IS gated by the water checks, because
 * "the operator asked for water" is not a reason to run the pump dry. */
clean_reason_t cleaning_cycle_manual_pump(uint32_t duration_ms);

/* Stop any running cycle immediately. Used by the enable switch. */
void cleaning_cycle_stop(const char *why);

clean_state_t cleaning_cycle_state(void);
bool cleaning_cycle_is_busy(void);
float cleaning_cycle_target_pan(void);
float cleaning_cycle_target_tilt(void);
uint32_t cleaning_cycle_cooldown_remaining_ms(void);
uint32_t cleaning_cycle_spray_count(void);
clean_reason_t cleaning_cycle_last_reason_code(void);

/* The cycle task. Declared here so the task can be created before it is
 * defined; it is not meant to be called by anything else. */
void cleaning_cycle_task(void *arg);
