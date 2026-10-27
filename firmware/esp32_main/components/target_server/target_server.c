/*
 * The main board's HTTP server.
 *
 * The important endpoint is POST /target: it receives a detection from the
 * camera board, turns the coordinates into servo angles, and starts a
 * cleaning cycle if -- and only if -- every interlock allows it.
 *
 * The reply tells the camera board exactly what happened, so "sent and
 * refused" is distinguishable from "never arrived". The camera board shows
 * that on its dashboard.
 */
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "cleaning_cycle.h"
#include "geometry.h"
#include "geometry_selftest.h"
#include "main_config.h"
#include "servo_driver.h"
#include "target_server.h"
#include "water_sensor.h"

static const char *TAG = "target";

/* The camera board's detection ids. Used to refuse a repeat of a detection
 * that has already been acted on. */
static uint32_t s_last_detection_id = 0;
static int64_t  s_last_detection_ms = 0;

/* How old a message may be before it is refused. A clock-skew guard, not a
 * scheduler: the two boards' clocks can differ. */
#define MAX_TARGET_AGE_MS 30000

/* ---------------- tiny JSON helpers ----------------
 * The messages are small, flat and come from a known peer on the LAN, so a
 * scanner is enough. A JSON library would cost more flash than the whole
 * rest of this board. */

static const char *prv_find_key(const char *json, const char *key)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return NULL;
    return p + strlen(needle);
}

static bool prv_get_float(const char *json, const char *key, float *out)
{
    const char *p = prv_find_key(json, key);
    if (p == NULL) return false;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = (float)v;
    return true;
}

