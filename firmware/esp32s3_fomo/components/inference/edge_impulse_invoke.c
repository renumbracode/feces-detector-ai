/*
 * edge_impulse_invoke.c — runs the trained Edge Impulse FOMO model.
 *
 * Wiring for the exported Edge Impulse library (see edge_impulse/README.md):
 *
 *   1. Edge Impulse project -> Deployment -> "ESP32 / ESP32-S3 C++ library"
 *      download. Unzip it under:  components/inference/edge_impulse/
 *   2. The export ships its own CMakeLists.txt with a component target; the
 *      top-level project CMakeLists adds it via EXTRA_COMPONENT_DIRS when the
 *      folder exists. This file then sees the SDK headers automatically.
 *   3. This translation unit compiles in the real run_classifier() path when
 *      <edge_impulse_capsrd.h> is detected; otherwise the luma-brightness
 *      stub keeps the firmware buildable and bootable pre-export.
 *
 * Class semantics (2-class export):
 *   EI_CLASS_FECES = 0 -> the target; drives spray + publish + green box.
 *   EI_CLASS_PIG   = 1 -> context; a high pig score alone must never trigger.
 *   A 1-class export (fecies only) is treated as class 0 throughout.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "inference_service.h"

/* Input geometry. A real export defines these in model_parameters.h; the
 * fallbacks keep the stub build working without the SDK. 160x160 is the
 * intended Edge Impulse model size: FOMO emits a 1/8-scale heat map (20x20),
 * and the 10th-percentile feces blob in our dataset is ~5px at 96x96 but ~8.8px
 * at 160x160. Below ~one heat-map cell an object is effectively unsupervised. */
#define EI_INPUT_W_FALLBACK 160
#define EI_INPUT_H_FALLBACK 160

/* Minimum FOMO cell score for a box to be reported as a detection. FOMO
 * scores are per-cell and typically want ~0.4-0.6, not 0.8 like an
 * object-detection score. */
#define EI_FOMO_PRESENT_THRESHOLD 0.45f

#if defined(__has_include)
#  if __has_include("edge_impulse_capsrd.h")
#    define EI_SDK_PRESENT 1
#  endif
#endif
#ifndef EI_SDK_PRESENT
#  define EI_SDK_PRESENT 0
#endif

#if EI_SDK_PRESENT
#include "edge-impulse-sdk/classifier/ei_run_classifier.h" /* run_classifier */
#include "edge-impulse-sdk/classifier/ei_classifier_types.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"                   /* signal_from_buffer */
#include "model-parameters/model_metadata.h"
#endif

static const char *TAG = "ei_invoke";

int edge_impulse_input_width(void)
{
#if EI_SDK_PRESENT
    return EI_CLASSIFIER_INPUT_WIDTH;
#else
    return EI_INPUT_W_FALLBACK;
#endif
}

int edge_impulse_input_height(void)
{
#if EI_SDK_PRESENT
    return EI_CLASSIFIER_INPUT_HEIGHT;
#else
    return EI_INPUT_H_FALLBACK;
#endif
}

/* Honest model id for /status + dashboard logging so the panel is never
 * misled about what is actually running on the device. */
const char *edge_impulse_model_tag(void)
{
#if EI_SDK_PRESENT
#  if EI_CLASSIFIER_NUMBER_OF_CLASSES == 2
    return "fomo:2class";
#  else
    return "fomo:1class";
#  endif
#else
    return "fomo:stub";
#endif
}

/* ------------------------------------------------------------------ stub */
static float prv_stub_feature(uint8_t r, uint8_t g, uint8_t b)
{
    return (0.299f * r + 0.587f * g + 0.114f * b);
}

/* Pre-export stub: central brightness as a stand-in "score". Always reports
 * class 0 (feces) so the full pipeline — box overlay, spray gate, publish —
 * can be exercised. A real model replaces this at compile time. */
static bool prv_run_stub(const uint8_t *input_rgb888, int w, int h,
                         fomo_result_t *out, uint32_t *elapsed_ms)
{
    uint32_t t0 = esp_timer_get_time();

    float sum = 0.0f;
    int n = w * h;
    for (int i = 0; i < n; i += 7) {
        const uint8_t *p = input_rgb888 + (size_t)i * 3;
        sum += prv_stub_feature(p[0], p[1], p[2]);
    }
    float mean = sum / (float)((n + 6) / 7);
    float score = mean / 255.0f;

    out->score = score;
    out->class_id = (score >= 0.8f) ? EI_CLASS_FECES : -1;
    out->object_present = out->class_id == EI_CLASS_FECES;
    out->x = 0.5f;
    out->y = 0.5f;
    out->width = 0.2f;
    out->height = 0.2f;

    *elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000u);
    return true;
}

