#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "main_config.h"

static const char *TAG = "act_cfg";

/* NVS keys. Capped at 15 characters by the NVS API, so these are short and
 * some are abbreviated. */
#define K_CAM_X          "cam_x"
#define K_CAM_Y          "cam_y"
#define K_CAM_HEIGHT     "cam_height"
#define K_CAM_YAW        "cam_yaw"
#define K_CAM_PITCH      "cam_pitch"
#define K_FOCAL          "focal_px"
#define K_NOZZLE_X       "nozzle_x"
#define K_NOZZLE_Y       "nozzle_y"
#define K_NOZZLE_H       "nozzle_h"
#define K_PAN_MIN        "pan_min"
#define K_PAN_MAX        "pan_max"
#define K_TILT_MIN       "tilt_min"
#define K_TILT_MAX       "tilt_max"
#define K_PULSE_MIN      "pulse_min"
#define K_PULSE_MAX      "pulse_max"
#define K_WATER_MIN      "water_min"
#define K_SPRAY_MS       "spray_ms"
#define K_COOLDOWN_MS    "cooldown_ms"
#define K_RELAY_LOW      "relay_low"
#define K_ENABLED        "enabled"
#define K_FLOAT_SWITCH   "float_sw"
#define K_WIFI_SSID      "wifi_ssid"
#define K_WIFI_PASS      "wifi_pass"

void main_config_defaults(main_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* A camera 2 m up, 2 m in front of the pen centre, looking down 45
     * degrees. These are PLACEHOLDERS -- the whole point of the /setup page
     * is that the operator replaces them with measured numbers. */
    cfg->cam_x = 0.0f;
    cfg->cam_y = 2000.0f;
    cfg->cam_height = 2000.0f;
    cfg->cam_yaw_deg = 0.0f;
    cfg->cam_pitch_deg = 45.0f;
    /* A 60 degree horizontal field of view on a 640 px wide frame. */
    cfg->focal_px = 640.0f / (2.0f * tanf(30.0f * 3.14159265f / 180.0f));

    cfg->nozzle_x = 0.0f;
    cfg->nozzle_y = 0.0f;
    cfg->nozzle_height = 1000.0f;

    cfg->pan_min = -90.0f;
    cfg->pan_max = 90.0f;
    cfg->tilt_min = 0.0f;
    cfg->tilt_max = 90.0f;

    cfg->servo_pulse_min_us = 500;
    cfg->servo_pulse_max_us = 2400;

    cfg->water_min_pct = 20.0f;
    cfg->spray_ms = 3000;
    cfg->cooldown_ms = 30000;

    cfg->relay_active_low = true;
    cfg->enabled = true;
    cfg->use_float_switch = false;
}

static void prv_get_f(nvs_handle_t h, const char *key, float *dst)
{
    size_t len = sizeof(*dst);
    if (nvs_get_blob(h, key, dst, &len) != ESP_OK) {
        /* Absent: leave the default standing. */
        return;
    }
}

static void prv_get_u(nvs_handle_t h, const char *key, uint32_t *dst)
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) == ESP_OK) *dst = v;
}

static void prv_get_b(nvs_handle_t h, const char *key, bool *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *dst = (v != 0);
}

void main_config_load(main_config_t *cfg)
{
    if (cfg == NULL) return;
    main_config_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(MAIN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no stored config; using defaults");
        return;
    }

    prv_get_f(h, K_CAM_X, &cfg->cam_x);
    prv_get_f(h, K_CAM_Y, &cfg->cam_y);
    prv_get_f(h, K_CAM_HEIGHT, &cfg->cam_height);
    prv_get_f(h, K_CAM_YAW, &cfg->cam_yaw_deg);
    prv_get_f(h, K_CAM_PITCH, &cfg->cam_pitch_deg);
    prv_get_f(h, K_FOCAL, &cfg->focal_px);
    prv_get_f(h, K_NOZZLE_X, &cfg->nozzle_x);
    prv_get_f(h, K_NOZZLE_Y, &cfg->nozzle_y);
    prv_get_f(h, K_NOZZLE_H, &cfg->nozzle_height);
    prv_get_f(h, K_PAN_MIN, &cfg->pan_min);
    prv_get_f(h, K_PAN_MAX, &cfg->pan_max);
    prv_get_f(h, K_TILT_MIN, &cfg->tilt_min);
    prv_get_f(h, K_TILT_MAX, &cfg->tilt_max);
    prv_get_u(h, K_PULSE_MIN, &cfg->servo_pulse_min_us);
    prv_get_u(h, K_PULSE_MAX, &cfg->servo_pulse_max_us);
    prv_get_f(h, K_WATER_MIN, &cfg->water_min_pct);
    prv_get_u(h, K_SPRAY_MS, &cfg->spray_ms);
    prv_get_u(h, K_COOLDOWN_MS, &cfg->cooldown_ms);
    prv_get_b(h, K_RELAY_LOW, &cfg->relay_active_low);
    prv_get_b(h, K_ENABLED, &cfg->enabled);
    prv_get_b(h, K_FLOAT_SWITCH, &cfg->use_float_switch);

    nvs_get_str(h, K_WIFI_SSID, cfg->wifi_ssid,
                &(size_t){ sizeof(cfg->wifi_ssid) });
    nvs_get_str(h, K_WIFI_PASS, cfg->wifi_pass,
                &(size_t){ sizeof(cfg->wifi_pass) });

    nvs_close(h);
}

