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
#include <strings.h>
#include <stdint.h>
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
#include "wifi_service.h"
#include "http_server_service.h"

static const char *TAG = "http_svc";
#define STREAM_BOUNDARY "fomodetectboundary"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

/* Stream pacing bounds. The interval is the delay AFTER a frame is sent, so it
 * adds to the send time rather than replacing it.
 *
 * 40 ms is the historical fixed delay (~25 fps on a healthy link). The 500 ms
 * ceiling matters on a degraded link: MJPEG over TCP cannot drop frames, so a
 * congested socket stalls the whole task, and pushing faster only deepens the
 * backlog. Slower-but-current video is far more useful for checking the pen
 * than fast-but-frozen video. */
#define STREAM_INTERVAL_MIN_MS 40
#define STREAM_INTERVAL_MAX_MS 500
#define STREAM_INTERVAL_STEP_MS 40

uint64_t g_start_us;

/* Real MJPEG throughput, surfaced by /status. This used to be hardcoded to 0,
 * which made the stream rate unmeasurable and left the frame budget
 * untunable -- the only way to judge a change was to time downloads. A float
 * store is a single 32-bit write, so /status reading it concurrently is safe
 * without a lock. */
static volatile uint32_t s_stream_frames = 0;
static volatile float s_stream_fps = 0.0f;
static volatile int64_t s_stream_window_ms = 0;

/* Byte-rate accounting, used to keep the stream from saturating the link.
 *
 * The adaptive interval below used to react only to how long a send took, which
 * is blind to frame SIZE. At 48 KB/frame a send can be slow simply because the
 * frame is large, so the loop kept the rate high and filled the socket until
 * /status could not get a connection behind it. Tracking bytes/sec gives the
 * pacer a second, independent signal: it can hold the stream under a budget and
 * leave headroom for every other endpoint on the same TCP path. */
static volatile uint32_t s_stream_bytes = 0;
static volatile uint32_t s_stream_bps = 0;
/* 120 KB/s. This was 61440 (60 KB/s), which sized itself against a frame the
 * sensor never actually produced: at 1280x720 and jpeg_quality 30 a frame
 * measured 25,753 bytes, so 60 KB/s capped the live view at 61440/25753 =
 * ~2.4 fps and the pacer sat pinned against its ceiling on a healthy link
 * (measured 2.0-2.25 fps). The budget, not the Wi-Fi, was the binding
 * constraint.
 *
 * The frame size has since dropped to ~10 KB via FRAMESIZE_VGA, so at the old
 * 60 KB/s budget the cap would be ~6 fps -- and that would leave headroom
 * unused. 120 KB/s allows ~12 fps, with the adaptive interval above (and the
 * link itself) determining the real rate.
 *
 * This is a ceiling on OFFERED load, not a promise of throughput: the pacer
 * still starts at STREAM_INTERVAL_MIN_MS and only slows when send_ms or the
 * byte rate say the link is struggling. Raising it risks starving /status and
 * /spray on the same TCP path, which is a failure mode this file has hit
 * before -- verify both stay responsive while streaming. */
#define STREAM_BUDGET_BPS 122880u  /* 120 KB/s: leaves room for /status and /spray */
#define STREAM_BPS_STEP    4096u

/* Adaptive stream pacing. Owned by the stream task only (it is reset per
 * viewer in handler_stream), so no locking is needed. */
