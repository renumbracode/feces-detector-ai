#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* Result of one FOMO inference pass. */
typedef struct {
    bool object_present;     /* FOMO "feces" score above configured threshold */
    float score;             /* maximum class score (0..1) */
    float x;                 /* normalized centroid x of largest blob (0..1) */
    float y;                 /* normalized centroid y (0..1) */
    float width;             /* normalized estimated width (0..1) */
    float height;            /* normalized estimated height (0..1) */
    uint32_t inference_ms;   /* time taken for this inference */
} fomo_result_t;

/* Init the inference subsystem: intercepts camera frames, runs the Edge
 * Impulse FOMO model in a dedicated core, exposes latest result. */
esp_err_t inference_service_init(void);

/* Inject a raw frame (JPEG from the camera service) for inference.
 * Called on the camera side. Returns the decoded run's result. */
fomo_result_t inference_service_classify(const uint8_t *jpeg, size_t len);

/* Copy the latest result (thread-safe snapshot). */
void inference_service_latest(fomo_result_t *out);

/* Model input geometry (from the exported Edge Impulse model, or defaults). */
int edge_impulse_input_width(void);
int edge_impulse_input_height(void);
