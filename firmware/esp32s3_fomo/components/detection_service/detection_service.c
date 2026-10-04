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
 * once a poll fails, and s_verify_retry_ms gates the next attempt.
 *
 * last_verify_ok_ms is when a verifier answer was last accepted. It exists so a
 * silent stall is detectable: the flag and deadline alone can both look healthy
 * while no poll is actually happening (this happened -- vrf.ageMs reached six
 * minutes with the server healthy), and this timestamp is what the stall guard
 * compares against. */
static bool s_verify_ok = true;
static int64_t s_verify_retry_ms = 0;
static int64_t last_verify_ok_ms = 0;

/* Consecutive failed polls since the last accepted answer, for /status. */
static uint32_t s_verify_failures = 0;

/* Feces detections acted on this boot (see detection_service_detection_count).
 * Bumped by the detection task only; read by the /status handler. A uint32_t
 * counter and a single writer make the torn-read window irrelevant in practice,
 * and the value is diagnostic, not control-critical. */
static volatile uint32_t s_detection_count = 0;

/* How often the live frame is re-checked by the server-side YOLOv8 model.
 * Independent of the on-device cadence so the dashboard overlay keeps updating
 * between on-device inference runs. */
#define VERIFY_POLL_MS 2000

/* A failed poll is retried on a backoff, but the backoff is CAPPED and always
 * expires. It used to be an unbounded latch: s_verify_ok went false and, if the
 * poll never got a clean attempt again, the gate at verify_due stopped being
 * satisfied entirely -- the board silently stopped polling and /status kept
 * reporting the last (increasingly ancient) answer. On a lossy link, where a
 * failed attempt can take as long as the poll interval, that state was reachable
 * and was reached: vrf.ageMs climbed past 6 minutes while the verifier was
 * healthy and reachable.
 *
 * Two rules make that unrecoverable-stall impossible:
 *   1. the gate ORs the backoff with elapsed time, so it can never latch shut;
 *   2. the cap is well under a few poll intervals, so a recovered link is picked
 *      up promptly instead of after a long silence.
 * The verify leg is display/corroboration data, so trading a little extra
 * traffic during an outage for guaranteed recovery is the right way round. */
#define VERIFY_BACKOFF_MAX_MS 4000

/* No accepted verifier answer for this long means the poll leg has stalled,
 * whatever the backoff state claims. Comfortably longer than poll interval +
 * backoff, so it only fires when something is genuinely wrong. */
#define VERIFY_STALL_MS 15000

/* After a failed poll the verifier is presumed down and skipped for this long.
 * The poll runs inline in the detect loop, so retrying every 2 s against a
 * closed port would spend most of the loop waiting on TCP timeouts -- but the
 * value is a CEILING, not a latch, and the gate below treats it as such. */
#define VERIFY_BACKOFF_MS VERIFY_BACKOFF_MAX_MS

/* Inference is gated on elapsed time, not on a frame count. classify() costs
 * ~1.1 s, so the loop's pace is set BY inference: skipping every other frame
 * would make the skipped iterations ~100 ms instead of ~1.2 s, the loop would
 * spin faster, and inference would run *more* often than before. Time gating
 * halves the load in absolute terms, which is what the MJPEG stream needs. */
#define INFER_INTERVAL_MS 2400

/* Report/publish throttle, also time-based for the same reason: once the loop
 * is no longer paced by inference, a frame counter would fire many times a
 * second. */
#define REPORT_MIN_INTERVAL_MS 1000

/* Per-request timeout for the verifier poll. See prv_poll_verify for why 1.5 s
 * was too tight on a hotspot. */
#define VERIFY_TIMEOUT_MS 5000

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

/* Leg 1 — POST the detection to the dashboard. Returns the new row id, or 0.
 *
 * `source` labels which detector produced the row. FOMO rows keep their
 * original wording so existing history rows and the demo script still read the
 * same; server-gated rows say so explicitly, because a row triggered by
 * YOLOv8 at 0.28 is a materially different claim from one the board made on its
 * own FOMO score, and the dashboard must not blur the two. */
