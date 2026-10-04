#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"

static const char *TAG = "app_cfg";

/* Strict dotted-quad parser: four decimal octets 0-255 and nothing else.
 * inet_aton would also accept "10.1" as 10.0.0.1 and octal/short forms, which
 * makes a typo silently mean something other than what was typed -- the kind of
 * error that turns into an unreachable board with no way to correct it. */
static bool prv_parse_ipv4(const char *s, uint32_t *out)
{
    if (s == NULL || out == NULL) return false;
    uint32_t acc = 0;
    int octet = 0;
    const char *p = s;

    for (;;) {
        if (*p < '0' || *p > '9') return false;
        uint32_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            if (++digits > 3 || val > 255) return false;
            p++;
        }
        acc = (acc << 8) | val;
        if (++octet == 4) break;
        if (*p != '.') return false;
        p++;
    }
    if (*p != '\0') return false;   /* trailing junk */
    *out = acc;
    return true;
}

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
    cfg->relay_active_low = APP_DEFAULT_RELAY_ACTIVE_LOW ? true : false;
    cfg->server_trigger_conf = APP_DEFAULT_SERVER_TRIGGER_CONF;
    /* Empty = DHCP. prv_load_static() decides whether to honour them. */
    cfg->static_ip[0] = '\0';
    cfg->static_mask[0] = '\0';
    cfg->static_gw[0] = '\0';
}

/* Apply the stored static address only when all three parts are present and
 * syntactically valid. A partial or malformed set is discarded back to DHCP on
 * purpose: a half-applied static config (address but no gateway, or a typo in
 * the mask) leaves the board reachable by nobody, including the person trying
 * to fix it, since POST /config needs the network that just broke. */
static void prv_load_static(app_config_t *cfg, nvs_handle_t h)
{
    nvs_get_str(h, APP_STATIC_IP_KEY, cfg->static_ip,
                &(size_t){ sizeof(cfg->static_ip) });
    nvs_get_str(h, APP_STATIC_MASK_KEY, cfg->static_mask,
                &(size_t){ sizeof(cfg->static_mask) });
    nvs_get_str(h, APP_STATIC_GW_KEY, cfg->static_gw,
                &(size_t){ sizeof(cfg->static_gw) });

    bool have_ip = cfg->static_ip[0] != '\0';
    bool have_mask = cfg->static_mask[0] != '\0';
    bool have_gw = cfg->static_gw[0] != '\0';

    if (!have_ip && !have_mask && !have_gw) return;   /* all empty: DHCP */

    bool all = have_ip && have_mask && have_gw;
    if (!all) {
        ESP_LOGW(TAG, "Ignoring incomplete static IP config "
                      "(ip=%s mask=%s gw=%s); falling back to DHCP",
                 have_ip ? cfg->static_ip : "-",
                 have_mask ? cfg->static_mask : "-",
                 have_gw ? cfg->static_gw : "-");
        cfg->static_ip[0] = '\0';
        cfg->static_mask[0] = '\0';
        cfg->static_gw[0] = '\0';
        return;
    }

    uint32_t ip, mask, gw;
    if (!prv_parse_ipv4(cfg->static_ip, &ip) ||
        !prv_parse_ipv4(cfg->static_mask, &mask) ||
        !prv_parse_ipv4(cfg->static_gw, &gw)) {
        ESP_LOGW(TAG, "Static IP config is not valid dotted-quad; using DHCP");
        cfg->static_ip[0] = '\0';
        cfg->static_mask[0] = '\0';
        cfg->static_gw[0] = '\0';
        return;
    }

    ESP_LOGI(TAG, "Static IP configured: %s/%s gw %s",
             cfg->static_ip, cfg->static_mask, cfg->static_gw);
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

    /* Stored as u8 (0 = active-high, 1 = active-low). When the key is absent
     * the prv_defaults() value stands, which is the fail-safe active-low
     * assumption -- a device that has never been configured keeps its relay
     * de-energised on reset. */
    uint8_t low = 0;
    if (nvs_get_u8(h, APP_RELAY_ACTIVE_LOW_KEY, &low) == ESP_OK) {
        cfg->relay_active_low = (low != 0);
    }

    /* Absent on devices provisioned before server-gated spray existed, in which
     * case the prv_defaults() value stands. */
    {
        size_t len = sizeof(cfg->server_trigger_conf);
        nvs_get_blob(h, APP_SERVER_TRIGGER_CONF_KEY, &cfg->server_trigger_conf, &len);
    }

    prv_load_static(cfg, h);

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
    nvs_set_u8(h, APP_RELAY_ACTIVE_LOW_KEY, cfg->relay_active_low ? 1 : 0);
    nvs_set_blob(h, APP_SERVER_TRIGGER_CONF_KEY, &cfg->server_trigger_conf,
                 sizeof(cfg->server_trigger_conf));
    nvs_set_str(h, APP_STATIC_IP_KEY, cfg->static_ip);
    nvs_set_str(h, APP_STATIC_MASK_KEY, cfg->static_mask);
    nvs_set_str(h, APP_STATIC_GW_KEY, cfg->static_gw);
    nvs_commit(h);
    nvs_close(h);
}
