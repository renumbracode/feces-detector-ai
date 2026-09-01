/*
 * Continuous on-device detection.
 *
 * Runs a dedicated FreeRTOS task that:
 *   1. grabs a camera frame,
 *   2. runs the Edge Impulse FOMO inference,
 *   3. updates the relay/spray state,
 *   4. reports detections to the CodeIgniter dashboard and pushes the JPEG
 *      to the Python YOLOv8 verification server for cross-checking.
 *
 * "Whenever technically feasible" is honoured here: the FOMO model is small
 * enough to run fully on the ESP32-S3; YOLOv8 runs on the server because it
 * is not feasible on the MCU.
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_camera.h"

#include "app_config.h"
#include "camera_service.h"
#include "inference_service.h"
#include "spray_controller.h"
#include "detection_service.h"

static const char *TAG = "detect";

static void prv_post_dashboard(const app_config_t *cfg, float conf,
                               bool triggered, uint32_t spray_ms)
{
    char url[512];
    snprintf(url, sizeof(url),
        "%s?confidence=%.4f&triggered=%d&spray_ms=%u&esp=esp32s3&model=fomo:v1&notes=%s",
        cfg->dash_url, (double)conf, triggered ? 1 : 0, (unsigned)spray_ms,
        triggered ? "auto+spray" : "cooldown+detected");

    esp_http_client_config_t cc = {
        .url = url,
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (c) {
        esp_err_t e = esp_http_client_perform(c);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "Dashboard log failed: %s", esp_err_to_name(e));
        }
        esp_http_client_cleanup(c);
    }
}

static void prv_post_verify(const app_config_t *cfg, const uint8_t *jpeg,
                            size_t len, float fomo_conf)
{
    if (jpeg == NULL || len == 0) return;

    esp_http_client_config_t cc = {
        .url = cfg->verify_url,
        .timeout_ms = 15000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (!c) return;

    char fomo_hdr[16];
    snprintf(fomo_hdr, sizeof(fomo_hdr), "%.4f", (double)fomo_conf);

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "X-Fomo-Conf", fomo_hdr);
    esp_http_client_set_post_field(c, (const char *)jpeg, len);

    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "YOLOv8 verify POST failed: %s", esp_err_to_name(e));
    }
    esp_http_client_cleanup(c);
}

static void detection_task(void *arg)
{
    (void)arg;
    uint32_t frame_counter = 0;

    for (;;) {
        camera_fb_t *fb = camera_service_get_frame();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fomo_result_t res = inference_service_classify(fb->buf, fb->len);
        bool triggered = spray_controller_on_detection(res.score);

        if (res.object_present && (frame_counter % 5 == 0)) {
            app_config_t cfg;
            app_config_load(&cfg);
            prv_post_dashboard(&cfg, res.score, triggered, cfg.spray_ms);
            /* Push every Nth detected frame to YOLOv8 verifier. */
            prv_post_verify(&cfg, fb->buf, fb->len, res.score);
        }

        camera_service_release_frame(fb);
        frame_counter++;
        vTaskDelay(pdMS_TO_TICKS(100)); /* ~10 fps detection cadence */
    }
}

esp_err_t detection_service_start(void)
{
    xTaskCreatePinnedToCore(detection_task, "detect", 8192, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Detection loop started on core 1");
    return ESP_OK;
}
