/*
 * Publishes detected feces coordinates to an external actuator board.
 *
 * Design notes that matter:
 *
 *  - The detection loop is already paced by ~700 ms of on-device FOMO
 *    inference plus a verifier poll that costs another ~600 ms. A second
 *    blocking HTTP POST inside that loop would stretch the loop further and
 *    delay every detection behind it. So publishing happens on its OWN task
 *    and the detection loop only ever hands over a message.
 *
 *  - A dead actuator board must not be able to stall anything. The POST has a
 *    short timeout, and a message that arrives while another is still in
 *    flight is DROPPED rather than queued. The actuator board has its own
 *    "busy" guard, and a target that is 10 s late is worse than one that
 *    never arrives -- the pen has moved on by then.
 *
 *  - The camera board never decides whether to spray. It publishes what it
 *    saw and reports what came back.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "target_publisher.h"

static const char *TAG = "target_pub";

/* POST timeout. The actuator board is on the same LAN and answers in
 * milliseconds, so this is generous: it exists to bound the damage when the
 * board is powered off but still on the network (a stale ARP entry makes the
 * connect hang until the timeout). */
#define TARGET_HTTP_TIMEOUT_MS 1500

/* The reply is tiny ({"ok":true,"accepted":false,"reason":"low_water"}), but
 * it must be captured while esp_http_client_perform() runs -- perform()
 * drains the body itself and a following read() would return nothing. */
#define TARGET_REPLY_CAP 256

/* One queued message. Sized for 8 boxes; the JSON builder refuses to
 * overflow rather than truncating mid-object. */
#define TARGET_JSON_CAP 768

typedef struct {
    char  json[TARGET_JSON_CAP];
    uint32_t detection_id;
    uint8_t  count;
    char     source[8];
} target_msg_t;

static QueueHandle_t s_queue;
static target_publisher_status_t s_status;
/* Absolute time of the last completed POST, or -1 for never. The age is
 * derived from this at read time rather than counted up by the task, so it
 * stays truthful even when nothing is being published. */
static int64_t s_last_ok_at_ms = -1;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void prv_lock(void) { taskENTER_CRITICAL(&s_lock); }
static void prv_unlock(void) { taskEXIT_CRITICAL(&s_lock); }

/* ---------- reply parsing ----------
 * The reply is a small flat JSON object. A tiny scanner is enough and avoids
 * pulling a JSON library into a firmware image whose app partition is
 * already 99% full. Anything unparsable is reported as "unknown", which the
 * dashboard shows as "replied, outcome not understood" -- never as success. */
static bool prv_json_bool(const char *json, const char *key, bool *out)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return false;
    p += strlen(needle);
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool prv_json_float(const char *json, const char *key, float *out)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return false;
    p += strlen(needle);
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = (float)v;
    return true;
}

static void prv_json_str(const char *json, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (p == NULL) return;
    p += strlen(needle);
    size_t i = 0;
    while (p[i] != '"' && p[i] != '\0' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

/* ---------- the POST ---------- */

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

/* Send one message and record the outcome. Runs on the publisher task only,
 * so it may block. */
static void prv_send(target_msg_t *msg)
{
    char reply[TARGET_REPLY_CAP];
    body_sink_t sink = { .buf = reply, .cap = sizeof(reply), .len = 0 };
    reply[0] = '\0';

    esp_http_client_config_t cc = {
        .url = s_status.url,
        .timeout_ms = TARGET_HTTP_TIMEOUT_MS,
        .user_data = &sink,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cc);
    if (c == NULL) {
        ESP_LOGW(TAG, "cannot create client for %s", s_status.url);
        prv_lock();
        s_status.last_ok = false;
        s_status.failures++;
        prv_unlock();
        return;
    }
    esp_http_client_set_event_handler(c, prv_body_cb);
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, msg->json, strlen(msg->json));

    int64_t t0 = esp_timer_get_time() / 1000;
    esp_err_t e = esp_http_client_perform(c);
    int64_t took = esp_timer_get_time() / 1000 - t0;

    bool ok = false;
    bool accepted = false;
    bool have_angles = false;
    char reason[24] = "";
    float pan = 0.0f, tilt = 0.0f;

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s (took %lld ms)",
                 s_status.url, esp_err_to_name(e), (long long)took);
    } else {
        int status = esp_http_client_get_status_code(c);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "POST %s -> HTTP %d (took %lld ms)",
                     s_status.url, status, (long long)took);
        } else {
            ok = true;
            /* The board may answer with this repo's "accepted", or with the
             * older "ack" that the deployed board uses. Both mean it took
             * the message. The fallback only happens when "accepted" is
             * ABSENT -- a board that says accepted:false has refused, and
             * must not be second-guessed. */
            if (!prv_json_bool(reply, "accepted", &accepted)) {
                prv_json_bool(reply, "ack", &accepted);
            }
            prv_json_str(reply, "reason", reason, sizeof(reason));
            /* The angles are only reported by a board that works them out
             * itself; one that aims internally sends none, and the dashboard
             * must not print "pan 0.0" for a spray that really happened. */
            have_angles = prv_json_float(reply, "pan", &pan)
                       && prv_json_float(reply, "tilt", &tilt);
            ESP_LOGI(TAG, "detection %u -> accepted=%d reason=%s pan=%.1f tilt=%.1f "
                          "(%d boxes, %s, took %lld ms)",
                     (unsigned)msg->detection_id, (int)accepted,
                     reason[0] ? reason : "-", (double)pan, (double)tilt,
                     (int)msg->count, msg->source, (long long)took);
        }
    }
    esp_http_client_cleanup(c);

    prv_lock();
    s_status.last_ok = ok;
    s_status.last_accepted = accepted;
    snprintf(s_status.last_reason, sizeof(s_status.last_reason), "%s", reason);
    snprintf(s_status.last_source, sizeof(s_status.last_source), "%s",
             msg->source);
    s_status.last_pan = pan;
    s_status.last_tilt = tilt;
    s_status.last_angles = have_angles;
    s_status.last_detection_id = msg->detection_id;
    if (ok) {
        s_last_ok_at_ms = esp_timer_get_time() / 1000;
        s_status.failures = 0;
    } else {
        s_status.failures++;
    }
    prv_unlock();
}

