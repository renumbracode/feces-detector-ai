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