#if EI_SDK_PRESENT

/* RGB888 -> int8 input tensor exactly as the export's DSP block expects.
 * Edge Impulse int8 images are centred on 0 with a 1/255 or 1/127.5 scale;
 * the int8 tensor from the standard mobile-net preprocessing is px-128. */
static void prv_fill_tensor(const uint8_t *rgb888, int w, int h,
                            int16_t *buf)
{
    const int ch = EI_CLASSIFIER_INPUT_CHANNELS;
    for (int i = 0; i < w * h; i++) {
        const int16_t r = rgb888[i * 3 + 0];
        const int16_t g = rgb888[i * 3 + 1];
        const int16_t b = rgb888[i * 3 + 2];
        if (ch == 1) {
            buf[i] = (int16_t)((EI_DNN_INPUT_SCALE * (0.299f * r + 0.587f * g + 0.114f * b)));
        } else {
            buf[i * 3 + 0] = (int16_t)(r - 128);
            buf[i * 3 + 1] = (int16_t)(g - 128);
            buf[i * 3 + 2] = (int16_t)(b - 128);
        }
    }
}

static bool prv_run_classifier(const uint8_t *input_rgb888, int w, int h,
                               fomo_result_t *out, uint32_t *elapsed_ms)
{
    uint32_t t0 = esp_timer_get_time();

    const int iw = EI_CLASSIFIER_INPUT_WIDTH;
    const int ih = EI_CLASSIFIER_INPUT_HEIGHT;
    const int ch = EI_CLASSIFIER_INPUT_CHANNELS;

    /* The caller feeds exactly the model's input geometry, but an odd sensor
     * aspect could slip a mismatched size through. Guard before touching the
     * stack/heap so a bad resize cannot silently corrupt class scores. */
    if (w != iw || h != ih) {
        ESP_LOGW(TAG, "input %dx%d != model %dx%d", w, h, iw, ih);
        return false;
    }

    int16_t *buf = malloc((size_t)iw * ih * ch * sizeof(int16_t));
    if (buf == NULL) return false;

    prv_fill_tensor(input_rgb888, w, h, buf);

    signal_t signal;
    int err = numpy::signal_from_buffer(buf, iw * ih * ch, &signal);
    if (err != 0) {
        free(buf);
        return false;
    }

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    free(buf);

    if (r != EI_IMPULSE_OK) {
        ESP_LOGW(TAG, "run_classifier returned %d", (int)r);
        return false;
    }

    /* Find the highest-scoring box and pick the class that goes with it. */
    out->score = 0.0f;
    out->class_id = -1;
    for (size_t i = 0; i < EI_CLASSIFIER_OBJECT_DETECTION_COUNT; ++i) {
        const ei_impulse_result_bounding_box_t *bb = &result.bounding_boxes[i];
        if (bb->value < EI_FOMO_PRESENT_THRESHOLD || bb->value <= out->score) {
            continue;
        }
        out->score = bb->value;
        out->class_id = bb->label_id;
        /* Edge Impulse boxes are (x, y, w, h) of the bounding box in
         * normalized 0..1 coords. fomo_result_t stores the centroid + size. */
        out->x = bb->x + bb->width / 2.0f;
        out->y = bb->y + bb->height / 2.0f;
        out->width = bb->width;
        out->height = bb->height;
    }

    out->object_present = out->class_id == EI_CLASS_FECES;
    *elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000u);
    return true;
}

#endif /* EI_SDK_PRESENT */

bool edge_impulse_run_model(const uint8_t *input_rgb888, int w, int h,
                            fomo_result_t *out, uint32_t *elapsed_ms)
{
    if (input_rgb888 == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

#if EI_SDK_PRESENT
    return prv_run_classifier(input_rgb888, w, h, out, elapsed_ms);
#else
    return prv_run_stub(input_rgb888, w, h, out, elapsed_ms);
#endif
}