static long prv_post_dashboard(const app_config_t *cfg, float conf,
                               bool triggered, uint32_t spray_ms,
                               const char *source)
{
    /* notes travels as a URL query value, so it must contain no raw spaces --
     * an unencoded space makes the whole URL invalid and esp_http_client
     * rejects it, which would silently break every dashboard POST. Underscores
     * are encoding-proof in a query string, unlike '+' (which PHP decodes as a
     * space, so it works only by accident); Api.php turns them back into
     * spaces on insert. */
    char notes[48];
    if (source == NULL || strcmp(source, "fomo") == 0) {
        strncpy(notes, triggered ? "auto_spray" : "cooldown_detected",
                sizeof(notes) - 1);
    } else {
        strncpy(notes, triggered ? "yolo_server_spray" : "yolo_server_detected",
                sizeof(notes) - 1);
    }
    notes[sizeof(notes) - 1] = '\0';

    char url[512];
    snprintf(url, sizeof(url),
        "%s?confidence=%.4f&triggered=%d&spray_ms=%u&esp=esp32s3&model=%s&notes=%s",
        cfg->dash_url, (double)conf, triggered ? 1 : 0, (unsigned)spray_ms,
        edge_impulse_model_tag(), notes);

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
 * answer for the dashboard overlay. Runs on a fixed cadence independent of the
 * on-device model, so the dashboard always has a second opinion.
 *
 * It ALSO gates the relay: a feces box at or above cfg->server_trigger_conf
 * sprays, alongside the on-device FOMO class in detection_task(). The two are
 * OR'd, so the on-device path still works with the server offline -- the
 * server-gated path is an additional trigger, never the only one.
 *
 * That trade is deliberate and has a cost worth stating: a server-gated spray
 * needs the PC and the Wi-Fi link up, and pays the poll interval plus the
 * round trip before the pump opens. On a marginal link that latency is seconds,
 * not milliseconds. Auto-spray on the FOMO class remains the offline path.
 *
 * Returns true if this poll triggered a spray. */
static bool prv_poll_verify(const app_config_t *cfg, const uint8_t *jpeg,
                            size_t len, float fomo_conf)
{
    bool triggered = false;
    if (jpeg == NULL || len == 0) return false;

    esp_http_client_handle_t c;
    char body[1280] = "";
    body_sink_t sink;
    /* 5 s, not 1.5 s. 1.5 s was tuned on a wired LAN, where the round trip
     * plus a JPEG upload fit comfortably. Over a phone hotspot the RTT sits at
     * 100-200 ms with spikes past 500 ms, and the 1.5 s budget expired
     * *mid-upload*: the board hung up before finishing the request body, the
     * server logged ClientDisconnect, and the poll failed. The cost of the
     * longer timeout is that a genuinely dead verifier now stalls this inline
     * detect loop for 5 s per attempt instead of 1.5 s -- acceptable because
     * the server is an *additional* trigger: the on-device FOMO path sprays
     * with this leg failing indefinitely. */
    c = prv_client_init(&sink, body, sizeof(body), cfg->verify_url, VERIFY_TIMEOUT_MS);
    if (!c) return false;

    char fomo_hdr[16];
    snprintf(fomo_hdr, sizeof(fomo_hdr), "%.4f", (double)fomo_conf);

    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "X-Fomo-Conf", fomo_hdr);
    esp_http_client_set_post_field(c, (const char *)jpeg, len);

    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "verify poll failed: %s (sent %u B, timeout %d ms)",
                 esp_err_to_name(e), (unsigned)len, VERIFY_TIMEOUT_MS);
        /* Back off so an offline verifier costs one timeout per
         * VERIFY_BACKOFF_MAX_MS instead of stalling every 2 s cycle. Kept short
         * (4 s, was 30 s): a 30 s backoff read as a dead verifier in the UI
         * long after the server came back. It is a ceiling and the gate ORs it
         * with elapsed time, so it delays a retry but can never suppress one
         * permanently. */
        s_verify_ok = false;
        s_verify_failures++;
        s_verify_retry_ms = esp_timer_get_time() / 1000 + VERIFY_BACKOFF_MAX_MS;
    } else {
        /* esp_http_client_perform() returns ESP_OK for 4xx/5xx too, so without
         * this check a rejected frame (the server answers 400 for an empty or
         * undecodable JPEG) would be published as a confident "no feces"
         * result. Only a 2xx is a real answer. */
        int status = esp_http_client_get_status_code(c);
        if (status < 200 || status >= 300) {
            ESP_LOGD(TAG, "verify poll rejected: HTTP %d", status);
            s_verify_ok = false;
            s_verify_failures++;
            s_verify_retry_ms = esp_timer_get_time() / 1000 + VERIFY_BACKOFF_MAX_MS;
        } else if (sink.len > 0) {
            s_verify_ok = true;
            s_verify_failures = 0;
            /* Clear the backoff deadline as well as the flag. Leaving the old
             * deadline in place meant a later failure reused a timestamp from
             * long ago, which could already have elapsed and so permitted an
             * immediate retry -- or, worse, a deadline in the future that kept
             * the gate shut past the point where the link had recovered. */
            s_verify_retry_ms = 0;
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
            last_verify_ok_ms = esp_timer_get_time() / 1000;

            ESP_LOGD(TAG, "verify poll: detected=%d conf=%.3f box=%.2f,%.2f %.2fx%.2f",
                     (int)v.detected, (double)v.conf, (double)v.x, (double)v.y,
                     (double)v.w, (double)v.h);

            /* Server-gated spray. The server only ever reports boxes at or
             * above its own YOLO_CONF (0.25), so `detected` is a necessary but
             * not sufficient condition -- the confidence is re-checked here
             * against server_trigger_conf so the effective gate is exactly the
             * configured one and not whatever the server happens to use.
             *
             * Reuses the confidence we already have, so a server-triggered
             * detection does not need a second JPEG upload to fill in
             * yolo_conf. */
            if (v.detected && v.conf >= cfg->server_trigger_conf) {
                if (spray_controller_trigger_external(v.conf,
                                                      cfg->server_trigger_conf,
                                                      cfg->spray_ms)) {
                    triggered = true;
                    ESP_LOGI(TAG, "server-triggered spray: YOLOv8 feces conf=%.3f "
                                  "(>= %.2f)",
                             (double)v.conf, (double)cfg->server_trigger_conf);
                }
            }
        } else {
            /* A 2xx whose body never reached the sink used to fall out of the
             * if/else with no branch at all: no publish, no backoff, no log.
             * /status then showed the last good result forever while the
             * server logged a healthy 200, so the overlay looked alive but was
             * stale -- and every row in the dashboard kept yolo_conf=NULL.
             * Report it, and treat it as a failed poll rather than silence. */
            ESP_LOGI(TAG, "verify poll: HTTP %d but empty body "
                          "(sink.len=0, sent %u B, timeout %d ms)",
                     status, (unsigned)len, VERIFY_TIMEOUT_MS);
            s_verify_ok = false;
            s_verify_failures++;
            s_verify_retry_ms = esp_timer_get_time() / 1000 + VERIFY_BACKOFF_MAX_MS;
        }
    }
    esp_http_client_cleanup(c);
    return triggered;
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