bool main_config_load_wifi(char *ssid, size_t ssid_cap,
                           char *pass, size_t pass_cap)
{
    if (ssid == NULL || pass == NULL) return false;

    main_config_t c;
    main_config_load(&c);
    if (c.wifi_ssid[0] == '\0') return false;

    snprintf(ssid, ssid_cap, "%.32s", c.wifi_ssid);
    snprintf(pass, pass_cap, "%.64s", c.wifi_pass);
    return true;
}

esp_err_t main_config_save(const main_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(MAIN_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    nvs_set_blob(h, K_CAM_X, &cfg->cam_x, sizeof(cfg->cam_x));
    nvs_set_blob(h, K_CAM_Y, &cfg->cam_y, sizeof(cfg->cam_y));
    nvs_set_blob(h, K_CAM_HEIGHT, &cfg->cam_height, sizeof(cfg->cam_height));
    nvs_set_blob(h, K_CAM_YAW, &cfg->cam_yaw_deg, sizeof(cfg->cam_yaw_deg));
    nvs_set_blob(h, K_CAM_PITCH, &cfg->cam_pitch_deg, sizeof(cfg->cam_pitch_deg));
    nvs_set_blob(h, K_FOCAL, &cfg->focal_px, sizeof(cfg->focal_px));
    nvs_set_blob(h, K_NOZZLE_X, &cfg->nozzle_x, sizeof(cfg->nozzle_x));
    nvs_set_blob(h, K_NOZZLE_Y, &cfg->nozzle_y, sizeof(cfg->nozzle_y));
    nvs_set_blob(h, K_NOZZLE_H, &cfg->nozzle_height, sizeof(cfg->nozzle_height));
    nvs_set_blob(h, K_PAN_MIN, &cfg->pan_min, sizeof(cfg->pan_min));
    nvs_set_blob(h, K_PAN_MAX, &cfg->pan_max, sizeof(cfg->pan_max));
    nvs_set_blob(h, K_TILT_MIN, &cfg->tilt_min, sizeof(cfg->tilt_min));
    nvs_set_blob(h, K_TILT_MAX, &cfg->tilt_max, sizeof(cfg->tilt_max));
    nvs_set_u32(h, K_PULSE_MIN, cfg->servo_pulse_min_us);
    nvs_set_u32(h, K_PULSE_MAX, cfg->servo_pulse_max_us);
    nvs_set_blob(h, K_WATER_MIN, &cfg->water_min_pct, sizeof(cfg->water_min_pct));
    nvs_set_u32(h, K_SPRAY_MS, cfg->spray_ms);
    nvs_set_u32(h, K_COOLDOWN_MS, cfg->cooldown_ms);
    nvs_set_u8(h, K_RELAY_LOW, cfg->relay_active_low ? 1 : 0);
    nvs_set_u8(h, K_ENABLED, cfg->enabled ? 1 : 0);
    nvs_set_u8(h, K_FLOAT_SWITCH, cfg->use_float_switch ? 1 : 0);
    nvs_set_str(h, K_WIFI_SSID, cfg->wifi_ssid);
    nvs_set_str(h, K_WIFI_PASS, cfg->wifi_pass);

    e = nvs_commit(h);
    nvs_close(h);
    return e;
}
