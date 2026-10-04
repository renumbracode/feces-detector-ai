#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_service.h"

static const char *TAG = "wifi_svc";
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_events;

/* Association state, owned by the event handler (writer) and read by the
 * monitor task and the /status handler. A bool store is a single 32-bit write,
 * so a concurrent read cannot observe a torn value. */
static volatile bool s_connected = false;

/* Consecutive failed connect attempts, reset whenever we associate. Drives the
 * reconnect backoff and is reported on /status so a degrading link is visible
 * before it turns into a visibly flaky dashboard. */
static volatile uint32_t s_reconnect_attempts = 0;

/* Timestamp of the last disconnect, so the backoff is measured from the drop
 * rather than from when the event happened to be processed. */
static volatile int64_t s_last_disconnect_us = 0;

/* Reconnect backoff. The old code called esp_wifi_connect() directly inside
 * the DISCONNECTED event, so every drop triggered an immediate retry. On a
 * marginal link that becomes a hot reconnect loop: the STA re-scans and re-
 * associates continuously, which keeps the radio busy and measurably makes an
 * already-flaky link worse. Exponential backoff with a cap keeps recovery
 * fast on a brief hiccup and gentle when the AP is genuinely down. */
#define WIFI_BACKOFF_MIN_MS  1000
#define WIFI_BACKOFF_MAX_MS  16000

/* How often the monitor task samples and logs link quality. Long enough not
 * to add noise to the serial console, short enough to see a fade developing. */
#define WIFI_MONITOR_PERIOD_MS 10000

static void prv_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Do NOT reconnect here. Record the drop and let the monitor task own
         * the retry so the delay is honoured; calling esp_wifi_connect() from
         * inside this handler is what produced the reconnect storm. */
        s_connected = false;
        s_last_disconnect_us = esp_timer_get_time();
        ESP_LOGW(TAG, "WiFi disconnected");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* Under a static address no GOT_IP event is ever emitted, so treat
         * association itself as "connected" and report the configured address.
         * Under DHCP this fires first and is harmless: GOT_IP arrives moments
         * later and re-publishes the lease address, which is the authoritative
         * one, so the two paths converge on the same value. */
        wifi_ap_record_t ap;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
        esp_netif_ip_info_t info;
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n && esp_netif_get_ip_info(n, &info) == ESP_OK) {
            ESP_LOGI(TAG, "Associated, RSSI %d dBm, IP " IPSTR, (int)rssi,
                     IP2STR(&info.ip));
        }
        s_connected = true;
        s_reconnect_attempts = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        /* Report RSSI at association time. A value much worse than -60 dBm
         * points at the antenna/placement rather than the AP: the phone at
         * the same spot will show full bars while this board cannot hold a
         * link. */
        wifi_ap_record_t ap;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "WiFi connected, RSSI %d dBm%s",
                 (int)rssi,
                 rssi < -75 ? " (WEAK - check antenna clearance / camera placement)"
                            : "");
        s_connected = true;
        s_reconnect_attempts = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

int8_t wifi_service_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected) return INT8_MIN;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return INT8_MIN;
    return ap.rssi;
}

bool wifi_service_is_connected(void)
{
    return s_connected;
}

uint32_t wifi_service_reconnect_attempts(void)
{
    return s_reconnect_attempts;
}

static uint32_t prv_backoff_ms(uint32_t attempts)
{
    /* 1s, 2s, 4s, 8s, then hold at 16s. attempts==0 would be an immediate
     * retry, which callers only use on the very first drop. */
    if (attempts == 0) return 0;
    uint32_t d = WIFI_BACKOFF_MIN_MS << (attempts - 1);
    return (d > WIFI_BACKOFF_MAX_MS) ? WIFI_BACKOFF_MAX_MS : d;
}

/* Owns reconnection and link-quality reporting. One task, 10 s period, so it
 * costs almost nothing and runs on core 0 alongside Wi-Fi housekeeping rather
 * than competing with the detection task on core 1. */