bool detection_service_verify_ok(void)
{
    return s_verify_ok;
}

uint64_t detection_service_last_verify_ok_age_ms(void)
{
    if (last_verify_ok_ms == 0) return UINT64_MAX;
    return (uint64_t)(esp_timer_get_time() / 1000) - (uint64_t)last_verify_ok_ms;
}

uint32_t detection_service_verify_failures(void)
{
    return s_verify_failures;
}

static void detection_task(void *arg)
{
    (void)arg;
    int64_t last_cfg_ms = 0;
    int64_t last_verify_ms = esp_timer_get_time() / 1000;
    int64_t last_infer_ms = 0;
    int64_t last_report_ms = 0;
    /* Cached decision, reused on iterations that skip classify(). Only this
     * task reads or writes it, so no lock is needed. */
    fomo_result_t last_res = { 0 };
    bool have_res = false;
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

        int64_t now_ms = esp_timer_get_time() / 1000;
        bool infer_due = !have_res || (now_ms - last_infer_ms) >= INFER_INTERVAL_MS;
        /* The backoff gate ORs with elapsed time, so it can only ever DELAY a
         * poll, never suppress one permanently: even with s_verify_ok stuck
         * false, once now_ms passes the deadline the poll runs again. That is
         * what makes the verifier self-heal after an outage. The separate
         * stall check below is a second, independent guard -- if the inline poll
         * itself is slow enough to starve its own scheduling, elapsed time
         * alone would never be reached. */
        bool verify_due = (now_ms - last_verify_ms) >= VERIFY_POLL_MS &&
                          (s_verify_ok || now_ms >= s_verify_retry_ms);
        /* Hard stall guard: if no successful verifier answer has landed for far
         * longer than the poll interval plus a few retries, force one attempt
         * regardless of any backoff state. Cheap (one poll) and the only thing
         * standing between a transient outage and a permanently dead overlay. */
        bool verify_stalled = (s_verify_ok == false) &&
                              (now_ms - last_verify_ok_ms) > VERIFY_STALL_MS;
        if (verify_stalled) verify_due = true;

        /* Only pull the bytes out of the driver when something actually needs
         * them. A skipped iteration used to malloc+memcpy+free the whole JPEG
         * for nothing, and that copy is the PSRAM traffic competing with the
         * stream. The camera buffer goes straight back either way. */
        uint8_t *frame_copy = NULL;
        size_t frame_len = 0;
        if (infer_due || verify_due) {
            frame_copy = malloc(fb->len);
            if (frame_copy != NULL) {
                memcpy(frame_copy, fb->buf, fb->len);
                frame_len = fb->len;
            } else {
                infer_due = false;
            }
        }
        camera_service_release_frame(fb);

        /* Reuse the previous decision when classify() is not due: the spray
         * logic still sees a real result, just an older one. With a 300 s
         * cooldown a 2.4 s-old decision is irrelevant to spray timing. */
        fomo_result_t res = last_res;
        if (infer_due) {
            res = inference_service_classify(frame_copy, frame_len);
            last_res = res;
            have_res = true;
            last_infer_ms = esp_timer_get_time() / 1000;
        }

        /* Only the feces class may spray or publish. A pig-only frame must
         * neither trigger the relay nor be logged as a detection. */
        bool is_feces = res.class_id == EI_CLASS_FECES;
        bool triggered = false;
        if (is_feces) {
            triggered = spray_controller_on_detection(res.score);
        }

        /* Config only changes across a reboot, but re-read it a few times a
         * minute so a future non-rebooting settings path still takes effect
         * without paying 10 NVS reads per second. */
        if (now_ms - last_cfg_ms >= 5000) {
            app_config_load(&cfg);
            last_cfg_ms = now_ms;
        }

        /* Report/publish detections (feces class only), on inference frames
         * and throttled by time so backend traffic stays bounded regardless of
         * how fast the loop runs. */
        bool do_report = infer_due && is_feces &&
                         (now_ms - last_report_ms) >= REPORT_MIN_INTERVAL_MS;
        verify_due = verify_due && frame_copy != NULL;
        bool do_verify = verify_due;

        /* Count the device-side decision, not the HTTP outcome: a detection
         * still counts if the dashboard is down. Reported after the spray
         * decision above, so a counted detection has already been acted on. */
        if (do_report) {
            s_detection_count++;
            last_report_ms = esp_timer_get_time() / 1000;
        }

        /* The camera buffer is already back with the driver (see the copy
         * above), and frame_copy is a private heap JPEG, so network I/O here
         * cannot starve the stream. Reuse it rather than copying twice. */
        if (do_report || do_verify) {
            bool server_triggered = false;
            if (do_verify) {
                server_triggered = prv_poll_verify(&cfg, frame_copy, frame_len,
                                                   res.score);
                last_verify_ms = esp_timer_get_time() / 1000;
            }
            if (do_report) {
                long id = prv_post_dashboard(&cfg, res.score, triggered,
                                             cfg.spray_ms, "fomo");
                float yolo_conf = prv_post_verify(&cfg, frame_copy, frame_len, res.score);
                if (id > 0 && yolo_conf >= 0.0f) {
                    prv_post_verify_result(&cfg, id, yolo_conf);
                }
            }

            /* A server-triggered detection must reach the dashboard history too,
             * or the log silently omits exactly the events that caused a spray
             * while the history still shows FOMO-only columns. Counted on the
             * trigger decision rather than the HTTP outcome, so the counter
             * reflects what the device decided even if the upload fails.
             *
             * yolo_conf comes from the answer we already have, so this costs one
             * small GET rather than re-uploading the JPEG. */
            if (server_triggered) {
                s_detection_count++;
                long id = prv_post_dashboard(&cfg, cfg.server_trigger_conf, true,
                                             cfg.spray_ms, "server");
                if (id > 0) {
                    verify_result_t v;
                    detection_service_verify_result(&v);
                    prv_post_verify_result(&cfg, id, v.conf);
                }
            }
        }
        free(frame_copy);
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
