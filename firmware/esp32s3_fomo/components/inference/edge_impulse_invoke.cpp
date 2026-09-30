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
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"

/* The EI SDK headers are C++; this file is compiled as C++. Only the five
 * functions below are consumed by C callers, so they get C linkage. inference
 * _service.h is a plain C header; the extern "C" mismatch header between the
 * C structs and the future C++ SDK includes. */
extern "C" {
#include "inference_service.h"
}

/* Input geometry. A real export defines these in model_parameters.h; the
 * fallbacks keep the stub build working without the SDK. 160x160 is the
 * intended Edge Impulse model size: FOMO emits a 1/8-scale heat map (20x20),
 * and the 10th-percentile feces blob in our dataset is ~5px at 96x96 but ~8.8px
 * at 160x160. Below ~one heat-map cell an object is effectively unsupervised. */
#define EI_INPUT_W_FALLBACK 160
#define EI_INPUT_H_FALLBACK 160

/* Minimum FOMO cell score for a box to be reported as a detection. This is a
 * fixed noise floor only, NOT the recall<->precision knob: anything that
 * clears 0.30 becomes a candidate, and the runtime spray dial (web /config
 * -> threshold, NVS-backed) makes the final spray decision. Keeping the floor
 * below the dial range is deliberate -- a floor above the dial would silently
 * drop detections the dial could otherwise accept. NOTE: the exported model's
 * own FOMO post-process (model_variables.h: ei_fill_result_fomo_i8_config_*
 * .threshold) already drops boxes below 0.5, so the effective presentation
 * floor is really 0.5 unless that generated constant is lowered and rebuilt. */
#define EI_FOMO_PRESENT_THRESHOLD 0.30f

#if defined(__has_include)
#  if __has_include("edge-impulse-sdk/classifier/ei_run_classifier.h")
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

extern "C" int edge_impulse_input_width(void)
{
#if EI_SDK_PRESENT
    return EI_CLASSIFIER_INPUT_WIDTH;
#else
    return EI_INPUT_W_FALLBACK;
#endif
}

extern "C" int edge_impulse_input_height(void)
{
#if EI_SDK_PRESENT
    return EI_CLASSIFIER_INPUT_HEIGHT;
#else
    return EI_INPUT_H_FALLBACK;
#endif
}

/* Fixed box-presentation floor (see EI_FOMO_PRESENT_THRESHOLD). Exposed so
 * the inference service can log the real gate instead of a stale constant. */
extern "C" float edge_impulse_present_threshold(void)
{
    return (float)EI_FOMO_PRESENT_THRESHOLD;
}

/* Honest model id for /status + dashboard logging so the panel is never
 * misled about what is actually running on the device. */
extern "C" const char *edge_impulse_model_tag(void)
{
#if EI_SDK_PRESENT
#  if EI_CLASSIFIER_LABEL_COUNT == 2
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

/* Pack each RGB888 pixel as 0x00RRGGBB into one float sample, exactly as the
 * export's Image DSP (extract_image_features) expects: it casts every signal
 * sample back to uint32 and splits r/g/b itself. 0x00RRGGBB is exact in float
 * (integers up to 2^24), so the round-trip is lossless. One sample per pixel,
 * at model resolution -- the caller (inference_service.c) has already resized
 * the frame to EI_CLASSIFIER_INPUT_WIDTH/HEIGHT. No luma or centre-on-zero
 * scaling here: this export's DSP is a plain image pass-through, and the model
 * owns its own normalisation (IMAGE_SCALING_NONE). */
static void prv_fill_tensor(const uint8_t *rgb888, int w, int h, float *buf)
{
    for (int i = 0; i < w * h; i++) {
        uint32_t p = (uint32_t)rgb888[i * 3 + 0] << 16
                   | (uint32_t)rgb888[i * 3 + 1] << 8
                   | (uint32_t)rgb888[i * 3 + 2];
        buf[i] = (float)p;
    }
}

static bool prv_run_classifier(const uint8_t *input_rgb888, int w, int h,
                               fomo_result_t *out, uint32_t *elapsed_ms)
{
    uint32_t t0 = esp_timer_get_time();

    const int iw = EI_CLASSIFIER_INPUT_WIDTH;
    const int ih = EI_CLASSIFIER_INPUT_HEIGHT;

    /* The caller feeds exactly the model's input geometry, but an odd sensor
     * aspect could slip a mismatched size through. Guard before touching the
     * stack/heap so a bad resize cannot silently corrupt class scores. */
    if (w != iw || h != ih) {
        ESP_LOGW(TAG, "input %dx%d != model %dx%d", w, h, iw, ih);
        return false;
    }

    float *buf = (float *)malloc((size_t)iw * ih * sizeof(float));
    if (buf == NULL) return false;

    prv_fill_tensor(input_rgb888, w, h, buf);

    signal_t signal;
    int err = numpy::signal_from_buffer(buf, iw * ih, &signal);
    if (err != 0) {
        free(buf);
        return false;
    }

ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    free(buf);

    if (r != EI_IMPULSE_OK) {
        ESP_LOGW(TAG, "run_classifier returned %d", (int)r);
    } else {
        ESP_LOGI(TAG, "classify: dsp=%dms infer=%dms post=%dms boxes=%u",
                 (int)result.timing.dsp, (int)result.timing.classification,
                 (int)result.timing.postprocessing,
                 (unsigned)result.bounding_boxes_count);
    }

    /* Prefer the highest-scoring FECES box. A co-present pig box must not
     * suppress the target class (the old code kept only the globally top box,
     * so a strong pig box could mask a weaker-but-real feces hit). Pig is kept
     * as the fallback so a pig-only frame still reports a non-target class id
     * instead of -1. Note: this SDK revision iters bounding boxes by LABEL
     * (class name string) rather than a numeric id. */
    int32_t best_feces = -1, best_pig = -1;
    for (size_t i = 0; i < result.bounding_boxes_count; ++i) {
        const ei_impulse_result_bounding_box_t *bb = &result.bounding_boxes[i];
        if (bb->value < EI_FOMO_PRESENT_THRESHOLD) {
            continue;
        }
        if (strcmp(bb->label, "feces") == 0) {
            if (best_feces < 0 || bb->value > result.bounding_boxes[best_feces].value) {
                best_feces = (int32_t)i;
            }
        } else if (strcmp(bb->label, "pig") == 0) {
            if (best_pig < 0 || bb->value > result.bounding_boxes[best_pig].value) {
                best_pig = (int32_t)i;
            }
        }
    }

    out->score = 0.0f;
    out->class_id = -1;
    int32_t pick = (best_feces >= 0) ? best_feces : best_pig;
    if (pick >= 0) {
        const ei_impulse_result_bounding_box_t *bb = &result.bounding_boxes[pick];
        out->score = bb->value;
        out->class_id = (strcmp(bb->label, "feces") == 0) ? EI_CLASS_FECES : EI_CLASS_PIG;
        /* This SDK reports boxes in pixel coords of the model input; convert
         * to the normalized 0..1 centroid+size that fomo_result_t carries. */
        out->x = (bb->x + bb->width / 2.0f) / (float)iw;
        out->y = (bb->y + bb->height / 2.0f) / (float)ih;
        out->width = (float)bb->width / (float)iw;
        out->height = (float)bb->height / (float)ih;
    }

    out->object_present = out->class_id == EI_CLASS_FECES;
    *elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000u);
    return true;
}

#endif /* EI_SDK_PRESENT */

extern "C" bool edge_impulse_run_model(const uint8_t *input_rgb888, int w, int h,
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
