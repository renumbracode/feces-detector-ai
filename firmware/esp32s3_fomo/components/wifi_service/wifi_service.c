#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_service.h"

static const char *TAG = "wifi_svc";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_events;

static void prv_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_service_start(void)
{
    app_config_t cfg;
    app_config_load(&cfg);

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &prv_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &prv_event, NULL));

    wifi_config_t wf = { 0 };
    memcpy(wf.sta.ssid, cfg.wifi_ssid, sizeof(wf.sta.ssid) - 1);
    memcpy(wf.sta.password, cfg.wifi_pass, sizeof(wf.sta.password) - 1);
    wf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wf));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Keep the radio always on: the dashboard + verify server must be able
     * to reach /stream and /status at any time. Modem-sleep makes the STA
     * unresponsive on the LAN (dropped ARP/TCP). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to SSID: %s", cfg.wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        esp_netif_ip_info_t info;
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n && esp_netif_get_ip_info(n, &info) == ESP_OK) {
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&info.ip));
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "AP unreachable - continuing (detection still runs on-device)");
    return ESP_OK;
}
