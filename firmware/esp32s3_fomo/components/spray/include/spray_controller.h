#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Relay / water pump controller with detection-driven and manual triggering. */
typedef struct {
    bool spraying;
    bool cooldown_active;
    float last_confidence;      /* last FOMO/YOLOv8 score sent to us */
    uint64_t last_spray_ago_ms; /* ms since the last spray ended */
    uint64_t cooldown_until_ms; /* absolute time (esp_timer us /1000) when cooldown ends */
} spray_status_t;

/* Init GPIO + buzzer, apply config. Base is main-facing.
 * buzzer_pin drives an ACTIVE buzzer as a plain GPIO level (a steady DC level,
 * not a tone -- a passive buzzer would need LEDC and would only click).
 * buzzer_pin MUST NOT equal relay_pin or led_pin: two outputs on one pad fight
 * each other. Pass relay_active_low to match the relay module (most KY-019 /
 * SRD-05VDC boards pull the coil LOW to fire). Only the relay is
 * polarity-sensitive; the LED and buzzer are always active-high. */
void spray_controller_init(int relay_pin, int led_pin, int buzzer_pin,
                           float threshold,
                           uint32_t cooldown_ms, uint32_t spray_ms,
                           bool relay_active_low);

/* Attempt an auto spray based on a detection confidence. Returns true if triggered. */
bool spray_controller_on_detection(float confidence);

/* Force a spray regardless of cooldown (manual override). Returns true if started. */
bool spray_controller_force_spray(uint32_t duration_ms);

/* Trigger a spray from a detector OTHER than the on-device FOMO class -- in
 * practice the server-side YOLOv8 verifier, via detection_service.c.
 *
 * `min_confidence` is applied instead of the FOMO threshold, because the two
 * models are not calibrated against each other and one shared dial would
 * either spray on FOMO noise or discard good YOLO hits. `duration_ms` of 0
 * means "use the configured spray_ms".
 *
 * Still honours an in-progress spray and the shared cooldown, so the two
 * detection sources cannot stack two sprays on one detection episode. When the
 * cooldown is what suppressed it, the LED and buzzer still alert: a detection
 * must never be silently dropped just because the pump is rate-limited.
 * Returns true if a spray actually started. */
bool spray_controller_trigger_external(float confidence, float min_confidence,
                                       uint32_t duration_ms);

/* Is a spray currently active? */
bool spray_controller_is_spraying(void);

/* Is the pump currently being held back by a cooldown? Derived from elapsed
 * time since the last spray, so it clears itself when the window expires. */
bool spray_controller_cooldown_active(void);

/* Milliseconds left on the cooldown, 0 when none is running. */
uint32_t spray_controller_cooldown_remaining_ms(void);

/* Copy current status for the /status endpoint. */
void spray_controller_status(spray_status_t *out);

/* Configure threshold / cooldown / spray duration at runtime. */
void spray_controller_set_config(float threshold, uint32_t cooldown_ms, uint32_t spray_ms);

/* Re-map relay IN polarity at runtime. active_low=true for the common modules
 * that energise the coil when IN is pulled LOW. Takes effect immediately and
 * leaves the relay in whichever state matches the new polarity, so the pump is
 * never left energised by the change. */
void spray_controller_set_relay_polarity(bool active_low);