static bool prv_get_int(const char *json, const char *key, long *out)
{
    const char *p = prv_find_key(json, key);
    if (p == NULL) return false;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool prv_get_bool(const char *json, const char *key, bool *out)
{
    const char *p = prv_find_key(json, key);
    if (p == NULL) return false;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool prv_get_str(const char *json, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = prv_find_key(json, key);
    if (p == NULL || *p != '"') return false;
    p++;
    size_t i = 0;
    while (p[i] != '"' && p[i] != '\0' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

/* Walk the "boxes" array and keep the highest-confidence one. The nozzle is
 * aimed at ONE spot at a time, so a frame holding several spots is cleaned
 * one after another -- the next message brings the next spot.
 *
 * The callback-style scan is done inline rather than with a helper so the
 * object being parsed is a copy, never a pointer into the request body. */
typedef struct {
    float x, y, w, h;
    float conf;
    bool  ok;
} prv_box_t;

static void prv_box_parse(const char *obj, prv_box_t *b)
{
    b->ok = false;
    if (!prv_get_float(obj, "x", &b->x)) return;
    if (!prv_get_float(obj, "y", &b->y)) return;
    if (!prv_get_float(obj, "w", &b->w)) return;
    if (!prv_get_float(obj, "h", &b->h)) return;
    prv_get_float(obj, "conf", &b->conf);
    b->ok = true;
}

static bool prv_find_best_box(const char *json, prv_box_t *best)
{
    best->ok = false;
    best->conf = 0.0f;

    const char *p = strstr(json, "\"boxes\":");
    if (p == NULL) return false;
    p = strchr(p, '[');
    if (p == NULL) return false;

    int depth = 0;
    const char *start = NULL;
    for (const char *q = p + 1; *q != '\0'; q++) {
        if (*q == '{') {
            if (depth == 0) start = q;
            depth++;
        } else if (*q == '}') {
            depth--;
            if (depth == 0 && start != NULL) {
                char obj[192];
                size_t n = (size_t)(q - start);
                if (n >= sizeof(obj)) n = sizeof(obj) - 1;
                memcpy(obj, start, n);
                obj[n] = '\0';
                prv_box_t b = { 0 };
                prv_box_parse(obj, &b);
                if (b.ok && b.conf > best->conf) *best = b;
            }
        } else if (*q == ']' && depth == 0) {
            break;
        }
    }
    return best->ok;
}

/* ---------------- POST /target ---------------- */

static esp_err_t prv_reply(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* The frame dimensions live inside a nested object:
 *   "frame":{"width":640,"height":480}
 *
 * A plain search for "\"width\":" would find it -- but it would also find some
 * unrelated width later in the document, and it would stop being obvious
 * which one it picked. Scoping the search to the frame object is explicit
 * and cannot drift. */
static void prv_get_frame_size(const char *json, long *w, long *h)
{
    *w = 0;
    *h = 0;
    const char *p = strstr(json, "\"frame\":");
    if (p == NULL) return;
    /* Only look inside the frame object: stop at the closing brace. */
    const char *end = strchr(p, '}');
    if (end == NULL) return;

    char needle[24];
    snprintf(needle, sizeof(needle), "\"width\":");
    const char *q = strstr(p, needle);
    if (q != NULL && q < end) {
        *w = strtol(q + strlen(needle), NULL, 10);
    }
    snprintf(needle, sizeof(needle), "\"height\":");
    q = strstr(p, needle);
    if (q != NULL && q < end) {
        *h = strtol(q + strlen(needle), NULL, 10);
    }
}

static esp_err_t handler_target(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - (size_t)total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';
    if (total == 0) return prv_reply(req, "{\"ok\":false,\"error\":\"empty body\"}");

    long version = 0;
    if (!prv_get_int(body, "v", &version) || version != 1) {
        ESP_LOGW(TAG, "unknown protocol version %ld", version);
        return prv_reply(req, "{\"ok\":false,\"error\":\"bad_version\"}");
    }

    long det_id = 0;
    prv_get_int(body, "detection_id", &det_id);
    long ts_ms = 0;
    prv_get_int(body, "ts_ms", &ts_ms);

    long frame_w = 0, frame_h = 0;
    prv_get_frame_size(body, &frame_w, &frame_h);

    /* Duplicate guard: the SAME detection id is ignored. A NEW id is
     * accepted even if it is lower, because the camera board restarts and
     * its counter resets. */
    int64_t now = esp_timer_get_time() / 1000;
    if (det_id > 0 && (uint32_t)det_id == s_last_detection_id) {
        ESP_LOGI(TAG, "duplicate detection %ld ignored", det_id);
        return prv_reply(req, "{\"ok\":true,\"accepted\":false,\"reason\":\"duplicate\"}");
    }
    /* Stale guard. The clocks are two different boards, so this is a sanity
     * bound, not a scheduler. */
    if (ts_ms > 0) {
        int64_t age = (int64_t)(now - ts_ms);
        if (age > MAX_TARGET_AGE_MS || age < -MAX_TARGET_AGE_MS) {
            ESP_LOGW(TAG, "stale detection %ld (age %lld ms)", det_id,
                     (long long)age);
            return prv_reply(req, "{\"ok\":true,\"accepted\":false,\"reason\":\"stale_id\"}");
        }
    }

    /* Find the best box: the highest confidence. The nozzle is aimed at one
     * spot at a time, so a frame with several spots is cleaned one after
     * another -- not all at once. */
    prv_box_t best = { 0 };
    if (!prv_find_best_box(body, &best)) {
        return prv_reply(req, "{\"ok\":true,\"accepted\":false,\"reason\":\"no_target\"}");
    }
    if (frame_w <= 0 || frame_h <= 0) {
        /* Without the frame size the coordinates cannot be placed. */
        return prv_reply(req, "{\"ok\":true,\"accepted\":false,\"reason\":\"no_target\"}");
    }

    main_config_t cfg;
    main_config_load(&cfg);

    geometry_cal_t cal = {
        .cam_x = cfg.cam_x, .cam_y = cfg.cam_y, .cam_height = cfg.cam_height,
        .cam_yaw_deg = cfg.cam_yaw_deg, .cam_pitch_deg = cfg.cam_pitch_deg,
        .focal_px = cfg.focal_px,
        .nozzle_x = cfg.nozzle_x, .nozzle_y = cfg.nozzle_y,
        .nozzle_height = cfg.nozzle_height,
        .pan_min = cfg.pan_min, .pan_max = cfg.pan_max,
        .tilt_min = cfg.tilt_min, .tilt_max = cfg.tilt_max,
    };

    geometry_result_t g;
    if (!geometry_solve(&cal, best.x, best.y, best.w, best.h,
                        (uint16_t)frame_w, (uint16_t)frame_h, &g)) {
        ESP_LOGW(TAG, "detection %ld did not solve to an angle", det_id);
        return prv_reply(req, "{\"ok\":true,\"accepted\":false,\"reason\":\"solve_failed\"}");
    }

    if (g.clamped) {
        ESP_LOGW(TAG, "detection %ld hit a servo limit: pan=%.1f tilt=%.1f",
                 det_id, (double)g.pan, (double)g.tilt);
    }

    clean_reason_t why = cleaning_cycle_start(g.pan, g.tilt);
    if (why != CLEAN_REASON_OK) {
        /* The angles are echoed back even on a refusal: the operator can see
         * where the nozzle WOULD have pointed, which is the difference
         * between "aimed wrong" and "refused for lack of water". */
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"accepted\":false,\"pan\":%.1f,\"tilt\":%.1f,"
                 "\"reason\":\"%s\"}",
                 (double)g.pan, (double)g.tilt, cleaning_cycle_reason_str(why));
        return prv_reply(req, buf);
    }

    s_last_detection_id = (uint32_t)det_id;
    s_last_detection_ms = now;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"accepted\":true,\"pan\":%.1f,\"tilt\":%.1f,"
             "\"reason\":\"\"}",
             (double)g.pan, (double)g.tilt);
    ESP_LOGI(TAG, "detection %ld -> accepted, pan=%.1f tilt=%.1f",
             det_id, (double)g.pan, (double)g.tilt);
    return prv_reply(req, buf);
}

/* ---------------- GET /status ---------------- */

static esp_err_t handler_status(httpd_req_t *req)
{
    main_config_t cfg;
    main_config_load(&cfg);
    water_reading_t w = water_sensor_read();

    char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"state\":\"%s\",\"enabled\":%s,"
        "\"sprayActive\":%s,\"busy\":%s,"
        "\"servo\":{\"pan\":%.1f,\"tilt\":%.1f,"
        "\"targetPan\":%.1f,\"targetTilt\":%.1f},"
        "\"water\":{\"pct\":%.1f,\"valid\":%s,\"floatOk\":%s,\"floatUsed\":%s,"
        "\"raw\":%u,\"minPct\":%.1f},"
        "\"cooldownMs\":%u,\"sprayMs\":%u,\"sprays\":%u,"
        "\"lastDetectionId\":%u,\"lastReason\":\"%s\"}",
        "0.0.0.0",
        cleaning_cycle_state() == CLEAN_STATE_IDLE ? "idle" :
        (cleaning_cycle_state() == CLEAN_STATE_AIMING ? "aiming" :
        (cleaning_cycle_state() == CLEAN_STATE_SPRAYING ? "spraying" : "cooldown")),
        cfg.enabled ? "true" : "false",
        cleaning_cycle_state() == CLEAN_STATE_SPRAYING ? "true" : "false",
        cleaning_cycle_is_busy() ? "true" : "false",
        (double)servo_driver_angle(SERVO_PAN),
        (double)servo_driver_angle(SERVO_TILT),
        (double)cleaning_cycle_target_pan(),
        (double)cleaning_cycle_target_tilt(),
        (double)w.pct,
        w.valid ? "true" : "false",
        w.float_ok ? "true" : "false",
        w.float_used ? "true" : "false",
        (unsigned)w.raw,
        (double)cfg.water_min_pct,
        (unsigned)cleaning_cycle_cooldown_remaining_ms(),
        (unsigned)cfg.spray_ms,
        (unsigned)cleaning_cycle_spray_count(),
        (unsigned)s_last_detection_id,
        cleaning_cycle_reason_str(cleaning_cycle_last_reason_code()));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* ---------------- GET /servo ---------------- */

static esp_err_t handler_servo(httpd_req_t *req)
{
    char q[32];
    float pan = 0.0f, tilt = 0.0f;
    bool have_pan = false, have_tilt = false;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char buf[160];
        char *tok = strtok(q, "&");
        while (tok != NULL) {
            if (strncmp(tok, "pan=", 4) == 0) {
                pan = strtof(tok + 4, NULL);
                have_pan = true;
            } else if (strncmp(tok, "tilt=", 5) == 0) {
                tilt = strtof(tok + 5, NULL);
                have_tilt = true;
            }
            (void)buf;
            tok = strtok(NULL, "&");
        }
    }
    if (!have_pan && !have_tilt) {
        return prv_reply(req, "{\"ok\":false,\"error\":\"need pan and/or tilt\"}");
    }

    /* A manual servo test NEVER touches the pump. That separation is the
     * point of the staged testing: aim first, with the water still off. */
    if (have_pan) servo_driver_set_angle(SERVO_PAN, pan);
    if (have_tilt) servo_driver_set_angle(SERVO_TILT, tilt);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"pan\":%.1f,\"tilt\":%.1f,\"pump\":\"off\"}",
             (double)servo_driver_angle(SERVO_PAN),
             (double)servo_driver_angle(SERVO_TILT));
    return prv_reply(req, buf);
}

/* ---------------- GET /pump ---------------- */

static esp_err_t handler_pump(httpd_req_t *req)
{
    char q[32];
    uint32_t duration = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (strncmp(q, "duration=", 9) == 0) {
            duration = (uint32_t)strtoul(q + 9, NULL, 10);
        }
    }

    clean_reason_t why = cleaning_cycle_manual_pump(duration);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"reason\":\"%s\"}",
             why == CLEAN_REASON_OK ? "true" : "false",
             cleaning_cycle_reason_str(why));
    return prv_reply(req, buf);
}

