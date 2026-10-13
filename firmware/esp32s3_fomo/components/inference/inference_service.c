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
 * This file isolates the model invocation so that the runtime (TFLite Micro
 * / Edge Impulse SDK) touches only edge_impulse_invoke.cpp.
 *
 * The trained 2-class FOMO export is now present under edge_impulse/
 * (160x160 input, labels "feces" and "pig"), so EI_SDK_PRESENT is 1 and
 * edge_impulse_invoke.cpp compiles the real run_classifier() path. A
 * deterministic brightness stub is still compiled in as the fallback so the
 * firmware builds and boots without the export present; /status reports which
 * one is live via edge_impulse_model_tag() -- "fomo:2class" for the real model,
 * "fomo:stub" if the export ever goes missing.
 * ---------------------------------------------------------------------- */

static const char *TAG = "inference";

static SemaphoreHandle_t s_lock = NULL;
static fomo_result_t s_latest;

/* From edge_impulse_invoke.c */
extern bool edge_impulse_run_model(const uint8_t *input_rgb888, int w, int h,
                                   fomo_result_t *out, uint32_t *elapsed_ms);

/* Small nearest-neighbour resample of a JPEG-decoded RGB888 buffer, reading
 * only the source rectangle (sx0, sy0, cw, ch) of an sw x sh buffer.
 *
 * The source rect is what restores the pre-2026-10-13 framing; see the
 * geometry constants below for why a plain full-frame resize is wrong. */
static void prv_resize_rgb(const uint8_t *src, int sw, int sh,
                           int sx0, int sy0, int cw, int ch,
                           uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = sy0 + (y * ch) / dh;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = sx0 + (x * cw) / dw;
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = src + ((size_t)sy * sw + sx) * 3;
            uint8_t *q = dst + ((size_t)y * dw + x) * 3;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
        }
    }
}

/* ---- Framing geometry: why the model input is cropped, not full-frame -----
 *
 * The OV3660 does not merely rescale between FRAMESIZE_VGA and FRAMESIZE_HD --
 * the driver writes a DIFFERENT SENSOR WINDOW for each aspect ratio, taken
 * from ratio_table[] in esp32-camera/sensors/private_include/ov3660_settings.h:
 *
 *   4x3  (VGA 640x480):  x 0..2079, y 0..1547   -> 2080 x 1548  (full frame)
 *   16x9 (HD 1280x720):  x 64..2015, y 242..1333 -> 1952 x 1092
 *
 * The 16x9 window is therefore 1952/2080 = 93.8% of the width but only
 * 1092/1548 = 70.5% of the height: HD crops ~30% of the frame off the top and
 * bottom. Mapping each window onto the same 160x160 model input, an object
 * therefore appears
 *
 *     vertically   160/1548 = 0.1034  vs  160/1092 = 0.1465  -> 29.5% SMALLER
 *     horizontally 160/2080 = 0.0769  vs  160/1952 = 0.0820  ->  6.2% smaller
 *
 * in VGA than it did in HD. That is not a rounding detail. FOMO emits a 20x20
 * heat map, so one cell is 8 px at a 160x160 input, and the documented
 * 10th-percentile feces blob is ~8.8 px -- sitting just above a single cell.
 * At 70.5% scale that same blob is ~6.2 px, i.e. BELOW one cell, so the
 * smallest and most marginal feces are exactly what stops registering.
 *
 * So crop the 16:9 window back out of the decoded frame before resizing. This
 * reproduces the pre-VGA sensor-window-to-model mapping exactly while keeping
 * the ~7 KB frame and the ~720 ms inference. The alternative -- using the full
 * 160x120 -- shows ~42% more of the pen but shrinks every object by ~30%.
 *
 * Fractions are derived from the driver's window numbers above and applied to
 * whatever the decoder produced, rather than hardcoding 150x84, so this still
 * behaves if the sensor or framesize ever changes.
 */
#define SENSOR_WIN_4X3_W 2080
#define SENSOR_WIN_4X3_H 1548
#define SENSOR_WIN_16X9_W 1952
#define SENSOR_WIN_16X9_H 1092

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
 * decoder with a 1/4 scale. The FOMO input is tiny (160x160), so decoding the
 * full camera frame would be needlessly slow; scaling inside the decoder is
 * ~16x cheaper. A 640x480 frame at 1/4 gives 160x120, which
 * prv_resize_rgb() then resizes to whatever the model declares. Returns NULL
 * on failure, setting w and h to the scaled output width and height.
 *
 * JPEG_DECODE_DIVISOR and the out_scale enum below are ONE setting expressed
 * twice. outbuf_size is computed from the divisor while esp_jpeg actually
 * decodes at the enum, so if they ever disagree the call returns
 * ESP_ERR_INVALID_ARG, prv_decode_jpeg returns NULL, and every inference
 * reports class=-1 with no error anywhere in /status. Keep them together.
 *
 * This was 1/8 paired with a 1280x720 sensor, yielding 160x90 -- 14,400 source
 * pixels for a 160x160 input, and 25,753-byte frames that capped the live view
 * at ~2.4 fps. At 1/4 over VGA it yields 160x120: 19,200 source pixels, more
 * than before, with no upscaling in x, from a ~2.5x smaller frame. */
#define JPEG_DECODE_DIVISOR 4
static uint8_t *prv_decode_jpeg(const uint8_t *jpeg, size_t len, int *w, int *h)
{
    int sw = 0, sh = 0;
    if (!prv_jpeg_dims(jpeg, len, &sw, &sh) || sw == 0 || sh == 0) return NULL;

    int ow = sw / JPEG_DECODE_DIVISOR, oh = sh / JPEG_DECODE_DIVISOR;
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
        .out_scale = JPEG_IMAGE_SCALE_1_4,
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
    /* Crop to the 16:9 sensor window, centred, then resample to the model
     * input. See the geometry block above: without this the model sees a
     * ~30% smaller scene and the smallest feces blobs drop below one FOMO
     * heat-map cell. */
    int cw = (sw * SENSOR_WIN_16X9_W) / SENSOR_WIN_4X3_W;
    int ch = (sh * SENSOR_WIN_16X9_H) / SENSOR_WIN_4X3_H;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (cw > sw) cw = sw;
    if (ch > sh) ch = sh;
    int sx0 = (sw - cw) / 2;
    int sy0 = (sh - ch) / 2;

    uint8_t *small = malloc((size_t)IW * IH * 3);
    if (small == NULL) {
        free(rgb);
        return out;
    }
    prv_resize_rgb(rgb, sw, sh, sx0, sy0, cw, ch, small, IW, IH);
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
