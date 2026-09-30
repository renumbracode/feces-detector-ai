#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "esp_camera.h"

#include "inference_service.h"

/* ---------- Edge Impulse integration point ------------------------------
 * This file isolates the model invocation so that whichever runtime you
 * choose (TFLite Micro / Edge Impulse SDK) touches only
 * edge_impulse_invoke.c. To activate a trained model:
 *
 *   1. In Edge Impulse: Deploy -> Arduino/ESP-IDF library export.
 *   2. Unzip into components/inference/edge_impulse/ so that the
 *      exported sources (model_parameters.h, tflite-model source, etc.)
 *      are available to edge_impulse_invoke.c.
 *   3. Fill in the input/detail sizes in edge_impulse_invoke.c to match
 *      your exported model (EI_CLASSIFIER_INPUT_WIDTH/HEIGHT/etc.).
 *
 * Until a real model is placed there, a deterministic stub classifier is
 * compiled in so the firmware builds, boots, and serves /stream + /status.
 * ---------------------------------------------------------------------- */

static const char *TAG = "inference";

static SemaphoreHandle_t s_lock = NULL;
static fomo_result_t s_latest;

/* From edge_impulse_invoke.c */
extern bool edge_impulse_run_model(const uint8_t *input_rgb888, int w, int h,
                                   fomo_result_t *out, uint32_t *elapsed_ms);

/* Small nearest-neighbour downscale of a JPEG-decoded RGB888 buffer. */
static void prv_downscale_rgb(const uint8_t *src, int sw, int sh,
                              uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            const uint8_t *p = src + (sy * sw + sx) * 3;
            uint8_t *q = dst + (y * dw + x) * 3;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
        }
    }
}

/* Parse JPEG dimensions from the SOF marker so we know the source size
 * before esp_jpeg decodes (sensor frames are hardware-encoded JPEG). */
static bool prv_jpeg_dims(const uint8_t *jpeg, size_t len, int *w, int *h)
{
    *w = 0; *h = 0;
    if (len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
    const uint8_t *p = jpeg + 2, *end = jpeg + len;
    while (p + 9 < end) {
        if (p[0] != 0xFF) { p++; continue; }
        unsigned marker = p[1];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            *h = (p[5] << 8) | p[6];
            *w = (p[7] << 8) | p[8];
            return true;
        }
        if (marker == 0xDA) break;
        int seglen = (p[2] << 8) | p[3];
        if (seglen < 2) break;
        p += 2 + seglen;
    }
    return false;
}

/* Decode a JPEG to a small RGB888 buffer using the SIMD-optimised esp_jpeg
 * decoder with a 1/8 scale. The FOMO input is tiny (160x160), so decoding the
 * full camera frame (1280x720) would be needlessly slow; scaling inside the
 * decoder is ~64x cheaper. A 1280x720 frame at 1/8 gives 160x90, which
 * prv_downscale_rgb() then resizes to whatever the model declares. Returns NULL
 * on failure, setting w and h to the scaled output width and height. */
static uint8_t *prv_decode_jpeg(const uint8_t *jpeg, size_t len, int *w, int *h)
{
    int sw = 0, sh = 0;
    if (!prv_jpeg_dims(jpeg, len, &sw, &sh) || sw == 0 || sh == 0) return NULL;

    int ow = sw / 8, oh = sh / 8;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;

    uint8_t *rgb = malloc((size_t)ow * oh * 3);
    if (rgb == NULL) return NULL;

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)len,
        .outbuf = rgb,
        .outbuf_size = (uint32_t)ow * oh * 3,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_output_t out = { 0 };
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        free(rgb);
        return NULL;
    }

    *w = out.width;
    *h = out.height;
    return rgb;
}

fomo_result_t inference_service_classify(const uint8_t *jpeg, size_t len)
{
    fomo_result_t out;
    memset(&out, 0, sizeof(out));

    int sw = 0, sh = 0;
    uint8_t *rgb = prv_decode_jpeg(jpeg, len, &sw, &sh);
    if (rgb == NULL) {
        out.object_present = false;
        out.score = 0.0f;
        return out;
    }

    /* FOMO input: 160x160 RGB888 (the Edge Impulse model size; the exported
     * SDK overrides this via EI_CLASSIFIER_INPUT_WIDTH/HEIGHT). */
    const int IW = edge_impulse_input_width();
    const int IH = edge_impulse_input_height();
    uint8_t *small = malloc((size_t)IW * IH * 3);
    if (small == NULL) {
        free(rgb);
        return out;
    }
    prv_downscale_rgb(rgb, sw, sh, small, IW, IH);
    free(rgb);

    uint32_t ms = 0;
    bool ok = edge_impulse_run_model(small, IW, IH, &out, &ms);
    free(small);

    if (!ok) {
        out.object_present = false;
        return out;
    }
    out.inference_ms = ms;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_latest = out;
        xSemaphoreGive(s_lock);
    }
    return out;
}

void inference_service_latest(fomo_result_t *out)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_latest;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t inference_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_latest, 0, sizeof(s_latest));
    ESP_LOGI(TAG, "Inference service ready (FOMO, box floor>=%.2f; spray dial is the runtime trigger)",
             edge_impulse_present_threshold());
    return ESP_OK;
}