/* ---------------- GET /config ---------------- */

static esp_err_t handler_config(httpd_req_t *req)
{
    main_config_t c;
    main_config_load(&c);
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{\"cam_x\":%.1f,\"cam_y\":%.1f,\"cam_height\":%.1f,"
        "\"cam_yaw\":%.1f,\"cam_pitch\":%.1f,\"focal_px\":%.1f,"
        "\"nozzle_x\":%.1f,\"nozzle_y\":%.1f,\"nozzle_h\":%.1f,"
        "\"pan_min\":%.1f,\"pan_max\":%.1f,\"tilt_min\":%.1f,\"tilt_max\":%.1f,"
        "\"pulse_min\":%u,\"pulse_max\":%u,"
        "\"water_min\":%.1f,\"spray_ms\":%u,\"cooldown_ms\":%u,"
        "\"relay_active_low\":%s,\"enabled\":%s,\"float_sw\":%s}",
        (double)c.cam_x, (double)c.cam_y, (double)c.cam_height,
        (double)c.cam_yaw_deg, (double)c.cam_pitch_deg, (double)c.focal_px,
        (double)c.nozzle_x, (double)c.nozzle_y, (double)c.nozzle_height,
        (double)c.pan_min, (double)c.pan_max,
        (double)c.tilt_min, (double)c.tilt_max,
        (unsigned)c.servo_pulse_min_us, (unsigned)c.servo_pulse_max_us,
        (double)c.water_min_pct, (unsigned)c.spray_ms, (unsigned)c.cooldown_ms,
        c.relay_active_low ? "true" : "false",
        c.enabled ? "true" : "false",
        c.use_float_switch ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* ---------------- POST /config ---------------- */

static esp_err_t handler_config_post(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - (size_t)total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';

    main_config_t c;
    main_config_load(&c);

    char v[64];
    static const char *fkeys[] = {
        "cam_x", "cam_y", "cam_height", "cam_yaw", "cam_pitch", "focal_px",
        "nozzle_x", "nozzle_y", "nozzle_h", "pan_min", "pan_max",
        "tilt_min", "tilt_max", "water_min",
    };
    float *fdst[] = {
        &c.cam_x, &c.cam_y, &c.cam_height, &c.cam_yaw_deg, &c.cam_pitch_deg,
        &c.focal_px, &c.nozzle_x, &c.nozzle_y, &c.nozzle_height,
        &c.pan_min, &c.pan_max, &c.tilt_min, &c.tilt_max, &c.water_min_pct,
    };
    for (size_t i = 0; i < sizeof(fkeys) / sizeof(fkeys[0]); i++) {
        if (httpd_query_key_value(body, fkeys[i], v, sizeof(v)) == ESP_OK) {
            *fdst[i] = strtof(v, NULL);
        }
    }
    if (httpd_query_key_value(body, "pulse_min", v, sizeof(v)) == ESP_OK) {
        c.servo_pulse_min_us = (uint32_t)strtoul(v, NULL, 10);
    }
    if (httpd_query_key_value(body, "pulse_max", v, sizeof(v)) == ESP_OK) {
        c.servo_pulse_max_us = (uint32_t)strtoul(v, NULL, 10);
    }
    if (httpd_query_key_value(body, "spray_ms", v, sizeof(v)) == ESP_OK) {
        c.spray_ms = (uint32_t)strtoul(v, NULL, 10);
    }
    if (httpd_query_key_value(body, "cooldown_ms", v, sizeof(v)) == ESP_OK) {
        c.cooldown_ms = (uint32_t)strtoul(v, NULL, 10);
    }
    if (httpd_query_key_value(body, "relay_active_low", v, sizeof(v)) == ESP_OK) {
        c.relay_active_low = (strcmp(v, "0") != 0);
    }
    if (httpd_query_key_value(body, "enabled", v, sizeof(v)) == ESP_OK) {
        c.enabled = (strcmp(v, "0") != 0);
    }
    if (httpd_query_key_value(body, "float_sw", v, sizeof(v)) == ESP_OK) {
        c.use_float_switch = (strcmp(v, "0") != 0);
    }
    if (httpd_query_key_value(body, "wifi_ssid", v, sizeof(v)) == ESP_OK) {
        /* Explicit width: the field is shorter than the input buffer, and
         * truncating an SSID to fit is better than refusing to store it. */
        snprintf(c.wifi_ssid, sizeof(c.wifi_ssid), "%.32s", v);
    }
    if (httpd_query_key_value(body, "wifi_pass", v, sizeof(v)) == ESP_OK) {
        snprintf(c.wifi_pass, sizeof(c.wifi_pass), "%.64s", v);
    }

    main_config_save(&c);
    ESP_LOGI(TAG, "config updated; restarting to apply");

    prv_reply(req, "{\"ok\":true,\"saved\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ---------------- GET /setup ---------------- */

static esp_err_t handler_setup(httpd_req_t *req)
{
    main_config_t c;
    main_config_load(&c);
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "<!doctype html><html><head><title>Actuator setup</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
        "margin:40px;line-height:1.5}label{display:block;margin-top:10px;"
        "font-size:14px}input{width:100%%;padding:8px;margin:4px 0;"
        "border-radius:6px;border:1px solid #334155;background:#1e293b;"
        "color:#e2e8f0;box-sizing:border-box}"
        "button{padding:10px 18px;border:0;border-radius:6px;background:#34d399;"
        "color:#064e3b;font-weight:600;margin-top:16px}</style></head><body>"
        "<h1>Actuator board setup</h1>"
        "<p>All lengths in millimetres, all angles in degrees. These are "
        "MEASURED numbers: camera height and offset from the pen centre, the "
        "camera's downward tilt, the nozzle mount, and the servo's travel. "
        "The pen floor is z=0; the camera looks toward +Y when yaw is 0.</p>"
        "<form method='post' action='/config'>"
        "<label>Wi-Fi SSID (the same network the camera board is on)</label>"
        "<input name='wifi_ssid' value='%.32s' maxlength='32'>"
        "<label>Wi-Fi password</label>"
        "<input name='wifi_pass' value='%.64s' maxlength='64'>"
        "<label>Camera X</label><input name='cam_x' value='%.1f'>"
        "<label>Camera Y (from pen centre)</label><input name='cam_y' value='%.1f'>"
        "<label>Camera height above floor</label><input name='cam_height' value='%.1f'>"
        "<label>Camera yaw</label><input name='cam_yaw' value='%.1f'>"
        "<label>Camera pitch (below horizontal)</label><input name='cam_pitch' value='%.1f'>"
        "<label>Focal length (px)</label><input name='focal_px' value='%.1f'>"
        "<label>Nozzle X</label><input name='nozzle_x' value='%.1f'>"
        "<label>Nozzle Y</label><input name='nozzle_y' value='%.1f'>"
        "<label>Nozzle height</label><input name='nozzle_h' value='%.1f'>"
        "<label>Pan min</label><input name='pan_min' value='%.1f'>"
        "<label>Pan max</label><input name='pan_max' value='%.1f'>"
        "<label>Tilt min</label><input name='tilt_min' value='%.1f'>"
        "<label>Tilt max</label><input name='tilt_max' value='%.1f'>"
        "<label>Servo pulse min (us)</label><input name='pulse_min' value='%u'>"
        "<label>Servo pulse max (us)</label><input name='pulse_max' value='%u'>"
        "<label>Water minimum %%</label><input name='water_min' value='%.1f'>"
        "<label>Spray duration ms</label><input name='spray_ms' value='%u'>"
        "<label>Cooldown ms</label><input name='cooldown_ms' value='%u'>"
        "<label>Relay IN polarity</label><select name='relay_active_low'>"
        "<option value='1'%s>Active-LOW</option>"
        "<option value='0'%s>Active-HIGH</option></select>"
        "<label>Automatic cleaning</label><select name='enabled'>"
        "<option value='1'%s>Enabled</option>"
        "<option value='0'%s>Disabled</option></select>"
        "<label>Float switch fitted</label><select name='float_sw'>"
        "<option value='1'%s>Yes</option>"
        "<option value='0'%s>No</option></select>"
        "<button type='submit'>Save &amp; restart</button></form>"
        "</body></html>",
        c.wifi_ssid, c.wifi_pass,
        (double)c.cam_x, (double)c.cam_y, (double)c.cam_height,
        (double)c.cam_yaw_deg, (double)c.cam_pitch_deg, (double)c.focal_px,
        (double)c.nozzle_x, (double)c.nozzle_y, (double)c.nozzle_height,
        (double)c.pan_min, (double)c.pan_max,
        (double)c.tilt_min, (double)c.tilt_max,
        (unsigned)c.servo_pulse_min_us, (unsigned)c.servo_pulse_max_us,
        (double)c.water_min_pct, (unsigned)c.spray_ms, (unsigned)c.cooldown_ms,
        c.relay_active_low ? " selected" : "",
        c.relay_active_low ? "" : " selected",
        c.enabled ? " selected" : "",
        c.enabled ? "" : " selected",
        c.use_float_switch ? " selected" : "",
        c.use_float_switch ? "" : " selected");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, n);
}

/* ---------------- GET /selftest ---------------- */

/* The geometry solver is the part most likely to be wrong, so it can be
 * checked on the board itself. This is what to run first when the nozzle
 * aims somewhere unexpected: it separates "the maths is wrong" from "the
 * calibration numbers are wrong". */
static esp_err_t handler_selftest(httpd_req_t *req)
{
    char buf[512];
    geometry_selftest(buf, sizeof(buf));
    ESP_LOGI(TAG, "selftest: %s", buf);
    return prv_reply(req, buf);
}

/* ---------------- server ---------------- */

esp_err_t target_server_start(const target_server_opts_t *opts)
{
    httpd_config_t hc = {
        .server_port = opts->http_port,
        .max_uri_handlers = 8,
        .stack_size = 8192,
        .lru_purge_enable = true,
        .ctrl_port = 32768,
    };
    httpd_handle_t srv = NULL;
    esp_err_t e = httpd_start(&srv, &hc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/target",   .method = HTTP_POST, .handler = handler_target },
        { .uri = "/status",   .method = HTTP_GET,  .handler = handler_status },
        { .uri = "/servo",    .method = HTTP_GET,  .handler = handler_servo },
        { .uri = "/pump",     .method = HTTP_GET,  .handler = handler_pump },
        { .uri = "/config",   .method = HTTP_GET,  .handler = handler_config },
        { .uri = "/config",   .method = HTTP_POST, .handler = handler_config_post },
        { .uri = "/setup",    .method = HTTP_GET,  .handler = handler_setup },
        { .uri = "/selftest", .method = HTTP_GET,  .handler = handler_selftest },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        e = httpd_register_uri_handler(srv, &uris[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(e));
        }
    }

    ESP_LOGI(TAG, "target server on port %u (POST /target)", (unsigned)opts->http_port);
    return ESP_OK;
}
