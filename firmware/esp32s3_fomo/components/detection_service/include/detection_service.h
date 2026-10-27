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
#define VERIFY_MAX_FECES_BOXES 4

typedef struct {
    float x, y, w, h; /* normalized box, 0..1 */
} verify_box_t;

typedef struct {
    bool     valid;      /* a verify round-trip has completed at least once */
    bool     detected;   /* verifier reported a target-class box */
    float    conf;       /* best target-class confidence */
    float    x, y, w, h; /* normalized box of the best target box, 0..1 */
    /* All target-class boxes above the server's confidence floor (score
     * descending, capped at VERIFY_MAX_FECES_BOXES). x,y,w,h above mirror
     * feces_boxes[0] when any exist. */
    verify_box_t feces_boxes[VERIFY_MAX_FECES_BOXES];
    uint8_t feces_count; /* instance count reported by the verifier */
    uint32_t age_ms;     /* ms since this result was produced */
} verify_result_t;

void detection_service_verify_result(verify_result_t *out);

/* Number of feces detections this boot has acted on: frames the device
 * classified as feces and published to the dashboard (throttled to every 5th
 * frame). Counts device-side detections, so it keeps climbing even when the
 * dashboard is unreachable. Surfaced as "detections" in /status. */
uint32_t detection_service_detection_count(void);

/* ---------- verifier (server-side YOLOv8) health ----------
 * Exposed because the live page's "VERIFY OFFLINE" used to be indistinguishable
 * between "the server is down" and "the board stopped trying", which sends the
 * diagnosis after the wrong component entirely. A poll leg that stalls silently
 * is otherwise invisible: /status keeps reporting the last accepted answer and
 * its age climbs, but nothing says whether attempts are still being made. */
bool detection_service_verify_ok(void);
/* Milliseconds since a verifier answer was last accepted. UINT64_MAX if never. */
uint64_t detection_service_last_verify_ok_age_ms(void);
/* Consecutive failed verify polls since the last accepted answer. */
uint32_t detection_service_verify_failures(void);