/* ---------- publisher task ---------- */

static void prv_target_task(void *arg)
{
    (void)arg;
    target_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        /* Re-check the configuration: it may have been switched off (or
         * repointed) while this message sat in the queue. */
        prv_lock();
        bool enabled = s_status.enabled && s_status.url[0] != '\0';
        prv_unlock();
        if (!enabled) {
            ESP_LOGI(TAG, "publishing disabled; dropping detection %u",
                     (unsigned)msg.detection_id);
            continue;
        }

        prv_send(&msg);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- public API ---------- */

esp_err_t target_publisher_init(void)
{
    if (s_queue != NULL) return ESP_OK;   /* already running */

    s_queue = xQueueCreate(1, sizeof(target_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "cannot create publisher queue");
        return ESP_FAIL;
    }
    s_status.last_ok_age_ms = UINT64_MAX;

    if (xTaskCreatePinnedToCore(prv_target_task, "target_pub", 4096, NULL, 4,                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "cannot create publisher task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "target publisher ready");
    return ESP_OK;
}

void target_publisher_set_config(const char *url, bool enabled)
{
    prv_lock();
    bool changed = false;
    if (url != NULL) {
        if (strncmp(s_status.url, url, sizeof(s_status.url)) != 0) changed = true;
        snprintf(s_status.url, sizeof(s_status.url), "%s", url);
    }
    if (enabled != s_status.enabled) changed = true;
    s_status.enabled = enabled && s_status.url[0] != '\0';
    prv_unlock();

    if (changed) {
        ESP_LOGI(TAG, "publishing %s -> %s",
                 s_status.enabled ? "ENABLED" : "disabled",
                 s_status.url[0] ? s_status.url : "(no url)");
    }
}

void target_publisher_publish(const target_box_t *boxes, uint8_t count,
                              uint16_t frame_w, uint16_t frame_h,
                              const char *source, uint32_t detection_id)
{
    prv_lock();
    bool enabled = s_status.enabled && s_status.url[0] != '\0';
    prv_unlock();
    if (!enabled) return;
    if (boxes == NULL || count == 0 || count > TARGET_MAX_BOXES) return;

    /* Build the message. Every snprintf is bounds-checked and the whole
     * thing is abandoned rather than truncated: a half-written JSON object
     * would be rejected by the receiver with no useful error. */
    target_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.detection_id = detection_id;
    msg.count = count;
    snprintf(msg.source, sizeof(msg.source), "%s",
             (source && source[0]) ? source : "fomo");

    int n = snprintf(msg.json, sizeof(msg.json),
                     "{\"v\":1,\"detection_id\":%u,\"ts_ms\":%llu,"
                     "\"source\":\"%s\",\"frame\":{\"width\":%u,\"height\":%u},"
                     "\"count\":%u,\"boxes\":[",
                     (unsigned)detection_id,
                     (unsigned long long)(esp_timer_get_time() / 1000),
                     (source && source[0]) ? source : "fomo",
                     (unsigned)frame_w, (unsigned)frame_h, (unsigned)count);
    if (n < 0 || (size_t)n >= sizeof(msg.json)) return;

    for (uint8_t i = 0; i < count; i++) {
        int w = snprintf(msg.json + n, sizeof(msg.json) - (size_t)n,
                         "%s{\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f,"
                         "\"conf\":%.4f}",
                         (i > 0) ? "," : "",
                         (double)boxes[i].x, (double)boxes[i].y,
                         (double)boxes[i].w, (double)boxes[i].h,
                         (double)boxes[i].conf);
        if (w < 0 || (size_t)w >= sizeof(msg.json) - (size_t)n) {
            ESP_LOGW(TAG, "message would overflow; not published");
            return;
        }
        n += w;
    }
    int w = snprintf(msg.json + n, sizeof(msg.json) - (size_t)n, "]}");
    if (w < 0 || (size_t)w >= sizeof(msg.json) - (size_t)n) return;

    prv_lock();
    s_status.published++;
    prv_unlock();

    /* Drop rather than block if a message is still in flight. */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        prv_lock();
        s_status.dropped++;
        prv_unlock();
        ESP_LOGW(TAG, "publisher busy; dropped detection %u",
                 (unsigned)detection_id);
    }
}

void target_publisher_status(target_publisher_status_t *out)
{
    if (out == NULL) return;
    prv_lock();
    *out = s_status;
    int64_t last_at = s_last_ok_at_ms;
    prv_unlock();
    /* Derived here, not stored, so it is correct even if the task is idle. */
    out->last_ok_age_ms = (last_at < 0)
        ? UINT64_MAX
        : (uint64_t)(esp_timer_get_time() / 1000 - last_at);
}
