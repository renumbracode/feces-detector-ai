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

/* The pen, as the calibration target.
 *
 * The camera sits at ONE END of the pigpen and the sprayer at the other, so
 * the floor is seen in perspective and the pen's four corners are a known
 * rectangle on it. Four point correspondences fit a homography, and that maps
 * the picture onto the pen exactly -- no camera height, no tilt, no lens
 * field of view to be guessed.
 *
 * The matrix is stored as eight numbers: it maps pen centimetres to a
 * fraction of the image, with the bottom-right term fixed at 1.
 *
 * Pen frame:
 *   X = across the pen, 0 at the near-left corner as the camera sees it
 *   Y = along the pen, 0 at the camera's end, growing toward the sprayer
 *
 * The nozzle faces BACK toward the camera, so its forward direction is -Y.
 *
 * The travel limits here are the SERVO's own numbering (0 = one end stop),
 * which is not the same numbering as the geometric angles in the
 * camera-pose model above. That is why they are kept apart: the two models
 * describe the same servo from two different reference frames, and the
 * limits belong to the servo, not to either model. */
typedef struct {
    bool  present;           /* the pen's size is known, so the pen model
                              * may be used at all */
    bool  fitted;            /* h[] holds a fit to the picture; when it does
                              * not, the LINEAR model applies */
    float h[8];              /* the homography: pen cm -> image fraction */
    float width_cm;          /* across the pen */
    float length_cm;         /* along the pen */
    float flip_y;            /* 1 when the camera is at the top of the picture */
    float nozzle_x_cm;       /* the nozzle, in the pen frame */
    float nozzle_y_cm;
    float nozzle_height_mm;  /* above the pen floor */
    float pan_offset_deg;    /* the angle that aims straight down the pen */
    float tilt_offset_deg;   /* the angle that points straight down */
    float pan_dir;           /* +1: the picture's left is the servo's left */
    float tilt_dir;          /* +1: the servo number grows as the spot nears */
    float pan_min, pan_max;  /* the servo's travel, in its own numbering */
    float tilt_min, tilt_max;
} pen_cal_t;

/* Calibration and limits, all stored as 4-byte float blobs so a value can be
 * written without knowing its type at the call site. */
typedef struct {
    /* Camera pose relative to the pen floor. mm and degrees. */
    float cam_x, cam_y, cam_height;
    float cam_yaw_deg, cam_pitch_deg;
    float focal_px;

    /* Nozzle mount. */
    float nozzle_x, nozzle_y, nozzle_height;

    /* The pen calibration. When it is present, the board aims using the
     * pen's own centimetres instead of the camera-pose model above. It is
     * stored as one blob: it is one thing, set and cleared together. */
    pen_cal_t pen;

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