static uint32_t s_stream_interval_ms = STREAM_INTERVAL_MIN_MS;
static int consecutive_errors = 0;

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

    /* Expose the measured stream byte rate alongside fps. fps alone hides the
     * failure mode that matters: a low-fps stream caused by oversized frames
     * looks the same as one caused by a bad link, and the two have different
     * fixes (lower CAM_JPEG_QUALITY vs. improve Wi-Fi). */
    uint32_t stream_bps_snapshot = s_stream_bps;
    uint32_t free_sram = (uint32_t)esp_get_free_heap_size();
    /* fomo_result_t stores x,y as the box CENTROID; the dashboard overlay
     * draws from the top-left corner, so convert here. */
    float tlx = last.x - last.width / 2.0f;
    float tly = last.y - last.height / 2.0f;
    if (tlx < 0.0f) tlx = 0.0f;
    if (tly < 0.0f) tly = 0.0f;
    if (tlx + last.width > 1.0f) tlx = 1.0f - last.width;
    if (tly + last.height > 1.0f) tly = 1.0f - last.height;
    char buf[1280];
    /* vrf = the server-side YOLOv8 answer for the live frame, polled on a
     * fixed cadence so the dashboard overlay always has an independent
     * opinion. Display only: it never gates the relay, which stays on the
     * on-device FOMO class in detection_task(). */
    verify_result_t vr;
    detection_service_verify_result(&vr);

    /* Link telemetry. rssi is reported as the raw dBm when associated, and
     * as the sentinel below when not, so the dashboard can distinguish "weak
     * but connected" from "offline". wifiReconnects counts consecutive failed
     * association attempts and resets on success, so a rising value is an
     * early warning that the link is degrading. */
    int rssi = wifi_service_rssi();
    if (rssi == INT8_MIN) rssi = -128;

    int n = snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"heap\":%u,\"fps\":%.1f,\"streamBps\":%u,\"model\":\"%s\","
        "\"sprayActive\":%s,\"cooldownActive\":%s,"
        "\"lastConfidence\":%.4f,\"objectPresent\":%s,"
        "\"classId\":%d,\"box\":{\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f},"
        "\"vrf\":{\"valid\":%s,\"detected\":%s,\"conf\":%.4f,\"ageMs\":%u,"
        "\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f},"
        "\"wifiRssi\":%d,\"wifiConnected\":%s,\"wifiReconnects\":%u,"
        "\"verifyOk\":%s,\"verifyFailures\":%u,\"verifyOkAgeMs\":%llu,"
        "\"lastSprayAgoMs\":%llu,\"uptimeS\":%llu,"
        "\"inferenceMs\":%u,\"detections\":%u}",
        wifi_ip_str(), (unsigned)free_sram,
        (double)s_stream_fps,
        (unsigned)stream_bps_snapshot,
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
        rssi,
        wifi_service_is_connected() ? "true" : "false",
        (unsigned)wifi_service_reconnect_attempts(),
        /* Verifier leg health, so "VERIFY OFFLINE" can be attributed to the
         * server or to the board. verifyOkAgeMs is the age of the last ACCEPTED
         * answer; if it climbs far beyond the poll interval while verifyOk is
         * false, the poll leg itself has stalled rather than the server being
         * unreachable. -1 means no answer has ever been accepted. */
        detection_service_verify_ok() ? "true" : "false",
        (unsigned)detection_service_verify_failures(),
        detection_service_last_verify_ok_age_ms() == UINT64_MAX
            ? (unsigned long long)-1
            : (unsigned long long)detection_service_last_verify_ok_age_ms(),
        (unsigned long long)sp.last_spray_ago_ms,
        (unsigned long long)(esp_timer_get_time() / 1000000u),
        (unsigned)last.inference_ms,
        (unsigned)detection_service_detection_count());

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
        "{\"threshold\":%.2f,\"server_trigger_conf\":%.2f,"
        "\"cooldown_ms\":%u,\"spray_ms\":%u,"
        "\"relay_active_low\":%s,"
        "\"static_ip\":\"%s\",\"static_mask\":\"%s\",\"static_gw\":\"%s\","
        "\"verify_url\":\"%s\",\"dash_url\":\"%s\",\"dash_verify_url\":\"%s\"}",
        (double)cfg.threshold, (double)cfg.server_trigger_conf,
        (unsigned)cfg.cooldown_ms, (unsigned)cfg.spray_ms,
        cfg.relay_active_low ? "true" : "false",
        cfg.static_ip, cfg.static_mask, cfg.static_gw,
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

/* Escape a value for use inside a single-quoted HTML attribute. Covers the
 * five characters that can break out of value='...' or start markup. */
static void prv_html_escape(const char *in, char *out, size_t out_size)
{
    static const struct { char c; const char *rep; } MAP[] = {
        { '&',  "&amp;"  }, { '<',  "&lt;"   }, { '>',  "&gt;"   },
        { '\'', "&#39;"  }, { '"',  "&quot;" },
    };
    size_t o = 0;
    for (const char *r = in; *r && o + 1 < out_size; r++) {
        const char *rep = NULL;
        size_t rep_len = 1;
        for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
            if (*r == MAP[i].c) { rep = MAP[i].rep; rep_len = strlen(MAP[i].rep); break; }
        }
        if (o + rep_len + 1 > out_size) break;   /* truncate rather than overflow */
        if (rep) { memcpy(out + o, rep, rep_len); o += rep_len; }
        else     { out[o++] = *r; }
    }
    out[o] = '\0';
}

static int prv_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode in place. httpd_query_key_value() copies the raw bytes
 * between '=' and the next '&' without decoding, so a browser submitting an
 * SSID containing '&' sends "A%26B" and the device would store the escape
 * literally. Decoding here also turns '+' into a space, which is what form
 * encoding means for a space. Returns the new length. */
