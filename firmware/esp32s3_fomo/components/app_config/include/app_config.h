#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define APP_NVS_NAMESPACE "fomo_app"
#define APP_WIFI_SSID_KEY "wifi_ssid"
#define APP_WIFI_PASS_KEY "wifi_pass"
#define APP_VERIFY_URL_KEY "verify_url"
#define APP_DASH_URL_KEY "dash_url"
#define APP_DASH_VERIFY_URL_KEY "dash_verify_url"
/* NVS keys are capped at 15 characters, so "relay_active_low" (16) would fail
 * nvs_set_u8 and never persist. 15 chars exactly. */
#define APP_RELAY_ACTIVE_LOW_KEY "relay_act_low"
/* NVS keys are capped at 15 characters, so this cannot be
 * "server_trigger_conf" (19) -- nvs_set_blob would fail and the setting would
 * silently never persist across a reboot. 15 chars exactly. */
#define APP_SERVER_TRIGGER_CONF_KEY "srv_trig_conf"

/* Optional static addressing. Empty means "use DHCP", which is the default, so
 * an unprovisioned device behaves normally. A static address matters because
 * every dashboard /history /live URL and the manual "Spray now" button hardcode
 * the device address: when DHCP hands out a new one, the dashboard silently
 * points at nothing and looks like a dead detector rather than a stale address.
 *
 * A DHCP reservation on the router achieves the same thing with no firmware
 * coupling; this exists so the device can also be pinned on networks where the
 * router cannot be configured. */
#define APP_STATIC_IP_KEY    "static_ip"
#define APP_STATIC_MASK_KEY  "static_mask"
#define APP_STATIC_GW_KEY    "static_gw"

/* Operator kill switch for the pump: when 0, automatic sprays are refused
 * (detection still alerts and is still logged) until it is set back to 1.
 * 13 characters, inside the 15-char NVS key limit. Absent = enabled, so a
 * device that was never provisioned keeps spraying. */
#define APP_SPRAY_ENABLED_KEY "spray_enabled"

/* Where to publish detected feces coordinates for an external actuator board
 * ("main ESP32") to aim at. See docs/target-protocol.md.
 *
 * This is ADDITIVE on purpose: the camera board keeps its own relay and its
 * own local spray path exactly as they are, so it stays usable standalone.
 * Publishing is a second, independent consumer of the same detection.
 *
 * Empty = publishing off. "target_enabled" is a separate switch so an operator
 * can stop publishing without having to erase the address they will want back
 * (a DHCP main board moves; the address is tedious to retype).
 *
 * 10 and 14 characters -- both inside the 15-char NVS key limit. */
#define APP_TARGET_URL_KEY "target_url"
#define APP_TARGET_ENABLED_KEY "target_enabled"

/* Wi-Fi credentials are supplied per device and must not live in source. app_config
 * reads them from NVS (namespace "fomo_app", keys "wifi_ssid" / "wifi_pass") and
 * falls back to these only when NVS has no entry. Set them once with:
 *   curl -X POST http://<device-ip>/config -d 'wifi_ssid=...&wifi_pass=...'
 * which writes NVS and reboots. Keep them empty here. */
#define APP_DEFAULT_WIFI_SSID ""
#define APP_DEFAULT_WIFI_PASS ""
#define APP_DEFAULT_VERIFY_URL "http://192.168.1.3:8000/verify"
#define APP_DEFAULT_DASH_URL "http://192.168.1.3/feces-detector-ai/dashboard/api/insert"
#define APP_DEFAULT_DASH_VERIFY_URL "http://192.168.1.3/feces-detector-ai/dashboard/api/verify"

