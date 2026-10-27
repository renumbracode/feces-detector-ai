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
#include "target_publisher.h"

static const char *TAG = "fomo_main";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* The build stamp is not decoration: the app partition is nearly full, so
     * "which firmware is actually on the board" has to be answerable from the
     * serial log alone, without assuming the flashed binary matches the
     * working tree. __DATE__/__TIME__ are compile-time constants, so they
     * identify the build itself rather than the moment of boot. */
    ESP_LOGI(TAG, "Boot: ESP32-S3 Feces Detector (FOMO + YOLOv8 verify) idf=%s build=%s %s",
             esp_get_idf_version(), __DATE__, __TIME__);

    app_config_t cfg;
    app_config_load(&cfg);

    wifi_service_start();

    spray_controller_init(APP_PIN_RELAY, APP_PIN_LED, APP_PIN_BUZZER,
                          cfg.threshold, cfg.cooldown_ms, cfg.spray_ms,
                          cfg.relay_active_low);
    /* Operator switch from NVS: absent/default = enabled. Applied right after
     * init so a device left switched OFF stays OFF across reboots. */
    spray_controller_set_enabled(cfg.spray_enabled);

    /* Where to publish detections for an external actuator board to aim at.
     * Applied at boot so the setting takes effect immediately rather than
     * after the detection loop's first config refresh. */
    target_publisher_set_config(cfg.target_url, cfg.target_enabled);

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
