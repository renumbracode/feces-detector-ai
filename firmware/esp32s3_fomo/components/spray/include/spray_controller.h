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

/* Init GPIO, apply config. Base is main-facing. */
void spray_controller_init(int relay_pin, int led_pin, float threshold,
                           uint32_t cooldown_ms, uint32_t spray_ms);

/* Attempt an auto spray based on a detection confidence. Returns true if triggered. */
bool spray_controller_on_detection(float confidence);

/* Force a spray regardless of cooldown (manual override). Returns true if started. */
bool spray_controller_force_spray(uint32_t duration_ms);

/* Is a spray currently active? */
bool spray_controller_is_spraying(void);

/* Copy current status for the /status endpoint. */
void spray_controller_status(spray_status_t *out);

/* Configure threshold / cooldown / spray duration at runtime. */
void spray_controller_set_config(float threshold, uint32_t cooldown_ms, uint32_t spray_ms);