static void prv_link_task(void *arg)
{
    (void)arg;
    int64_t next_log_us = esp_timer_get_time();

    for (;;) {
        int64_t now = esp_timer_get_time();

        if (!s_connected) {
            int64_t since_drop_ms = (now - s_last_disconnect_us) / 1000;
            uint32_t wait = prv_backoff_ms(s_reconnect_attempts);
            if (since_drop_ms >= (int64_t)wait) {
                s_reconnect_attempts++;
                ESP_LOGD(TAG, "reconnect attempt %u (backoff %u ms)",
                         (unsigned)s_reconnect_attempts, (unsigned)wait);
                esp_wifi_connect();
                /* Wait out this attempt before retrying. esp_wifi_connect() is
                 * itself async; the ASSOCIATED/GOT_IP event clears
                 * s_connected, but if the AP never answers we would otherwise
                 * re-issue on the very next pass with no pause at all. */
                vTaskDelay(pdMS_TO_TICKS(wait ? wait : WIFI_BACKOFF_MIN_MS));
            }
        }

        if (now >= next_log_us) {
            next_log_us = now + (int64_t)WIFI_MONITOR_PERIOD_MS * 1000;
            if (s_connected) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    ESP_LOGI(TAG, "link: RSSI %d dBm ch=%d", (int)ap.rssi,
                             (int)ap.primary);
                }
            } else {
                ESP_LOGW(TAG, "link: disconnected, %u reconnect attempt(s) so far",
                         (unsigned)s_reconnect_attempts);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t wifi_service_start(void)
{
    /* static: app_config_t is ~750 B and this runs on the main task, whose
     * stack is small. Keeping it off the stack avoids smashing the heap. */
    static app_config_t cfg;
    app_config_load(&cfg);

    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    /* Optional static address. Must be applied AFTER esp_netif is up but
     * BEFORE esp_wifi_start(), because starting Wi-Fi is what begins DHCP --
     * setting the address first means the DHCP client never runs and cannot
     * overwrite it. app_config_load has already validated the triple, so a
     * malformed value never reaches here. */
    if (cfg.static_ip[0] != '\0' && sta != NULL) {
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        if (esp_ip4addr_aton(cfg.static_ip) != 0 &&
            esp_ip4addr_aton(cfg.static_mask) != 0 &&
            esp_ip4addr_aton(cfg.static_gw) != 0) {
            /* esp_ip4addr_aton() returns the address in network order and 0 on
             * parse failure. Assigning the fields individually keeps the
             * esp_netif_ip_info_t layout (ip, netmask, gw) intact. */
            ip.ip.addr = esp_ip4addr_aton(cfg.static_ip);
            ip.netmask.addr = esp_ip4addr_aton(cfg.static_mask);
            ip.gw.addr = esp_ip4addr_aton(cfg.static_gw);
            esp_netif_dhcpc_stop(sta);
            if (esp_netif_set_ip_info(sta, &ip) == ESP_OK) {
                ESP_LOGI(TAG, "Static IP set to %s/%s gw %s (DHCP stopped)",
                         cfg.static_ip, cfg.static_mask, cfg.static_gw);
            } else {
                ESP_LOGE(TAG, "Failed to apply static IP %s; leaving DHCP on",
                         cfg.static_ip);
            }
        } else {
            ESP_LOGE(TAG, "static IP parse failed at runtime; leaving DHCP on");
        }
    }

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &prv_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &prv_event, NULL));

    wifi_config_t wf = { 0 };
    memcpy(wf.sta.ssid, cfg.wifi_ssid, sizeof(wf.sta.ssid) - 1);
    memcpy(wf.sta.password, cfg.wifi_pass, sizeof(wf.sta.password) - 1);
    wf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* Scan every channel and take the strongest BSSID rather than committing to
     * whichever the AP happens to advertise first. On a phone hotspot, which
     * may expose several virtual BSSIDs on different channels, this avoids
     * latching onto a poor one. */
    wf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* The previous default (WIFI_FAST_SCAN) stops at the first SSID match and
     * can pick a weak channel, which matters when the link is already marginal. */
    wf.sta.threshold.rssi = -90;   /* accept anything this weak or better */
    wf.sta.listen_interval = 0;    /* DTIM 1: no beacon-based power saving */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wf));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Keep the radio always on: the dashboard + verify server must be able
     * to reach /stream and /status at any time. Modem-sleep makes the STA
     * unresponsive on the LAN (dropped ARP/TCP). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to SSID: %s", cfg.wifi_ssid);

    /* Started before the wait so link monitoring is live even if the first
     * association attempt is still in flight. */
    xTaskCreate(prv_link_task, "wifi_link", 3072, NULL, 4, NULL);

    EventBits_t bits = xEventGroupWaitBits(s_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    /* Association succeeds on a static-IP setup, but IP_EVENT_STA_GOT_IP only
 * fires from DHCP -- with DHCP stopped the board associates and then waits
 * forever for an event that never comes, so the "connected" state has to be
 * published once association completes instead. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        esp_netif_ip_info_t info;
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n && esp_netif_get_ip_info(n, &info) == ESP_OK) {
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&info.ip));
        }
        return ESP_OK;
    }

    /* Not a fatal error: the link task keeps retrying with backoff while the
     * on-device detection and spray path continues to run offline. */
    ESP_LOGW(TAG, "AP unreachable after 30 s - continuing (detection still "
                  "runs on-device; %u retry pending)",
             (unsigned)prv_backoff_ms(s_reconnect_attempts + 1) / 1000);
    return ESP_OK;
}