#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* Multi-instance support: one frame can hold several feces spots. The FOMO
 * heat map is scanned as a whole, and every cell above the presentation floor
 * becomes one entry, so the count reflects the pen state rather than a single
 * "worst spot" summary. */
#define FOMO_MAX_FECES_BOXES 8

/* One normalized detection box (centroid + size, 0..1). */
typedef struct {
    float x;                 /* normalized centroid x (0..1) */
    float y;                 /* normalized centroid y (0..1) */
    float width;             /* normalized estimated width (0..1) */
    float height;            /* normalized estimated height (0..1) */
    float score;             /* class score (0..1) */
} fomo_box_t;

/* Result of one FOMO inference pass. */
typedef struct {
    bool object_present;     /* FOMO "feces" score above configured threshold */
    float score;             /* maximum class score (0..1) */
    int class_id;            /* label of the winning box: 0 = feces (target),
                                1 = pig (suppressor). -1 when nothing detected. */
    float x;                 /* normalized centroid x of largest blob (0..1) */
    float y;                 /* normalized centroid y of largest blob (0..1) */
    float width;             /* normalized estimated width (0..1) */
    float height;            /* normalized estimated height (0..1) */
    uint32_t inference_ms;   /* time taken for this inference */
    /* All feces boxes above the presentation floor, sorted by score
     * descending, capped at FOMO_MAX_FECES_BOXES. x..height above mirror
     * feces_boxes[0] when the winning class is feces, kept for legacy
     * consumers. */
    fomo_box_t feces_boxes[FOMO_MAX_FECES_BOXES];
    uint8_t feces_count;     /* number of entries in feces_boxes (0..max) */
} fomo_result_t;

/* Class ids the trained model emits. Only the feces class ever triggers a
 * spray or a publish; the pig class is context that stops false positives. */
#define EI_CLASS_FECES 0
#define EI_CLASS_PIG   1

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

/* Fixed FOMO box-presentation floor. The runtime recall<->precision knob is
 * the separate spray dial ("threshold") in the web config / NVS. */
float edge_impulse_present_threshold(void);

/* Identifier string reported in /status + dashboard rows, telling what is
 * actually running on-device: fomo:stub / fomo:1class / fomo:2class. */
const char *edge_impulse_model_tag(void);
