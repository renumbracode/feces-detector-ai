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
#define CAM_FRAMESIZE    FRAMESIZE_HD   /* 1280x720: light MJPEG, enough for AI input */

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
        .jpeg_quality = 12,
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
    s->set_quality(s, 12);

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