static size_t prv_url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r == '%' && prv_hexval(r[1]) >= 0 && prv_hexval(r[2]) >= 0) {
            *w++ = (char)((prv_hexval(r[1]) << 4) | prv_hexval(r[2]));
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return (size_t)(w - s);
}

/* Fetch a key from an urlencoded body and percent-decode it into out. */
static bool prv_get_decoded(const char *body, const char *key,
                            char *out, size_t out_size)
{
    char raw[270];
    if (httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) return false;
    if (prv_url_decode(raw) >= out_size) {
        ESP_LOGW(TAG, "%s too long after decoding, ignored", key);
        return false;
    }
    strncpy(out, raw, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
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
    if (prv_get_decoded(body, "wifi_ssid", val, sizeof(val)) && val[0]) {
        strncpy(cfg.wifi_ssid, val, sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    }
    if (prv_get_decoded(body, "wifi_pass", val, sizeof(val))) {
        strncpy(cfg.wifi_pass, val, sizeof(cfg.wifi_pass) - 1);
        cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
    }
    if (prv_get_decoded(body, "verify_url", val, sizeof(val)) && val[0]) {
        strncpy(cfg.verify_url, val, sizeof(cfg.verify_url) - 1);
        cfg.verify_url[sizeof(cfg.verify_url) - 1] = '\0';
    }
    if (prv_get_decoded(body, "dash_url", val, sizeof(val)) && val[0]) {
        strncpy(cfg.dash_url, val, sizeof(cfg.dash_url) - 1);
        cfg.dash_url[sizeof(cfg.dash_url) - 1] = '\0';
    }
    if (prv_get_decoded(body, "dash_verify_url", val, sizeof(val)) && val[0]) {
        strncpy(cfg.dash_verify_url, val, sizeof(cfg.dash_verify_url) - 1);
        cfg.dash_verify_url[sizeof(cfg.dash_verify_url) - 1] = '\0';
    }
    if (httpd_query_key_value(body, "threshold", val, sizeof(val)) == ESP_OK) {
        float t = strtof(val, NULL);
        if (t >= 0.0f && t <= 1.0f) cfg.threshold = t;
    }
    /* YOLOv8 confidence that triggers a spray. Separate from `threshold` above
     * because the server model and the on-device model are not calibrated
     * against each other. */
    if (httpd_query_key_value(body, "server_trigger_conf", val, sizeof(val)) == ESP_OK) {
        float t = strtof(val, NULL);
        if (t >= 0.0f && t <= 1.0f) cfg.server_trigger_conf = t;
    }
    if (httpd_query_key_value(body, "cooldown_ms", val, sizeof(val)) == ESP_OK) {
        cfg.cooldown_ms = (uint32_t)strtoul(val, NULL, 10);
    }
    if (httpd_query_key_value(body, "spray_ms", val, sizeof(val)) == ESP_OK) {
        cfg.spray_ms = (uint32_t)strtoul(val, NULL, 10);
    }
    /* Optional static addressing. All three must be supplied together; the
     * loader rejects a partial set so a half-configured address cannot strand
     * the board with no route to correct it. Send empty strings to return to
     * DHCP. */
    {
        const char *keys[3] = { "static_ip", "static_mask", "static_gw" };
        char *dst[3] = { cfg.static_ip, cfg.static_mask, cfg.static_gw };
        size_t sizes[3] = { sizeof(cfg.static_ip), sizeof(cfg.static_mask),
                            sizeof(cfg.static_gw) };
        int supplied = 0;
        bool valid = true;
        for (int i = 0; i < 3; i++) {
            if (httpd_query_key_value(body, keys[i], val, sizeof(val)) != ESP_OK) {
                continue;
            }
            supplied++;
            /* Validate before storing: a typo'd mask is worse than no mask,
             * because it silently breaks reachability. */
            if (val[0] != '\0' && esp_ip4addr_aton(val) == 0) {
                ESP_LOGW(TAG, "%s='%s' is not a dotted-quad IPv4; ignored",
                         keys[i], val);
                valid = false;
                continue;
            }
            strncpy(dst[i], val, sizes[i] - 1);
            dst[i][sizes[i] - 1] = '\0';
        }
        if (supplied == 3 && valid) {
            ESP_LOGI(TAG, "Static addressing requested; rebooting to apply");
        } else if (supplied > 0) {
            ESP_LOGW(TAG, "Static address needs ip+mask+gw together; keeping "
                          "previous setting");
        }
    }

    /* Relay IN polarity, as 1/0 or true/false. Accept the JSON-ish spelling so
     * this can be flipped with a plain curl without a rebuild -- the usual way
     * to discover it is: fire /spray and watch whether the relay actually
     * clicks. */
    if (prv_get_decoded(body, "relay_active_low", val, sizeof(val)) && val[0]) {
        if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0) {
            cfg.relay_active_low = true;
        } else if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0) {
            cfg.relay_active_low = false;
        } else {
            ESP_LOGW(TAG, "relay_active_low '%s' not understood, keeping %s",
                     val, cfg.relay_active_low ? "active-low" : "active-high");
        }
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

    char *page = malloc(4608);
    if (page == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }
    /* Values go into single-quoted HTML attributes, so escape them first. An
     * SSID containing & or < would otherwise truncate the attribute or inject
     * markup. IDF 5.5.5 has no HTML escape helper, so this covers the five
     * characters that matter inside an attribute value. */
    char esc_ssid[130], esc_pass[200], esc_verify[300], esc_dash[300], esc_dash_v[300];
    prv_html_escape(cfg.wifi_ssid, esc_ssid, sizeof(esc_ssid));
    prv_html_escape(cfg.wifi_pass, esc_pass, sizeof(esc_pass));
    prv_html_escape(cfg.verify_url, esc_verify, sizeof(esc_verify));
    prv_html_escape(cfg.dash_url, esc_dash, sizeof(esc_dash));
    prv_html_escape(cfg.dash_verify_url, esc_dash_v, sizeof(esc_dash_v));
    /* Addresses go into value='...' too, so escape them the same way. */
    char esc_static_ip[24], esc_static_mask[24], esc_static_gw[24];
    prv_html_escape(cfg.static_ip, esc_static_ip, sizeof(esc_static_ip));
    prv_html_escape(cfg.static_mask, esc_static_mask, sizeof(esc_static_mask));
    prv_html_escape(cfg.static_gw, esc_static_gw, sizeof(esc_static_gw));

    int n = snprintf(page, 4608,
        "<!doctype html><html><head><title>Feces Detector setup</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
        "margin:40px;line-height:1.6}label{display:block;margin-top:12px;font-size:14px}"
        "input,select{width:100%%;padding:8px;margin:6px 0;border-radius:6px;"
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
        "<label>Detection threshold (0..1) — spray dial</label><input type='number' step='0.01' "
        "name='threshold' value='%.2f' min='0' max='1'>"
        "<label>YOLOv8 trigger confidence (0..1) — server-gated spray</label>"
        "<input type='number' step='0.01' name='server_trigger_conf' value='%.2f' "
        "min='0' max='1'>"
        "<p style='font-size:13px;color:#94a3b8'>A server YOLOv8 box at or above "
        "this confidence sprays, same as the device FOMO class. Needs the verify "
        "server reachable.</p>"
        "<label>Static IP (optional)</label><input name='static_ip' value='%s' "
        "placeholder='leave blank for DHCP'>"
        "<label>Subnet mask</label><input name='static_mask' value='%s' "
        "placeholder='255.255.255.0'>"
        "<label>Gateway</label><input name='static_gw' value='%s' "
        "placeholder='192.168.1.1'>"
        "<p style='font-size:13px;color:#94a3b8'>All three together, or all blank. "
        "A static address stops the router handing out a new one, which would "
        "otherwise break the dashboard's saved link to this board.</p>"
        "<label>Cooldown ms</label><input type='number' step='1000' name='cooldown_ms' "
        "value='%u'>"
        "<label>Spray duration ms</label><input type='number' step='100' name='spray_ms' "
        "value='%u'>"
        "<label>Relay IN polarity</label><select name='relay_active_low'>"
        "<option value='1'%s>Active-LOW — IN pulled to GND fires (KY-019, SRD-05VDC)</option>"
        "<option value='0'%s>Active-HIGH — IN pulled to VCC fires</option></select>"
        "<p style='font-size:13px;color:#94a3b8'>Unsure? Fire "
        "<code>GET /spray?duration=3000</code>. If the LED and buzzer respond but "
        "the relay stays silent, this module is active-HIGH — switch it and "
        "reboot.</p>"
        "<button type='submit'>Save &amp; restart</button></form>"
        "</body></html>",
        esc_ssid, esc_pass, esc_verify, esc_dash, esc_dash_v,
        (double)cfg.threshold, (double)cfg.server_trigger_conf,
        esc_static_ip, esc_static_mask, esc_static_gw,
        (unsigned)cfg.cooldown_ms, (unsigned)cfg.spray_ms,
        cfg.relay_active_low ? " selected" : "",
        cfg.relay_active_low ? "" : " selected");
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
    ESP_LOGI(TAG, "stream client connected, starting at %u ms/frame",
             (unsigned)s_stream_interval_ms);

    while (true) {
        camera_fb_t *fb = camera_service_get_frame();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Measure how long the send actually takes. On a healthy link this is
         * single-digit milliseconds; on a stalled one it grows to hundreds,
         * because MJPEG rides TCP and a lost segment blocks the socket until it
         * retransmits. That duration is the only honest measure of how fast this
         * link can actually carry video, and it is what the pacing below uses.
         * Without it the task would keep pushing at the nominal rate into a link
         * that cannot drain it, which is what produced a frozen picture. */
        int64_t send_start_us = esp_timer_get_time();
        size_t fb_len = fb->len;

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

        int64_t send_ms = (esp_timer_get_time() - send_start_us) / 1000;

        /* A failed send is not treated as fatal. The old code broke out of the
         * loop immediately, so one lost chunk ended the whole stream and the
         * browser showed a permanently frozen image with no way to recover
         * short of reloading the page. Now only a definite disconnect ends it;
         * a slow or transient error just costs one frame. */
        if (res != ESP_OK) {
            /* httpd_resp_send_chunk returns ESP_FAIL when the client has gone
             * away, and ESP_ERR_HTTPD_RESP_SEND for a mid-flight write failure
             * where the socket is congested but the peer may still be there.
             * Only the definite disconnect ends the stream immediately; a send
             * error just costs a few frames and a pause. */
            bool fatal = (res == ESP_FAIL);
            consecutive_errors++;
            if (fatal || consecutive_errors > 20) {
                ESP_LOGW(TAG, "stream ending after %d send error(s), last=%s",
                         consecutive_errors, esp_err_to_name(res));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        consecutive_errors = 0;

        /* Account only frames that actually went out, so a stalled socket
         * cannot inflate the reported rate. */
        s_stream_frames++;
        s_stream_bytes += (uint32_t)fb_len;
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_stream_window_ms >= 1000) {
            int64_t span = now_ms - s_stream_window_ms;
            s_stream_fps = (float)s_stream_frames * 1000.0f / (float)span;
            s_stream_bps = (uint32_t)((uint64_t)s_stream_bytes * 1000u / (uint64_t)span);
            s_stream_frames = 0;
            s_stream_bytes = 0;
            s_stream_window_ms = now_ms;
        }

        /* Adaptive frame interval. Start at the historical 40 ms (~25 fps) and
         * back off toward 500 ms as sends get slower, recovering toward the
         * target once the link is healthy again. The floor keeps the view
         * watchable over a bad link instead of stalling completely; the ceiling
         * bounds how long the operator stares at a stale frame.
         *
         * Frame time on a healthy link is well under the interval, so this only
         * slows down when the link genuinely cannot keep up. */
        bool over_send  = send_ms > s_stream_interval_ms;
        bool over_budget = s_stream_bps > STREAM_BUDGET_BPS;

        if (over_send || over_budget) {
            s_stream_interval_ms += STREAM_INTERVAL_STEP_MS;
            if (s_stream_interval_ms > STREAM_INTERVAL_MAX_MS) {
                s_stream_interval_ms = STREAM_INTERVAL_MAX_MS;
            }
        } else if (send_ms * 2 < s_stream_interval_ms
                   && s_stream_bps < STREAM_BUDGET_BPS * 3 / 4) {
            /* Comfortably ahead of the link on BOTH signals: creep back up.
             * The byte-rate guard matters as much as the send-time one --
             * without it the pacer recovers to a rate that refills the socket
             * and starves /status within a few seconds. */
            s_stream_interval_ms -= STREAM_INTERVAL_STEP_MS;
            if (s_stream_interval_ms < STREAM_INTERVAL_MIN_MS) {
                s_stream_interval_ms = STREAM_INTERVAL_MIN_MS;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_stream_interval_ms));
    }
    ESP_LOGI(TAG, "stream client gone; final interval %u ms/frame",
             (unsigned)s_stream_interval_ms);
    camera_streaming_stop();
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t handler_stream(httpd_req_t *req)
{
    prv_cors(req);
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    camera_streaming_start();
    /* Start a fresh measurement window per viewer, so /status reports the rate
     * of the stream actually being consumed. */
    s_stream_frames = 0;
    s_stream_bytes = 0;
    s_stream_bps = 0;
    s_stream_window_ms = esp_timer_get_time() / 1000;
    /* Per-viewer pacing state, so one slow client cannot leave the next viewer
     * inheriting a permanently throttled frame interval. */
    s_stream_interval_ms = STREAM_INTERVAL_MIN_MS;
    consecutive_errors = 0;

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
