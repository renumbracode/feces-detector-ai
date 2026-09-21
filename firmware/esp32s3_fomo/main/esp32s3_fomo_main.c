/*
 * ESP32-S3 Feces Detector — main entry point
 *
 * Coordinates:
 *   - WiFi station / soft-AP
 *   - OV3660 camera capture
 *   - Edge Impulse FOMO inference (on-device)
 *   - GPIO relay spray control (auto + manual)
 *   - HTTP server (/stream, /status, /spray, /config)
 *   - Reporting to CodeIgniter dashboard + Python YOLOv8 verifier
 *
 * The inference task runs on core 1 while camera capture + MJPEG
 * streaming runs on core 0. See components/ for the detail.
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_service.h"
#include "camera_service.h"
#include "inference_service.h"
#include "spray_controller.h"
#include "http_server_service.h"
#include "detection_service.h"

static const char *TAG = "fomo_main";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    ESP_LOGI(TAG, "Boot: ESP32-S3 Feces Detector (FOMO + YOLOv8 verify)");

    app_config_t cfg;
    app_config_load(&cfg);

    wifi_service_start();

    spray_controller_init(APP_PIN_RELAY, APP_PIN_LED, cfg.threshold,
                          cfg.cooldown_ms, cfg.spray_ms);

    e = camera_service_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed, retrying...");
        for (int i = 0; i < 5 && e != ESP_OK; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            e = camera_service_init();
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed permanently");
        }
    }

    inference_service_init();
    http_server_service_init();
    detection_service_start();

    ESP_LOGI(TAG, "Startup complete. Camera + inference + HTTP running.");
}
