/*
 * Continuous on-device detection.
 *
 * Runs a dedicated FreeRTOS task that:
 *   1. grabs a camera frame,
 *   2. runs the Edge Impulse FOMO inference,
 *   3. updates the relay/spray state (feces class only),
 *   4. reports detections to the CodeIgniter dashboard and pushes the JPEG
 *      to the Python YOLOv8 verification server for cross-checking, then
 *      writes the YOLOv8 confidence back onto the dashboard row (3-leg:
 *      insert -> verify -> mark_verified).
 *
 * "Whenever technically feasible" is honoured here: the FOMO model is small
 * enough to run fully on the ESP32-S3; YOLOv8 runs on the server because it
 * is not feasible on the MCU.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_camera.h"

#include "app_config.h"
#include "camera_service.h"
#include "inference_service.h"
#include "spray_controller.h"
#include "detection_service.h"

static const char *TAG = "detect";

/* Latest verifier result, shared between the detection task (writer) and the
 * httpd /status handler (reader). */
static verify_result_t s_verify = { 0 };
static int64_t s_verify_ms = 0;
static SemaphoreHandle_t s_verify_mux = NULL;

/* Verifier reachability, written only by the detect task. s_verify_ok is false
 * once a poll fails, and s_verify_retry_ms gates the next attempt. */
static bool s_verify_ok = true;
static int64_t s_verify_retry_ms = 0;

/* Feces detections acted on this boot (see detection_service_detection_count).
 * Bumped by the detection task only; read by the /status handler. A uint32_t
 * counter and a single writer make the torn-read window irrelevant in practice,
 * and the value is diagnostic, not control-critical. */
static volatile uint32_t s_detection_count = 0;

/* How often the live frame is re-checked by the server-side YOLOv8 model.
 * Independent of the on-device model so the dashboard overlay keeps working
 * while FOMO is still the pre-export stub. */
#define VERIFY_POLL_MS 2000

/* After a failed poll the verifier is presumed down and skipped for this long.
 * The poll runs inline in the detect loop, so retrying every 2 s against a
 * closed port would spend most of the loop waiting on TCP timeouts. */
#define VERIFY_BACKOFF_MS 30000

/* Find "\"key\":" then parse the following number. Used on trusted LAN JSON. */
static long prv_parse_int_key(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return 0;
    return strtol(p + strlen(needle), NULL, 10);
}

static float prv_parse_float_key(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return 0.0f;
    return strtof(p + strlen(needle), NULL);
}

/* Parse the normalized box the verifier publishes as
 *   "box_norm":{"x":0.1,"y":0.2,"w":0.3,"h":0.4}
 * Returns false when the verifier answered with "box_norm":null. */
static bool prv_parse_box_norm(const char *json, float *x, float *y,
                               float *w, float *h)
{
    const char *p = strstr(json, "\"box_norm\":");
    if (p == NULL) return false;
    p = strchr(p, '{');
    if (p == NULL) return false;
    /* parse within this object only, so a later "x" elsewhere cannot be picked */
    char obj[128];
    size_t i = 0;
    while (p[i] != '}' && p[i] != '\0' && i + 1 < sizeof(obj)) {
        obj[i] = p[i];
        i++;
    }
    obj[i] = '}';
    obj[i + 1] = '\0';
    *x = prv_parse_float_key(obj, "x");
    *y = prv_parse_float_key(obj, "y");
    *w = prv_parse_float_key(obj, "w");
    *h = prv_parse_float_key(obj, "h");
    return (*w > 0.001f && *h > 0.001f);
}

/* Parse a JSON boolean: "\"key\":" followed by true/false. */
static bool prv_parse_bool_key(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return false;
    return strncmp(p + strlen(needle), "true", 4) == 0;
}

