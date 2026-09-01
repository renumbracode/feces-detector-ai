/*
 * HTTP server for the ESP32-S3 Feces Detector.
 *
 * Endpoints (dashboard-compatible):
 *   GET /                 simple index
 *   GET /stream           MJPEG with FOMO overlay
 *   GET /status           JSON status (heap, spray, fps, detections, ...)
 *   GET /spray?duration=  manual override spray
 *   GET /config           JSON current config
 *   POST /config          update threshold/cooldown/spray from dashboard
 *
 * The connection handler for /stream runs with a shared frame buffer so the
 * camera task can keep feeding frames; the MJPEG boundary is standard.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "camera_service.h"
#include "inference_service.h"
#include "spray_controller.h"
#include "http_server_service.h"

static const char *TAG = "http_svc";
#define STREAM_BOUNDARY "fomodetectboundary"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"
uint64_t g_start_us;

static const char *wifi_ip_str(void);

static const char INDEX_HTML[] =
    "<!doctype html><html><head><title>ESP32-S3 Feces Detector</title>"
    "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
    "margin:40px;line-height:1.6}code{background:#1e293b;padding:2px 6px;"
    "border-radius:4px}a{color:#34d399}</style></head><body>"
    "<h1>ESP32-S3 Feces Detector</h1>"
    "<p>Edge Impulse FOMO inference runs on-device; YOLOv8 verification "
    "runs on the Python server.</p>"
    "<ul><li><a href=\"/stream\">/stream</a> — MJPEG with FOMO overlay "
    "(point a browser or the dashboard live view at this)"
    "</li><li><a href=\"/status\">/status</a> — JSON status</li>"
    "<li><a href=\"/spray?duration=3000\">/spray?duration=3000</a> — manual spray</li>"
    "<li><a href=\"/config\">/config</a> — device config</li></ul>"
    "</body></html>";

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    spray_status_t sp;
    spray_controller_status(&sp);

    fomo_result_t last;
    inference_service_latest(&last);

    uint32_t free_sram = (uint32_t)esp_get_free_heap_size();
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"heap\":%u,\"fps\":0,\"model\":\"fomo:v1\","
        "\"sprayActive\":%s,\"cooldownActive\":%s,"
        "\"lastConfidence\":%.4f,\"objectPresent\":%s,"
        "\"lastSprayAgoMs\":%llu,\"uptimeS\":%llu,"
        "\"inferenceMs\":%u,\"detections\":0}",
        wifi_ip_str(), free_sram,
        sp.spraying ? "true" : "false",
        sp.cooldown_active ? "true" : "false",
        (double)sp.last_confidence,
        last.object_present ? "true" : "false",
        (unsigned long long)sp.last_spray_ago_ms,
        (unsigned long long)(esp_timer_get_time() / 1000000u),
        last.inference_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handler_spray(httpd_req_t *req)
{
    char buf[16] = "0";
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char dur[16] = "0";
        if (httpd_query_key_value(buf, "duration", dur, sizeof(dur)) == ESP_OK) {
            uint32_t ms = (uint32_t)strtoul(dur, NULL, 10);
            if (ms > 0) {
                spray_controller_force_spray(ms);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"ok\":true,\"spray\":true}");
                return ESP_OK;
            }
        }
    }
    spray_controller_force_spray(0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"spray\":true}");
}

static esp_err_t handler_config(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_load(&cfg);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"threshold\":%.2f,\"cooldown_ms\":%u,\"spray_ms\":%u,"
        "\"verify_url\":\"%s\",\"dash_url\":\"%s\"}",
        (double)cfg.threshold, (unsigned)cfg.cooldown_ms, (unsigned)cfg.spray_ms,
        cfg.verify_url, cfg.dash_url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handler_stream(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    camera_streaming_start();

    char part_hdr[64];
    while (true) {
        camera_fb_t *fb = camera_service_get_frame();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Run FOMO on the frame (camera side). Real deployments often do
         * this on a dedicated core; here we call synchronously for simplicity. */
        fomo_result_t res = inference_service_classify(fb->buf, fb->len);

        httpd_resp_send_chunk(req, "--" STREAM_BOUNDARY "\r\n", HTTPD_RESP_USE_STRLEN);
        int h = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART, (unsigned)fb->len);
        httpd_resp_send_chunk(req, part_hdr, h);
        httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        httpd_resp_send_chunk(req, "\r\n", 2);
        camera_service_release_frame(fb);

        vTaskDelay(pdMS_TO_TICKS(60)); /* ~16 fps target */

        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            break; /* client disconnected */
        }

        /* Optional: trigger spray from detection inside stream handler. */
        (void)res;
    }
    camera_streaming_stop();
    return ESP_OK;
}

static httpd_uri_t uris[] = {
    { .uri = "/",       .method = HTTP_GET, .handler = handler_index,   .user_ctx = NULL },
    { .uri = "/status", .method = HTTP_GET, .handler = handler_status,  .user_ctx = NULL },
    { .uri = "/spray",  .method = HTTP_GET, .handler = handler_spray,   .user_ctx = NULL },
    { .uri = "/config", .method = HTTP_GET, .handler = handler_config,  .user_ctx = NULL },
    { .uri = "/stream", .method = HTTP_GET, .handler = handler_stream,  .user_ctx = NULL },
};

static const char *wifi_ip_str(void)
{
    static char ip[16];
    esp_netif_ip_info_t info;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n && esp_netif_get_ip_info(n, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
        return ip;
    }
    return "0.0.0.0";
}

esp_err_t http_server_service_init(void)
{
    g_start_us = esp_timer_get_time();

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.lru_purge_enable = true;
    c.max_uri_handlers = 8;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &c) == ESP_OK) {
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
            httpd_register_uri_handler(srv, &uris[i]);
        }
    }

    ESP_LOGI(TAG, "HTTP server ready on port %u", c.server_port);
    return ESP_OK;
}
