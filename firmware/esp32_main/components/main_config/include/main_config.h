#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Everything the main board needs to remember between reboots.
 *
 * All of it is calibration: where the camera is, where the nozzle is, how far
 * the servos can swing, how much water is enough. The camera board sends
 * image coordinates; turning those into angles needs these numbers, and they
 * are deployment facts, so they belong in NVS rather than in the source.
 *
 * The camera board does NOT send any of this -- it cannot know where the
 * nozzle is. See docs/target-protocol.md.
 */

#define MAIN_NVS_NAMESPACE "fomo_act"

/* Calibration and limits, all stored as 4-byte float blobs so a value can be
 * written without knowing its type at the call site. */
typedef struct {
    /* Camera pose relative to the pen floor. mm and degrees. */
    float cam_x, cam_y, cam_height;
    float cam_yaw_deg, cam_pitch_deg;
    float focal_px;

    /* Nozzle mount. */
    float nozzle_x, nozzle_y, nozzle_height;

    /* Servo mechanical limits, degrees. The solver clamps to these, so a
     * bad calibration cannot drive a servo into its end stop. */
    float pan_min, pan_max;
    float tilt_min, tilt_max;

    /* Servo pulse widths. 500..2400 us is the usual hobby-servo range
     * (SG90 and friends). A servo that needs a different range gets these
     * two numbers changed and nothing else. */
    uint32_t servo_pulse_min_us;
    uint32_t servo_pulse_max_us;

    /* Water level below which cleaning is refused. Percentage of full. */
    float water_min_pct;

    /* How long to spray, and how long to wait afterwards. */
    uint32_t spray_ms;
    uint32_t cooldown_ms;

    /* Relay IN polarity: most single-channel modules are active-LOW. */
    bool relay_active_low;

    /* Operator switch for automatic cleaning. */
    bool enabled;

    /* The water float switch, when fitted, is an independent interlock: the
     * float must read "water present" for a spray to be allowed. Disabled by
     * default because most tanks do not have one. */
    bool use_float_switch;

    /* Wi-Fi credentials for the LAN the camera board is also on.
     *
     * There is deliberately NO compiled-in default. A default SSID and
     * password in the source is a default password in every clone of the
     * source, and this board must never try to join a network nobody chose
     * for it. The /setup page writes them over USB. */
    char wifi_ssid[33];
    char wifi_pass[65];
} main_config_t;

/* Load from NVS, filling in defaults for anything not stored. Always
 * succeeds: a device with no stored config gets the defaults rather than
 * refusing to boot. */
void main_config_load(main_config_t *cfg);

/* Write to NVS. */
esp_err_t main_config_save(const main_config_t *cfg);

/* The defaults, for the setup page and for a fresh board. */
void main_config_defaults(main_config_t *cfg);

/* Copy the stored Wi-Fi credentials out. Returns false when none are stored,
 * in which case the caller must not try to connect to anything. */
bool main_config_load_wifi(char *ssid, size_t ssid_cap,
                           char *pass, size_t pass_cap);