/* esp_http_client_perform() runs to completion in blocking mode: it drains the
 * whole response body itself and then clears the cached buffer, so a following
 * esp_http_client_read() always returns 0. The body therefore has to be
 * captured from the HTTP_EVENT_ON_DATA event while perform() runs.
 * The server puts the fields this firmware needs first, so a truncated tail is
 * harmless. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} body_sink_t;

static esp_err_t prv_body_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    body_sink_t *sink = (body_sink_t *)evt->user_data;
    if (sink == NULL || sink->buf == NULL || sink->cap < 2) return ESP_OK;
    size_t room = sink->cap - 1 - sink->len;
    size_t n = (size_t)evt->data_len;
    if (n > room) n = room;
    if (n > 0) {
        memcpy(sink->buf + sink->len, evt->data, n);
        sink->len += n;
        sink->buf[sink->len] = '\0';
    }
    return ESP_OK;
}

/* esp_http_client_handle_t that will accumulate its reply into buf. The sink
 * must outlive the client, so callers keep both on the stack. */
static esp_http_client_handle_t prv_client_init(body_sink_t *sink, char *buf,
                                                size_t size, const char *url,
                                                int timeout_ms)
{
    sink->buf = buf;
    sink->cap = size;
    sink->len = 0;
    buf[0] = '\0';

    esp_http_client_config_t cc = {
        .url = url,
        .timeout_ms = timeout_ms,
        .skip_cert_common_name_check = true,
        .user_data = sink,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (c) esp_http_client_set_event_handler(c, prv_body_cb);
    return c;
}

/* Leg 1 — POST the detection to the dashboard. Returns the new row id, or 0. */
static long prv_post_dashboard(const app_config_t *cfg, float conf,
                               bool triggered, uint32_t spray_ms)
{
    char url[512];
    snprintf(url, sizeof(url),
        "%s?confidence=%.4f&triggered=%d&spray_ms=%u&esp=esp32s3&model=%s&notes=%s",
        cfg->dash_url, (double)conf, triggered ? 1 : 0, (unsigned)spray_ms,
        edge_impulse_model_tag(),
        triggered ? "auto+spray" : "cooldown+detected");

    esp_http_client_handle_t c;
    char body[128] = "";
    body_sink_t sink;
    c = prv_client_init(&sink, body, sizeof(body), url, 5000);
    if (!c) return 0;

    long id = 0;
    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Dashboard log failed: %s", esp_err_to_name(e));
    } else if (sink.len > 0) {
        id = prv_parse_int_key(body, "id");
        if (id <= 0) id = 0;
    }
    esp_http_client_cleanup(c);
    return id;
}

/* Leg 2 — POST the JPEG to the YOLOv8 verifier. Returns the best feces
 * confidence (>=0), or -1 if the verify leg failed entirely. */
static float prv_post_verify(const app_config_t *cfg, const uint8_t *jpeg,
                             size_t len, float fomo_conf)
{
    if (jpeg == NULL || len == 0) return -1.0f;

    esp_http_client_handle_t c;
    char body[512] = "";
    body_sink_t sink;
    c = prv_client_init(&sink, body, sizeof(body), cfg->verify_url, 15000);
    if (!c) return -1.0f;

    char fomo_hdr[16];
    snprintf(fomo_hdr, sizeof(fomo_hdr), "%.4f", (double)fomo_conf);

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "X-Fomo-Conf", fomo_hdr);
    esp_http_client_set_post_field(c, (const char *)jpeg, len);

    float yolo_conf = -1.0f;
    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "YOLOv8 verify POST failed: %s", esp_err_to_name(e));
    } else if (sink.len > 0) {
        /* Only report a verification when YOLOv8 actually saw a feces box.
         * uclass "none"/"pig" means yolo_conf should not light the badge. */
        if (strstr(body, "\"uclass\":\"feces\"") != NULL) {
            yolo_conf = prv_parse_float_key(body, "yolo_conf");
        }
    }
    esp_http_client_cleanup(c);
    return yolo_conf;
}

/* Leg 3 — write the YOLOv8 confidence back onto the dashboard row so the
 * "YOLOv8 verified" badge on the history/dashboard pages lights up. */
