#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"

#include "camera_service.h"

static const char *TAG = "camera_svc";

/* ---- OV3660 (and OV2640/OV5640) pin mapping for ESP32-S3 camera boards ----
 * Many ESP32-S3 CAM dev boards use these GPIO lines. Adjust for your board. */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

#define CAM_XCLK_FREQ_HZ 20000000
/* 640x480, and this must stay compatible with the 1/4 JPEG decode scale in
 * inference_service.c (prv_decode_jpeg). Those two are coupled:
 *
 *    640x480 --esp_jpeg 1/4--> 160x120 --resize--> 160x160 tensor  (1.33x vertical)
 *   1280x720 --esp_jpeg 1/4--> 320x180 --resize--> 160x160 tensor  (0.5x downscale)
 *    640x480 --esp_jpeg 1/8-->  80x60 --resize--> 160x160 tensor  (2x UPSCALE, wrong)
 *
 * This used to be FRAMESIZE_HD (1280x720) at a 1/8 decode, giving 160x90 --
 * 14,400 source pixels for a 160x160 model input. The earlier comment here
 * warned against dropping to VGA, on the grounds that VGA plus the 1/8 scale
 * would yield 80x60 and a 2x upscale. That warning was correct about the
 * coupling but was read as "never drop the resolution", and the two were never
 * changed together. They are now: VGA with a 1/4 decode gives 160x120, i.e.
 * 19,200 source pixels -- MORE than the 160x90 it replaces, and with no
 * upscaling in x at all. Detection input gets better while the frame gets
 * ~2.5x smaller.
 *
 * The smaller frame is what makes the live view usable. At 1280x720 and
 * jpeg_quality 30 a frame measured 25,753 bytes (verified by parsing the SOF
 * marker of a live /stream capture, not by reading the comment that claimed
 * 10-15 KB). Against the 60 KB/s stream budget in http_server_service.c that
 * capped the live view at ~2.4 fps no matter how healthy the link was.
 *
 * KEEP THE DIVISOR AND THE DECODE ENUM IN STEP. prv_decode_jpeg derives
 * outbuf_size from the same constant it passes as out_scale; if they disagree
 * esp_jpeg_decode returns ESP_ERR_INVALID_ARG and classify() yields no
 * detection at all, with no error surfaced to /status. */
#define CAM_FRAMESIZE    FRAMESIZE_VGA   /* 640x480 */

/* jpeg_quality 0-63, and LOWER means higher quality on OV-series sensors.
 *
 * This was 12, which is near-lossless and produced ~48 KB frames at 1280x720.
 * On this board's link (RSSI -61..-76 dBm, ping ~180 ms, measured ceiling
 * ~6 KB/s) a 48 KB frame takes ~8 s to send, so the live view ran at 0.09 fps
 * and /status timed out behind the saturated socket.
 *
 * Unchanged at 30 through the 2026-10-13 VGA change: the frame size drop came
 * from resolution, not from quality, so image fidelity per pixel is unchanged
 * and there is no reason to trade more JPEG artifacts for bytes we no longer
 * need. Measured at 640x480: ~10 KB/frame.
 *
 * The earlier "roughly 10-15 KB" claim in this comment was written for
 * 1280x720 and was wrong by ~2x -- a live capture measured 25,753 bytes at
 * 1280x720 with this same quality of 30. That misestimate is what left the
 * live view pinned at ~2 fps, because the 60 KB/s budget was sized against a
 * frame size the sensor never produced. Resolution is now the frame-size
 * lever; do NOT go back toward quality 12.
 */
#define CAM_JPEG_QUALITY 30

/* What CAM_FRAMESIZE is supposed to produce. Only used to warn at boot when the
 * sensor negotiates something else, so a resolution surprise is visible in the
 * boot log instead of silently degrading the model input. */
#define EXPECTED_WIDTH  640
#define EXPECTED_HEIGHT 480

static volatile bool s_streaming = false;
static volatile int s_streaming_refs = 0;

esp_err_t camera_service_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,
        .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAM_FRAMESIZE,
        .jpeg_quality = CAM_JPEG_QUALITY,
        .fb_count = 6,              /* stream + detect consumers; 4 starved the MJPEG task */
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t e = esp_camera_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", e);
        return e;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "sensor_get failed");
        return ESP_FAIL;
    }
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_quality(s, CAM_JPEG_QUALITY);

    /* Report what the sensor ACTUALLY negotiated rather than what was asked
     * for. CAM_FRAMESIZE requests the size named at the top of this file, but
     * the requested framesize enum on its own cannot distinguish "clamped by the
     * driver" from "clamped by a different sensor" from "clamped by JPEG mode".
     * One live frame buffer settles all three.
     *
     * This matters because the frame size and the decode scale in
     * inference_service.c are coupled: if the sensor ever clamps below
     * FRAMESIZE_VGA the 1/4 decode stops yielding ~160 px wide and the model
     * input silently becomes an upscale. The warning below is the only signal
     * that this happened. */
    ESP_LOGI(TAG, "sensor PID=0x%04x VER=0x%04x, driver framesize enum=%u "
                  "(requested %u)",
             (unsigned)s->id.PID, (unsigned)s->id.VER,
             (unsigned)s->status.framesize, (unsigned)cfg.frame_size);

    camera_fb_t *probe = esp_camera_fb_get();
    if (probe) {
        ESP_LOGI(TAG, "first frame %ux%u, %u bytes (format %u)",
                 (unsigned)probe->width, (unsigned)probe->height,
                 (unsigned)probe->len, (unsigned)probe->format);
        if (probe->width != EXPECTED_WIDTH || probe->height != EXPECTED_HEIGHT) {
            ESP_LOGW(TAG, "sensor delivered %ux%u but CAM_FRAMESIZE implies %ux%u",
                     (unsigned)probe->width, (unsigned)probe->height,
                     (unsigned)EXPECTED_WIDTH, (unsigned)EXPECTED_HEIGHT);
        }
        esp_camera_fb_return(probe);
    } else {
        ESP_LOGE(TAG, "no frame available right after init");
    }

    ESP_LOGI(TAG, "Camera ready (OV3660 class sensor, JPEG)");
    return ESP_OK;
}

camera_fb_t *camera_service_get_frame(void)
{
    return esp_camera_fb_get();
}

void camera_service_release_frame(camera_fb_t *fb)
{
    if (fb) esp_camera_fb_return(fb);
}

/* Refcounted: /stream runs on its own task, so a second viewer (or a browser
 * reconnect where the old socket closes after the new one opens) must not cut
 * the camera feed for the others. */
void camera_streaming_start(void)
{
    s_streaming_refs++;
    s_streaming = true;
}

void camera_streaming_stop(void)
{
    if (s_streaming_refs > 0) s_streaming_refs--;
    s_streaming = (s_streaming_refs > 0);
}
