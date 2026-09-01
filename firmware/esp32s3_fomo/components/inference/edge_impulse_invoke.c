/*
 * edge_impulse_invoke.c — runs the trained Edge Impulse FOMO model.
 *
 * Replace the stub below with your exported runtime in 3 steps:
 *
 *   1. Edge Impulse project -> Deployment -> "ESP32 / ESP32-S3 C++ library"
 *      (or "Arduino library") download. Unzip it under:
 *          components/inference/edge_impulse/
 *   2. Make sure the export's include paths are visible. If you place the
 *      exported tree at edge_impulse/ with its own CMakeLists, add it to
 *      components/inference/CMakeLists.txt (idf_component_add includes).
 *   3. Uncomment/replace the body of edge_impulse_run_model() with the
 *      standard Edge Impulse SDK invocation:
 *
 *        ei_impulse_result_t result;
 *        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
 *        // pick max score, feed centroids into fomo_result_t
 *
 * The stub below keeps the firmware buildable and bootable before training.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "inference_service.h"

#define EI_INPUT_W 96
#define EI_INPUT_H 96

#define EI_INTERVAL_MS 0.0f
#define EI_DNN_INPUT_SCALE 0.003921568627451f  /* 1/255 */

int edge_impulse_input_width(void)  { return EI_INPUT_W; }
int edge_impulse_input_height(void) { return EI_INPUT_H; }

/* Pixel-level feature provider that a real model needs. For the stub we
 * ignore input RGB and just accumulate a "score" so the pipeline runs. */
static float prv_stub_feature(uint8_t r, uint8_t g, uint8_t b)
{
    return (0.299f * r + 0.587f * g + 0.114f * b);
}

/* Stub model: emits a weak "object present" signal so /status + spray logic
 * can be exercised without a trained model. Replace with the real FOMO. */
static bool prv_run_stub(const uint8_t *input_rgb888, int w, int h,
                         fomo_result_t *out, uint32_t *elapsed_ms)
{
    uint32_t t0 = esp_timer_get_time();

    /* Central brightness as a stand-in "novelty" score in [0,1]. */
    float sum = 0.0f;
    int n = w * h;
    for (int i = 0; i < n; i += 7) {
        const uint8_t *p = input_rgb888 + (size_t)i * 3;
        sum += prv_stub_feature(p[0], p[1], p[2]);
    }
    float mean = sum / (float)((n + 6) / 7);
    float score = mean / 255.0f;

    out->score = score;
    out->object_present = score >= 0.8f;
    out->x = 0.5f;
    out->y = 0.5f;
    out->width = 0.2f;
    out->height = 0.2f;

    *elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000u);
    return true;
}

/*
 * Public entry point. Routes to the stub until the real Edge Impulse SDK
 * is vendored in. See the top-of-file instructions.
 */
bool edge_impulse_run_model(const uint8_t *input_rgb888, int w, int h,
                            fomo_result_t *out, uint32_t *elapsed_ms)
{
    if (input_rgb888 == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

#if 0 /* ---- replace `#if 0` with `#if 1` once you have the export ---- */
    // Example with the Edge Impulse C++ SDK (source included under
    // edge_impulse/). The variable `signal` wraps input_rgb888 as the
    // model's expected input tensor.
    ei_impulse_result_t result = { 0 };
    signal_t signal;
    int16_t buf[EI_INPUT_W * EI_INPUT_H * EI_CLASSIFIER_CHANNELS];
    // ... fill buf from input_rgb888 using EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
    // ... then run_classifier(&signal, &result, false);
    // ... map result.classification / bounding boxes into *out
    *elapsed_ms = 0;
    return true;
#else
    return prv_run_stub(input_rgb888, w, h, out, elapsed_ms);
#endif
}