static void prv_post_verify_result(const app_config_t *cfg, long id, float yolo_conf)
{
    char url[512];
    snprintf(url, sizeof(url), "%s?detection_id=%ld&yolo_conf=%.4f",
             cfg->dash_verify_url, id, (double)yolo_conf);

    esp_http_client_config_t cc = {
        .url = url,
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (c) {
        esp_err_t e = esp_http_client_perform(c);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "YOLOv8->dashboard report failed: %s", esp_err_to_name(e));
        }
        esp_http_client_cleanup(c);
    }
}

/* Poll the server-side YOLOv8 model with the current frame and publish the
 * answer for the dashboard overlay. This runs on a fixed cadence regardless of
 * the on-device model, because the FOMO build is still the pre-export stub and
 * therefore never classifies anything. Display only: it must never gate the
 * relay, which stays on the FOMO class in detection_task(). */
static void prv_poll_verify(const app_config_t *cfg, const uint8_t *jpeg,
                            size_t len, float fomo_conf)
{
    if (jpeg == NULL || len == 0) return;

    esp_http_client_handle_t c;
    char body[1280] = "";
    body_sink_t sink;
    /* 1.5 s, not 8 s: this runs inline in the detect loop, so a dead verifier
     * cost a full 8 s timeout every other cycle. A slow answer is worthless
     * here anyway - the overlay is refreshed 2 s from now regardless. */
    c = prv_client_init(&sink, body, sizeof(body), cfg->verify_url, 1500);
    if (!c) return;

    char fomo_hdr[16];
    snprintf(fomo_hdr, sizeof(fomo_hdr), "%.4f", (double)fomo_conf);

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "X-Fomo-Conf", fomo_hdr);
    esp_http_client_set_post_field(c, (const char *)jpeg, len);

    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "verify poll failed: %s", esp_err_to_name(e));
        /* Back off so an offline verifier costs one 1.5 s timeout per
         * VERIFY_BACKOFF_MS instead of stalling every 2 s cycle. */
        s_verify_ok = false;
        s_verify_retry_ms = esp_timer_get_time() / 1000 + VERIFY_BACKOFF_MS;
    } else {
        /* esp_http_client_perform() returns ESP_OK for 4xx/5xx too, so without
         * this check a rejected frame (the server answers 400 for an empty or
         * undecodable JPEG) would be published as a confident "no feces"
         * result. Only a 2xx is a real answer. */
        int status = esp_http_client_get_status_code(c);
        if (status < 200 || status >= 300) {
            ESP_LOGD(TAG, "verify poll rejected: HTTP %d", status);
            s_verify_ok = false;
            s_verify_retry_ms = esp_timer_get_time() / 1000 + VERIFY_BACKOFF_MS;
        } else if (sink.len > 0) {
            s_verify_ok = true;
            verify_result_t v = { 0 };
            v.valid = true;
            v.detected = prv_parse_bool_key(body, "detected");
            v.conf = prv_parse_float_key(body, "yolo_conf");
            if (!prv_parse_box_norm(body, &v.x, &v.y, &v.w, &v.h)) {
                /* Server saw something but no target box (or the reply was cut
                 * short); fall back to a centred square so the overlay can
                 * still show a definite green/red answer. */
                v.x = 0.3f; v.y = 0.3f; v.w = 0.4f; v.h = 0.4f;
            }

            if (s_verify_mux) xSemaphoreTake(s_verify_mux, portMAX_DELAY);
            s_verify = v;
            s_verify_ms = esp_timer_get_time() / 1000;
            if (s_verify_mux) xSemaphoreGive(s_verify_mux);

            ESP_LOGD(TAG, "verify poll: detected=%d conf=%.3f box=%.2f,%.2f %.2fx%.2f",
                     (int)v.detected, (double)v.conf, (double)v.x, (double)v.y,
                     (double)v.w, (double)v.h);
        }
    }
    esp_http_client_cleanup(c);
}

void detection_service_verify_result(verify_result_t *out)
{
    if (out == NULL) return;
    if (s_verify_mux) xSemaphoreTake(s_verify_mux, portMAX_DELAY);
    *out = s_verify;
    int64_t stamp = s_verify_ms;
    if (s_verify_mux) xSemaphoreGive(s_verify_mux);
    out->age_ms = out->valid ? (uint32_t)(esp_timer_get_time() / 1000 - stamp) : 0;
}

