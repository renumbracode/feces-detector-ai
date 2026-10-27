/*
 * Main ESP32 -- the actuator board (Laptop B).
 *
 * It owns the pump relay, the two nozzle servos and the water-level sensor.
 * It does NOT own the camera, the detection or any inference: the camera
 * board on Laptop A detects the feces and sends the coordinates, and this
 * board decides what to do about them.
 *
 * The wire contract is docs/target-protocol.md.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "cleaning_cycle.h"
#include "main_config.h"
#include "servo_driver.h"
#include "target_server.h"
#include "water_sensor.h"

static const char *TAG = "act_main";

/* Pin assignments.
 *
 * This board has no camera, so most of the pins the camera board spends on
 * the OV3660 are free here. The relay stays on GPIO14 so the wiring habit
 * from the camera board carries over.
 *
 *   GPIO14  relay IN (water pump)
 *   GPIO16  pan servo signal
 *   GPIO17  tilt servo signal
 *   GPIO1   water level, analog (ADC1 channel 0)
 *   GPIO4   water float switch, digital (optional interlock)
 *   GPIO21  status LED
 */
#define PIN_RELAY       14
#define PIN_SERVO_PAN   16
#define PIN_SERVO_TILT  17
#define PIN_WATER_ADC   1
#define PIN_WATER_FLOAT 4
#define PIN_LED         21

#define HTTP_PORT 8080

static void prv_wifi_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected; retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void prv_wifi_start(const char *ssid, const char *pass)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        prv_wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        prv_wifi_event_handler, NULL, NULL);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    ESP_LOGI(TAG, "Boot: main ESP32 (actuator board) idf=%s build=%s %s",
             esp_get_idf_version(), __DATE__, __TIME__);

    main_config_t cfg;
    main_config_load(&cfg);

    /* The servos first: they must be at a known position before anything
     * can move the nozzle. */
    servo_limits_t pan = {
        .pulse_min_us = cfg.servo_pulse_min_us,
        .pulse_max_us = cfg.servo_pulse_max_us,
        .angle_min_deg = cfg.pan_min,
        .angle_max_deg = cfg.pan_max,
    };
    servo_limits_t tilt = {
        .pulse_min_us = cfg.servo_pulse_min_us,
        .pulse_max_us = cfg.servo_pulse_max_us,
        .angle_min_deg = cfg.tilt_min,
        .angle_max_deg = cfg.tilt_max,
    };
    ESP_ERROR_CHECK(servo_driver_init(PIN_SERVO_PAN, PIN_SERVO_TILT, &pan, &tilt));

    ESP_ERROR_CHECK(water_sensor_init(PIN_WATER_ADC, PIN_WATER_FLOAT,
                                      cfg.use_float_switch));

    ESP_ERROR_CHECK(cleaning_cycle_init(PIN_RELAY, cfg.relay_active_low,
                                        cfg.water_min_pct, cfg.spray_ms,
                                        cfg.cooldown_ms));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Wi-Fi credentials. These come from the board's own NVS, written by the
     * /setup page. There are no compiled-in defaults, because a default
     * password in the source is a default password in every clone. */
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    main_config_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (ssid[0] == '\0') {
        ESP_LOGE(TAG, "no Wi-Fi credentials stored; connect over USB and use "
                      "/setup to set them");
    } else {
        prv_wifi_start(ssid, pass);
    }

    target_server_opts_t opts = {
        .wifi_ssid = ssid,
        .wifi_pass = pass,
        .http_port = HTTP_PORT,
    };
    ESP_ERROR_CHECK(target_server_start(&opts));

    ESP_LOGI(TAG, "Startup complete. Relays, servos, water sensor and the "
                  "target server are up.");
}
