#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Continuous detection loop: captures frames on a runner, runs FOMO
 * inference, triggers the relay on detection, and reports events to the
 * CodeIgniter dashboard + Python YOLOv8 verifier. Runs on a dedicated core. */
esp_err_t detection_service_start(void);

/* Latest YOLOv8 verification of the live camera frame, polled on a fixed
 * cadence so the dashboard overlay has a real detection even while the
 * on-device FOMO model is still a pre-export stub. Normalized 0..1 box. */
typedef struct {
    bool     valid;      /* a verify round-trip has completed at least once */
    bool     detected;   /* verifier reported a target-class box */
    float    conf;       /* best target-class confidence */
    float    x, y, w, h; /* normalized box, 0..1 */
    uint32_t age_ms;     /* ms since this result was produced */
} verify_result_t;

void detection_service_verify_result(verify_result_t *out);

/* Number of feces detections this boot has acted on: frames the device
 * classified as feces and published to the dashboard (throttled to every 5th
 * frame). Counts device-side detections, so it keeps climbing even when the
 * dashboard is unreachable. Surfaced as "detections" in /status. */
uint32_t detection_service_detection_count(void);