uint32_t detection_service_detection_count(void)
{
    return s_detection_count;
}

static void detection_task(void *arg)
{
    (void)arg;
    uint32_t frame_counter = 0;
    int64_t last_cfg_ms = 0;
    int64_t last_verify_ms = esp_timer_get_time() / 1000;
    /* static: app_config_t is ~750 B and this task also parses HTTP bodies. */
    static app_config_t cfg;
    app_config_load(&cfg);
    last_cfg_ms = esp_timer_get_time() / 1000;

    for (;;) {
        camera_fb_t *fb = camera_service_get_frame();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Copy the JPEG and hand the camera buffer back BEFORE inferring.
         * classify() spends ~1.1 s in ESP-NN; holding a frame across that
         * window took a buffer out of circulation and starved the MJPEG
         * stream (2.85 fps of a 25 fps target). Same reasoning as the
         * report/verify copy below, just hoisted ahead of the model. */
        uint8_t *frame_copy = malloc(fb->len);
        if (frame_copy == NULL) {
            camera_service_release_frame(fb);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        memcpy(frame_copy, fb->buf, fb->len);
        size_t frame_len = fb->len;
        camera_service_release_frame(fb);

        fomo_result_t res = inference_service_classify(frame_copy, frame_len);

        /* Only the feces class may spray or publish. A pig-only frame must
         * neither trigger the relay nor be logged as a detection. */
        bool is_feces = res.class_id == EI_CLASS_FECES;
        bool triggered = false;
        if (is_feces) {
            triggered = spray_controller_on_detection(res.score);
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        /* Config only changes across a reboot, but re-read it a few times a
         * minute so a future non-rebooting settings path still takes effect
         * without paying 10 NVS reads per second. */
        if (now_ms - last_cfg_ms >= 5000) {
            app_config_load(&cfg);
            last_cfg_ms = now_ms;
        }

        /* Report/publish detections (feces class only), throttled to every Nth
         * frame so ~10 fps inference becomes ~2 Hz of network traffic. */
        bool do_report = is_feces && (frame_counter % 5 == 0);
        bool do_verify = (now_ms - last_verify_ms) >= VERIFY_POLL_MS &&
                         (s_verify_ok || now_ms >= s_verify_retry_ms);

        /* Count the device-side decision, not the HTTP outcome: a detection
         * still counts if the dashboard is down. Reported after the spray
         * decision above, so a counted detection has already been acted on. */
        if (do_report) {
            s_detection_count++;
        }

        /* The camera buffer is already back with the driver (see the copy
         * above), and frame_copy is a private heap JPEG, so network I/O here
         * cannot starve the stream. Reuse it rather than copying twice. */
        if (do_report || do_verify) {
            if (do_verify) {
                prv_poll_verify(&cfg, frame_copy, frame_len, res.score);
                last_verify_ms = esp_timer_get_time() / 1000;
            }
            if (do_report) {
                long id = prv_post_dashboard(&cfg, res.score, triggered, cfg.spray_ms);
                float yolo_conf = prv_post_verify(&cfg, frame_copy, frame_len, res.score);
                if (id > 0 && yolo_conf >= 0.0f) {
                    prv_post_verify_result(&cfg, id, yolo_conf);
                }
            }
        }
        free(frame_copy);
        frame_counter++;
        vTaskDelay(pdMS_TO_TICKS(100)); /* ~10 fps detection cadence */
    }
}

esp_err_t detection_service_start(void)
{
    s_verify_mux = xSemaphoreCreateMutex();
    /* 12 KB: this task parses HTTP replies (a 1.2 KB body buffer) and logging a
     * parsed reply goes through vprintf, which is not cheap on the stack. */
    xTaskCreatePinnedToCore(detection_task, "detect", 12288, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Detection loop started on core 1 (verify poll every %d ms)",
             VERIFY_POLL_MS);
    return ESP_OK;
}
