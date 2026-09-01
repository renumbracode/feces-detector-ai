#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
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
static float s_fomo_confidence = 0.8f;

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

/* Decode a JPEG frame to RGB888 using the esp32-camera img2rgb. */
static uint8_t *prv_decode_jpeg(const uint8_t *jpeg, size_t len, int *w, int *h)
{
    if (jpeg == NULL || len == 0) return NULL;
    size_t decoded = 0;
    uint8_t *rgb = NULL;
    bool ok = fmt2rgb888(jpeg, len, PIXFORMAT_JPEG, &rgb, &decoded);
    if (!ok || rgb == NULL) {
        ESP_LOGW(TAG, "JPEG decode failed");
        return NULL;
    }
    /* Infer dimensions from JPEG header. */
    *w = 0; *h = 0;
    if (len > 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
        const uint8_t *p = jpeg + 2;
        while (p + 9 < jpeg + len) {
            if (p[0] != 0xFF) { p++; continue; }
            unsigned marker = p[1];
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                *h = (p[5] << 8) | p[6];
                *w = (p[7] << 8) | p[8];
                break;
            }
            if (marker == 0xDA) break;
            int seglen = (p[2] << 8) | p[3];
            p += 2 + seglen;
        }
    }
    if (*w == 0 || *h == 0) {
        free(rgb);
        return NULL;
    }
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

    /* FOMO input: 96x96 RGB888 (common Edge Impulse FOMO size). */
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
    ESP_LOGI(TAG, "Inference service ready (FOMO, conf>=%.2f triggers)", s_fomo_confidence);
    return ESP_OK;
}
