#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"

static void prv_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->wifi_ssid, APP_DEFAULT_WIFI_SSID, sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_pass, APP_DEFAULT_WIFI_PASS, sizeof(cfg->wifi_pass) - 1);
    strncpy(cfg->verify_url, APP_DEFAULT_VERIFY_URL, sizeof(cfg->verify_url) - 1);
    strncpy(cfg->dash_url, APP_DEFAULT_DASH_URL, sizeof(cfg->dash_url) - 1);
    strncpy(cfg->dash_verify_url, APP_DEFAULT_DASH_VERIFY_URL,
            sizeof(cfg->dash_verify_url) - 1);
    cfg->threshold = APP_DEFAULT_THRESHOLD;
    cfg->cooldown_ms = APP_DEFAULT_COOLDOWN_MS;
    cfg->spray_ms = APP_DEFAULT_SPRAY_MS;
}

esp_err_t app_config_load(app_config_t *cfg)
{
    prv_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_FAIL;
    }

    nvs_get_str(h, APP_WIFI_SSID_KEY, cfg->wifi_ssid, &(size_t){ sizeof(cfg->wifi_ssid) });
    nvs_get_str(h, APP_WIFI_PASS_KEY, cfg->wifi_pass, &(size_t){ sizeof(cfg->wifi_pass) });
    nvs_get_str(h, APP_VERIFY_URL_KEY, cfg->verify_url, &(size_t){ sizeof(cfg->verify_url) });
    nvs_get_str(h, APP_DASH_URL_KEY, cfg->dash_url, &(size_t){ sizeof(cfg->dash_url) });
    nvs_get_str(h, APP_DASH_VERIFY_URL_KEY, cfg->dash_verify_url,
                &(size_t){ sizeof(cfg->dash_verify_url) });

    {
        size_t len = sizeof(cfg->threshold);
        nvs_get_blob(h, "threshold", &cfg->threshold, &len);
    }
    uint32_t u;
    if (nvs_get_u32(h, "cooldown_ms", &u) == ESP_OK) cfg->cooldown_ms = u;
    if (nvs_get_u32(h, "spray_ms", &u) == ESP_OK) cfg->spray_ms = u;

    nvs_close(h);
    return ESP_OK;
}

void app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, APP_WIFI_SSID_KEY, cfg->wifi_ssid);
    nvs_set_str(h, APP_WIFI_PASS_KEY, cfg->wifi_pass);
    nvs_set_str(h, APP_VERIFY_URL_KEY, cfg->verify_url);
    nvs_set_str(h, APP_DASH_URL_KEY, cfg->dash_url);
    nvs_set_str(h, APP_DASH_VERIFY_URL_KEY, cfg->dash_verify_url);
    nvs_set_blob(h, "threshold", &cfg->threshold, sizeof(cfg->threshold));
    nvs_set_u32(h, "cooldown_ms", cfg->cooldown_ms);
    nvs_set_u32(h, "spray_ms", cfg->spray_ms);
    nvs_commit(h);
    nvs_close(h);
}
