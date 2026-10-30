#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Publishing detected feces coordinates to an external actuator board
 * ("main ESP32") so it can aim a nozzle and spray.
 *
 * This is strictly ADDITIVE to the camera board. The board's own relay, LED,
 * buzzer and local spray path are untouched: the camera board keeps working
 * standalone, which is what makes it reusable for something else later.
 *
 * The wire contract is docs/target-protocol.md. Keep the two in step.
 *
 * What this component deliberately does NOT do:
 *   - decide whether a spray should happen (that is the main board's call);
 *   - compute servo angles (the main board owns the calibration);
 *   - touch the relay.
 */

/* The camera board can hold up to 8 FOMO boxes in one frame
 * (FOMO_MAX_FECES_BOXES); the verifier supplies up to 4. Accept any count. */
#define TARGET_MAX_BOXES 8

/* A box in the wire format: TOP-LEFT corner, normalized 0..1, not a centroid.
 * The FOMO result stores a centroid, so the caller converts before calling. */
typedef struct {
    float x, y, w, h;
    float conf;
} target_box_t;

/* What the main board last said. Surfaced in /status and on the dashboard so
 * "sent but refused" is distinguishable from "never arrived". */
typedef struct {
    bool     enabled;
    char     url[129];
    bool     last_ok;            /* the last POST completed */
    bool     last_accepted;      /* the main board accepted the target */
    char     last_reason[24];    /* refusal reason, "" when accepted */
    char     last_source[8];     /* which model produced the boxes */
    float    last_pan, last_tilt; /* servo angles the main board computed */
    bool     last_angles;        /* the board reported angles at all -- one
                                  * that aims internally sends none, and the
                                  * dashboard must not print "pan 0.0" for a
                                  * spray that really happened */
    uint32_t last_detection_id;
    uint64_t last_ok_age_ms;     /* ms since the last completed POST */
    uint32_t failures;           /* consecutive failed POSTs */
    uint32_t published;          /* messages handed to the publisher */
    uint32_t dropped;            /* messages dropped because one was pending */
} target_publisher_status_t;

/* Start the publisher task. Safe to call once; a second call is ignored.
 * The URL and the enabled flag are supplied separately and can be changed at
 * runtime without restarting the task. */
esp_err_t target_publisher_init(void);

/* Point the publisher at an actuator board, or turn publishing off.
 * url == NULL or url[0] == '\0' disables publishing regardless of `enabled`. */
void target_publisher_set_config(const char *url, bool enabled);

/* Publish one detection. Returns immediately: the message is handed to a
 * dedicated task so a slow or dead actuator board can never stall the
 * detection loop (which is already paced by ~700 ms of on-device inference
 * and a verifier poll).
 *
 * `source` is "fomo" or "yolo" and is passed through verbatim.
 * `detection_id` must increase per published detection; it is the main
 * board's duplicate guard. */
void target_publisher_publish(const target_box_t *boxes, uint8_t count,
                              uint16_t frame_w, uint16_t frame_h,
                              const char *source, uint32_t detection_id);

void target_publisher_status(target_publisher_status_t *out);
