#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Converts an image coordinate into a servo angle.
 *
 * This is a PURE function: no hardware, no globals, no I/O. That is
 * deliberate -- it is the part most likely to be wrong, and it has to be
 * testable on a host without a board, a servo or a pig pen.
 *
 * Why a full model and not a linear map: the prompt is explicit that image
 * coordinates must not be treated as servo angles. The camera looks DOWN at
 * the pen floor from a height, so a pixel near the top of the frame is far
 * away and a pixel near the bottom is close. A linear map gets the middle
 * right and the edges badly wrong, and the edges are exactly where the
 * corners of the pen are.
 *
 * World frame:
 *   X = East, Y = North, Z = Up. The pen floor is the plane z = 0.
 *   The camera sits at (cam_x, cam_y, cam_height) and looks toward +Y when
 *   cam_yaw_deg is 0.
 *
 *   cam_yaw_deg follows the right-hand rule about +Z, so a POSITIVE yaw
 *   turns the camera's +Y direction toward -X. cam_pitch_deg is positive
 *   when the camera looks DOWN.
 *
 * The nozzle sits at (nozzle_x, nozzle_y, nozzle_height) and aims at the point
 * on the floor where the camera ray lands. The nozzle's pan is measured the
 * other way round -- pan = 0 aims at +Y and a positive pan aims at +X --
 * because the nozzle and the camera face each other and it is easier to
 * think about each one from its own point of view.
 */

/* Everything the solver needs. All lengths in millimetres, all angles in
 * degrees, matching how a pen is actually measured. */
typedef struct {
    float cam_x, cam_y, cam_height;   /* camera position above the floor */
    float cam_yaw_deg;                /* 0 = looking toward +Y */
    float cam_pitch_deg;              /* degrees BELOW horizontal */
    float focal_px;                   /* focal length in pixels */
    float nozzle_x, nozzle_y, nozzle_height;
    float pan_min, pan_max;           /* servo mechanical limits, degrees */
    float tilt_min, tilt_max;
} geometry_cal_t;

typedef struct {
    bool  valid;        /* false when the ray misses the floor */
    float pan;          /* degrees, clamped to the servo limits */
    float tilt;         /* degrees, clamped to the servo limits */
    float world_x, world_y;  /* the point on the floor, in mm */
    float clamped;      /* true when the requested angle hit a limit */
} geometry_result_t;

/* Solve for one box.
 *
 * nx, ny are the box's TOP-LEFT corner, normalized 0..1 (the wire format).
 * The box CENTRE is what gets aimed at, because that is the middle of the
 * feces rather than its corner.
 *
 * frame_w / frame_h are the dimensions of the image the coordinates refer to.
 * focal_px is in the same pixel units.
 *
 * Returns false (and leaves out->valid = false) when the camera ray does not
 * hit the floor -- pointing above the horizon, or a calibration that is
 * obviously broken. The caller must NOT move a servo on an invalid solve. */
bool geometry_solve(const geometry_cal_t *cal,
                    float nx, float ny, float nw, float nh,
                    uint16_t frame_w, uint16_t frame_h,
                    geometry_result_t *out);

/* The same solve, but for a normalised point rather than a box. Used by the
 * manual "aim at this point" test, and by the calibration check. */
bool geometry_solve_point(const geometry_cal_t *cal,
                           float px, float py,
                           uint16_t frame_w, uint16_t frame_h,
                           geometry_result_t *out);

/* ---------------- the pen, as the calibration target ----------------
 *
 * The camera sits at ONE END of the pigpen and the sprayer at the other, so
 * the floor is seen in perspective: a pixel's size in centimetres depends on
 * how far down the pen it is. The camera-pose model above can describe that,
 * but it needs the camera's height, tilt and lens field of view -- three
 * numbers that are easy to get wrong and impossible to check without the
 * hardware.
 *
 * The pen itself is a KNOWN rectangle on the floor: four corners, a real size
 * in centimetres, and nothing else to measure. Four point correspondences
 * fit a homography, which maps the picture onto the pen exactly. The board
 * needs no camera pose at all -- just the matrix, the pen's size, and where
 * the nozzle is.
 *
 * Pen frame:
 *   X = across the pen, 0 at the near-left corner as the camera sees it
 *   Y = along the pen, 0 at the camera's end, growing toward the sprayer
 *
 * The nozzle faces BACK toward the camera, so its forward direction is -Y.
 * That is why the nozzle's pose is expressed in the pen frame rather than in
 * a camera frame: the pen already knows which way both ends face.
 */
typedef struct {
    bool  present;           /* false when the pen's size is unknown */
    bool  fitted;            /* h[] holds a fit to the picture; when it
                              * does not, the LINEAR model applies */
    float h[8];              /* the homography: pen cm -> image fraction */
    float width_cm;          /* across the pen */
    float length_cm;         /* along the pen */
    float flip_y;            /* 1 when the camera is at the top of the picture */

    /* The nozzle, in the PEN frame. */
    float nozzle_x_cm, nozzle_y_cm;
    float nozzle_height_mm;  /* above the pen floor */

    /* The servo's own zero, relative to the geometry's angles. The pan
     * offset is the angle that aims straight down the pen; the tilt offset
     * is the angle that points straight down.
     *
     * The two DIRECTION flags exist because the nozzle sits at the far end
     * of the pen facing back, so the picture's left is the servo's right,
     * and because a tilt servo may count up or down from its own zero. Both
     * are properties of the mechanism, not of the maths, so they are
     * settings: an operator checks the aim against a measured spot and
     * flips the flag rather than guessing. */
    float pan_offset_deg, tilt_offset_deg;
    float pan_dir;           /* +1: the picture's left is the servo's left */
    float tilt_dir;          /* +1: the servo number grows as the spot nears */

    float pan_min, pan_max;  /* servo travel, degrees */
    float tilt_min, tilt_max;
} geometry_pen_t;

/* The inverse of the 3x3 homography, as eight numbers with the bottom-right
 * term fixed at 1. Returns false when the matrix cannot be inverted (four
 * points in a line, or two on the same spot). */
bool geometry_invert_homography(const float h[8], float out[8]);

/* Image fraction (0..1 of the frame) -> pen centimetres.
 * Only meaningful when a fit to the picture is available. */
bool geometry_pen_image_to_cm(const geometry_pen_t *cal,
                              float u, float v,
                              float *cm_x, float *cm_y);

/* Solve for one box against the pen.
 *
 * nx, ny are the box's TOP-LEFT corner, normalized 0..1 (the wire format);
 * the box CENTRE is what gets aimed at, because that is the middle of the
 * feces rather than its corner.
 *
 * The result's world_x / world_y are the pen's own centimetres times ten,
 * i.e. millimetres in the pen frame, for the record.
 *
 * Returns false when there is no calibration, or when the spot is behind the
 * nozzle -- which no amount of servo travel can reach. */
bool geometry_solve_pen(const geometry_pen_t *cal,
                        float nx, float ny, float nw, float nh,
                        geometry_result_t *out);