/* Alarm output pins. Each load gets its OWN pin.
 *
 * GPIO4 is the camera's SCCB data line (CAM_PIN_SIOD in camera_service.c) and
 * camera init runs after spray init, so it reclaims the pin as an open-drain
 * input. Driving it as a relay silently does nothing. Use 14 instead.
 *
 * The buzzer must NOT share the relay pin. The buzzer is driven by LEDC
 * (TIMER_1/CHANNEL_1) at 2 kHz so that both active and passive parts sound;
 * LEDC routes its output to the pad through the GPIO matrix, and once it has
 * claimed the pin a plain gpio_set_level() on the same pin no longer reaches
 * the pad. A relay sharing that pin would see a 2 kHz square wave instead of
 * a steady level -- it never latches cleanly, while the serial log cheerfully
 * prints "Spray ON". GPIO21 is free on this devkit (see docs/wiring-guide.md).
 *
 * The LED is an external part on GPIO1 behind a 330 ohm resistor, which is
 * what docs/wiring-guide.md specifies. GPIO2 is deliberately left unused: on
 * several ESP32-S3 devkits it is an addressable RGB LED that needs a
 * NeoPixel/RMT bitstream and does nothing useful as a plain GPIO. */
#define APP_PIN_RELAY 14       /* GPIO driving the relay IN / water pump */
#define APP_PIN_LED 1          /* external status LED via 330 ohm */
#define APP_PIN_BUZZER 21      /* alarm buzzer via 220 ohm, driven via LEDC */

/* Most single-channel relay modules (KY-019, SRD-05VDC-SL-C) are ACTIVE-LOW:
 * the coil energises when IN is pulled to GND. That is fail-safe, because a
 * floating or crashed pin then means "pump off". Active-HIGH modules are also
 * common, so the polarity is a setting rather than a hardcoded assumption.
 * Confirm with a multimeter before trusting the default: touch a continuity
 * probe between GND and IN; if the relay clicks, it is active-low. */
#define APP_DEFAULT_RELAY_ACTIVE_LOW 1

#define APP_DEFAULT_THRESHOLD 0.60f
/* 0 = every detection above the dial sprays, with no cooldown hold-back. The
 * infer loop runs classify() about every 2.4 s and a spray lasts spray_ms, so
 * with this at 0 the relay re-arms almost immediately after each spray ends:
 * while feces stays in frame the pump runs continuously, which is the intended
 * "spray until the pen is clean" behaviour. Set a non-zero value over
 * POST /config to rate-limit water use; the LED and buzzer still fire on every
 * detection even when the pump is held back. */
#define APP_DEFAULT_COOLDOWN_MS 0u
#define APP_DEFAULT_SPRAY_MS 5000u
#define APP_DEFAULT_FOMO_CONF 0.8f /* min FOMO "object present" score */

/* Minimum YOLOv8 (server-side) confidence that counts as a detection and may
 * trigger a spray. The server's own YOLO_CONF is 0.25, so it *reports* boxes
 * down to 0.25; this is the gate that decides whether one of them sprays.
 *
 * Kept separate from `threshold` on purpose: that dial gates the on-device FOMO
 * score, and the two models are not calibrated against each other, so reusing
 * one number for both would either spray on FOMO noise or ignore good YOLO
 * hits. */
#define APP_DEFAULT_SERVER_TRIGGER_CONF 0.28f

/* Tunables persisted to NVS; on first boot these defaults are written. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char verify_url[129];
    char dash_url[257];
    char dash_verify_url[257];
    float threshold;
    uint32_t cooldown_ms;
    uint32_t spray_ms;
    bool relay_active_low;
    float server_trigger_conf;
    /* Operator switch for the pump. true = automatic sprays allowed;
     * false = the operator turned the sprayer OFF (detection still alerts
     * and is still logged; only the relay is refused). */
    bool spray_enabled;
    /* External actuator board ("main ESP32") to publish detections to, and
     * whether to publish at all. Empty target_url = publishing off. */
    char target_url[129];
    bool target_enabled;
    /* Optional static IPv4 addressing; all three empty = DHCP (the default).
     * Set together, never one at a time -- a partial set is rejected at load
     * so the device cannot end up with, say, an address but the wrong gateway
     * and become unreachable with no way in to correct it. */
    char static_ip[16];
    char static_mask[16];
    char static_gw[16];
} app_config_t;

esp_err_t app_config_load(app_config_t *cfg);
void app_config_save(const app_config_t *cfg);
