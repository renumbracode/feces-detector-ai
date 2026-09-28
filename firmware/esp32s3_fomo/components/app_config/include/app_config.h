#pragma once

#include "esp_err.h"

#define APP_NVS_NAMESPACE "fomo_app"
#define APP_WIFI_SSID_KEY "wifi_ssid"
#define APP_WIFI_PASS_KEY "wifi_pass"
#define APP_VERIFY_URL_KEY "verify_url"
#define APP_DASH_URL_KEY "dash_url"
#define APP_DASH_VERIFY_URL_KEY "dash_verify_url"

#define APP_DEFAULT_WIFI_SSID "REDACTED_SSID"
#define APP_DEFAULT_WIFI_PASS "REDACTED_PASS"
#define APP_DEFAULT_VERIFY_URL "http://192.168.1.3:8000/verify"
#define APP_DEFAULT_DASH_URL "http://192.168.1.3/feces-detector-ai/dashboard/api/insert"
#define APP_DEFAULT_DASH_VERIFY_URL "http://192.168.1.3/feces-detector-ai/dashboard/api/verify"

#define APP_PIN_RELAY 4        /* GPIO driving the relay / water pump */
#define APP_PIN_LED 2          /* on-board status LED (flash LED on some boards) */

#define APP_DEFAULT_THRESHOLD 0.60f
#define APP_DEFAULT_COOLDOWN_MS 300000u
#define APP_DEFAULT_SPRAY_MS 5000u
#define APP_DEFAULT_FOMO_CONF 0.8f /* min FOMO "object present" score */

/* Tunables persisted to NVS; on first boot these defaults are written. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char verify_url[129];
    char dash_url[257];
    char dash_verify_url[257];
    float threshold;
    uint32_t cooldown_ms;
    uint32_t spray_ms;
} app_config_t;

esp_err_t app_config_load(app_config_t *cfg);
void app_config_save(const app_config_t *cfg);
