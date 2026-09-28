/*
 * HTTP server for the ESP32-S3 Feces Detector.
 *
 * Endpoints (dashboard-compatible):
 *   GET /                 simple index
 *   GET /stream           MJPEG with FOMO overlay
 *   GET /status           JSON status (heap, spray, fps, detections, ...)
 *   GET /spray?duration=  manual override spray
 *   GET /config           JSON current config
 *   POST /config          update wifi/urls/threshold/cooldown/spray then reboot
 *   GET /setup            HTML form that POSTs to /config (network switching)
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
#include "detection_service.h"
#include "http_server_service.h"

static const char *TAG = "http_svc";
#define STREAM_BOUNDARY "fomodetectboundary"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"
uint64_t g_start_us;

static const char *wifi_ip_str(void);

/* Allow cross-origin browser clients (the CodeIgniter dashboard pages fetch
 * /status and /spray directly from a different host/port). */
static void prv_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

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
    "<li><a href=\"/config\">/config</a> — device config</li>"
    "<li><a href=\"/setup\">/setup</a> — change Wi-Fi / server URLs</li></ul>"
    "</body></html>";

static esp_err_t handler_index(httpd_req_t *req)
{
    prv_cors(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    prv_cors(req);
    spray_status_t sp;
    spray_controller_status(&sp);

    fomo_result_t last;
    inference_service_latest(&last);

    uint32_t free_sram = (uint32_t)esp_get_free_heap_size();
    /* fomo_result_t stores x,y as the box CENTROID; the dashboard overlay
     * draws from the top-left corner, so convert here. */
    float tlx = last.x - last.width / 2.0f;
    float tly = last.y - last.height / 2.0f;
    if (tlx < 0.0f) tlx = 0.0f;
    if (tly < 0.0f) tly = 0.0f;
    if (tlx + last.width > 1.0f) tlx = 1.0f - last.width;
    if (tly + last.height > 1.0f) tly = 1.0f - last.height;
    char buf[1024];
    /* vrf = the server-side YOLOv8 answer for the live frame, polled on a
     * fixed cadence so the dashboard overlay has a real detection even while
     * the on-device FOMO model is still a stub. Display only; it never gates
     * the relay. */
    verify_result_t vr;
    detection_service_verify_result(&vr);
    int n = snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"heap\":%u,\"fps\":0,\"model\":\"%s\","
        "\"sprayActive\":%s,\"cooldownActive\":%s,"
        "\"lastConfidence\":%.4f,\"objectPresent\":%s,"
        "\"classId\":%d,\"box\":{\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f},"
        "\"vrf\":{\"valid\":%s,\"detected\":%s,\"conf\":%.4f,\"ageMs\":%u,"
        "\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f},"
        "\"lastSprayAgoMs\":%llu,\"uptimeS\":%llu,"
        "\"inferenceMs\":%u,\"detections\":0}",
        wifi_ip_str(), (unsigned)free_sram,
        edge_impulse_model_tag(),
        sp.spraying ? "true" : "false",
        sp.cooldown_active ? "true" : "false",
        (double)sp.last_confidence,
        last.object_present ? "true" : "false",
        last.class_id,
        (double)tlx, (double)tly,
        (double)last.width, (double)last.height,
        vr.valid ? "true" : "false",
        vr.detected ? "true" : "false",
        (double)vr.conf,
        (unsigned)vr.age_ms,
        (double)vr.x, (double)vr.y, (double)vr.w, (double)vr.h,
        (unsigned long long)sp.last_spray_ago_ms,
        (unsigned long long)(esp_timer_get_time() / 1000000u),
        (unsigned)last.inference_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handler_spray(httpd_req_t *req)
{
    prv_cors(req);
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
    prv_cors(req);
    app_config_t cfg;
    app_config_load(&cfg);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"threshold\":%.2f,\"cooldown_ms\":%u,\"spray_ms\":%u,"
        "\"verify_url\":\"%s\",\"dash_url\":\"%s\",\"dash_verify_url\":\"%s\"}",
        (double)cfg.threshold, (unsigned)cfg.cooldown_ms, (unsigned)cfg.spray_ms,
        cfg.verify_url, cfg.dash_url, cfg.dash_verify_url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t prv_read_body(httpd_req_t *req, char *buf, size_t size)
{
    int total = 0;
    while (total < (int)size - 1) {
        int r = httpd_req_recv(req, buf + total, size - 1 - (size_t)total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    return total > 0 ? ESP_OK : ESP_FAIL;
}

/* POST /config — persist runtime settings to NVS then reboot.
 * Body is URL-encoded key=value pairs (form or query string), e.g.
 *   wifi_ssid, wifi_pass, verify_url, dash_url, threshold,
 *   cooldown_ms, spray_ms.
 * This is how the device is pointed at a different Wi-Fi/server without
 * recompiling: save here, connect a browser to /setup on the current
 * network, submit, and the board boots into the new network. */
static esp_err_t handler_config_post(httpd_req_t *req)
{
    prv_cors(req);
    char body[1024];
    if (prv_read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }

    app_config_t cfg;
    app_config_load(&cfg);

    char val[270];
    if (httpd_query_key_value(body, "wifi_ssid", val, sizeof(val)) == ESP_OK && val[0]) {
        strncpy(cfg.wifi_ssid, val, sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "wifi_pass", val, sizeof(val)) == ESP_OK) {
        strncpy(cfg.wifi_pass, val, sizeof(cfg.wifi_pass) - 1);
        cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "verify_url", val, sizeof(val)) == ESP_OK && val[0]) {
        strncpy(cfg.verify_url, val, sizeof(cfg.verify_url) - 1);
        cfg.verify_url[sizeof(cfg.verify_url) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "dash_url", val, sizeof(val)) == ESP_OK && val[0]) {
        strncpy(cfg.dash_url, val, sizeof(cfg.dash_url) - 1);
        cfg.dash_url[sizeof(cfg.dash_url) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "dash_verify_url", val, sizeof(val)) == ESP_OK
            && val[0]) {
        strncpy(cfg.dash_verify_url, val, sizeof(cfg.dash_verify_url) - 1);
        cfg.dash_verify_url[sizeof(cfg.dash_verify_url) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "threshold", val, sizeof(val)) == ESP_OK) {
        float t = strtof(val, NULL);
        if (t >= 0.0f && t <= 1.0f) cfg.threshold = t;
    }
    if (httpd_query_key_value(body, "cooldown_ms", val, sizeof(val)) == ESP_OK) {
        cfg.cooldown_ms = (uint32_t)strtoul(val, NULL, 10);
    }
    if (httpd_query_key_value(body, "spray_ms", val, sizeof(val)) == ESP_OK) {
        cfg.spray_ms = (uint32_t)strtoul(val, NULL, 10);
    }

    app_config_save(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"saved\":true,\"restarting\":true}");

    ESP_LOGI(TAG, "Config updated via POST /config, restarting to apply");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK; /* not reached */
}

/* GET /setup — dependency-free HTML form for switching network/server config. */
static esp_err_t handler_setup(httpd_req_t *req)
{
    prv_cors(req);
    app_config_t cfg;
    app_config_load(&cfg);

    char *page = malloc(3072);
    if (page == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }
    int n = snprintf(page, 3072,
        "<!doctype html><html><head><title>Feces Detector setup</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
        "margin:40px;line-height:1.6}label{display:block;margin-top:12px;font-size:14px}"
        "input{width:100%%;padding:8px;margin:6px 0;border-radius:6px;"
        "border:1px solid #334155;background:#1e293b;color:#e2e8f0;box-sizing:border-box}"
        "button{padding:10px 18px;border:0;border-radius:6px;background:#34d399;"
        "color:#064e3b;font-weight:600;margin-top:16px}code{background:#1e293b;"
        "padding:2px 6px}</style></head><body><h1>Feces Detector setup</h1>"
        "<p>Saved settings apply after the board restarts (~15 s). If you change "
        "the Wi-Fi, find the board's new IP in the router's DHCP/connected-device "
        "list, or re-flash. Server URLs must be reachable from this board's network.</p>"
        "<form method='post' action='/config'>"
        "<label>Wi-Fi SSID</label><input name='wifi_ssid' value='%s' maxlength='32'>"
        "<label>Wi-Fi password</label><input name='wifi_pass' value='%s' maxlength='64'>"
        "<label>YOLOv8 verify URL</label><input name='verify_url' value='%s' maxlength='128'>"
        "<label>Dashboard API URL</label><input name='dash_url' value='%s' maxlength='256'>"
        "<label>Dashboard verify URL (yolo_conf report)</label><input name='dash_verify_url' "
        "value='%s' maxlength='256'>"
        "<label>Detection threshold (0..1)</label><input type='number' step='0.05' "
        "name='threshold' value='%.2f' min='0' max='1'>"
        "<label>Cooldown ms</label><input type='number' step='1000' name='cooldown_ms' "
        "value='%u'>"
        "<label>Spray duration ms</label><input type='number' step='100' name='spray_ms' "
        "value='%u'>"
        "<button type='submit'>Save &amp; restart</button></form>"
        "</body></html>",
        cfg.wifi_ssid, cfg.wifi_pass, cfg.verify_url, cfg.dash_url, cfg.dash_verify_url,
        (double)cfg.threshold, (unsigned)cfg.cooldown_ms, (unsigned)cfg.spray_ms);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, n);
    free(page);
    return ESP_OK;
}

/*
 * The MJPEG loop lives on its own task. httpd runs every handler on a single
 * server task, so a long-lived /stream handler would otherwise stall /status
 * and every other endpoint while the camera is streaming (the dashboard then
 * reports "device unreachable"). Hand the request to this task with the
 * IDF 5.x async API so the server task stays free.
 */
static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char part_hdr[64];

    while (true) {
        camera_fb_t *fb = camera_service_get_frame();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        httpd_resp_send_chunk(req, "--" STREAM_BOUNDARY "\r\n", HTTPD_RESP_USE_STRLEN);
        int h = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART, (unsigned)fb->len);
        esp_err_t res = httpd_resp_send_chunk(req, part_hdr, h);
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        camera_service_release_frame(fb);

        if (res != ESP_OK) {
            break; /* client disconnected or send failed */
        }
        vTaskDelay(pdMS_TO_TICKS(40)); /* ~25 fps target; inference runs in detect task */
    }
    camera_streaming_stop();
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t handler_stream(httpd_req_t *req)
{
    prv_cors(req);
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    camera_streaming_start();

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        camera_streaming_stop();
        ESP_LOGE(TAG, "stream async begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream unavailable");
        return ESP_OK;
    }

    /* Priority 4 keeps the httpd task (prio 5) responsive to /status. */
    if (xTaskCreate(stream_task, "stream", 4096, async_req, 4, NULL) != pdPASS) {
        camera_streaming_stop();
        httpd_req_async_handler_complete(async_req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no stream task");
        return ESP_OK;
    }
    return ESP_OK; /* the stream task owns the response from here */
}

static httpd_uri_t uris[] = {
    { .uri = "/",       .method = HTTP_GET,  .handler = handler_index,       .user_ctx = NULL },
    { .uri = "/status", .method = HTTP_GET,  .handler = handler_status,      .user_ctx = NULL },
    { .uri = "/spray",  .method = HTTP_GET,  .handler = handler_spray,       .user_ctx = NULL },
    { .uri = "/config", .method = HTTP_GET,  .handler = handler_config,      .user_ctx = NULL },
    { .uri = "/config", .method = HTTP_POST, .handler = handler_config_post, .user_ctx = NULL },
    { .uri = "/setup",  .method = HTTP_GET,  .handler = handler_setup,       .user_ctx = NULL },
    { .uri = "/stream", .method = HTTP_GET,  .handler = handler_stream,      .user_ctx = NULL },
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
    c.max_uri_handlers = 10;
    c.stack_size = 8192; /* setup handler builds ~2 KB HTML on the task stack */

    httpd_handle_t srv = NULL;
    esp_err_t start = httpd_start(&srv, &c);
    if (start != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(start));
        return start;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server ready on port %u", c.server_port);
    return ESP_OK;
}
