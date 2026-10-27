/*
 * Image coordinate -> servo angle. See include/geometry.h for the model.
 *
 * The camera looks DOWN at the pen floor, so a pixel maps to a ray that has
 * to be intersected with the floor plane before it means anything. A pixel
 * near the top of the frame is far away; one near the bottom is close. That
 * is why this cannot be a linear map.
 */
#include <math.h>

#include "geometry.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float prv_clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Build the world-space direction of a camera-frame ray.
 *
 * Camera frame: +x right, +y down, +z forward.
 * World frame:   +X east, +Y north, +Z up.
 *
 * With pitch theta (degrees BELOW horizontal):
 *   forward = (0, cos t, -sin t)
 *   right   = (1, 0, 0)
 *   down    = (0, -sin t, -cos t)      <- forward x right
 *
 * `down` is the one that is easy to get wrong, and getting it wrong looks
 * plausible: the camera's own "down" direction tilts BACKWARD as the camera
 * pitches down, because the IMAGE's vertical axis is what swings, not the
 * view direction. With the wrong sign, a pixel above the centre solves to a
 * point CLOSER to the camera instead of farther away -- the error is
 * invisible until a servo sweeps the wrong way.
 */
static void prv_world_ray(float dx, float dy, float dz,
                          float pitch_deg, float yaw_deg,
                          float *wx, float *wy, float *wz)
{
    float t = pitch_deg * (float)M_PI / 180.0f;
    float p = yaw_deg * (float)M_PI / 180.0f;

    float ct = cosf(t), st = sinf(t);
    float cp = cosf(p), sp = sinf(p);

    /* Un-yawed world components. */
    float ux = dx;
    float uy = -dy * st + dz * ct;
    float uz = -dy * ct - dz * st;

    /* Rotate about Z by the yaw. Right-hand rule about +Z, so a POSITIVE yaw
     * turns the +Y direction toward -X. The calibration page says what the
     * numbers mean so the operator measures against the same convention. */
    *wx = ux * cp - uy * sp;
    *wy = ux * sp + uy * cp;
    *wz = uz;
}

bool geometry_solve_point(const geometry_cal_t *cal,
                           float px, float py,
                           uint16_t frame_w, uint16_t frame_h,
                           geometry_result_t *out)
{
    if (out == NULL || cal == NULL) return false;
    out->valid = false;
    out->clamped = false;
    out->pan = 0.0f;
    out->tilt = 0.0f;
    out->world_x = 0.0f;
    out->world_y = 0.0f;

    if (frame_w == 0 || frame_h == 0) return false;
    if (cal->focal_px <= 1.0f || cal->cam_height <= 0.0f) return false;

    /* The principal point is the frame centre. */
    float cx = (float)frame_w * 0.5f;
    float cy = (float)frame_h * 0.5f;

    /* Camera-frame ray direction. The +1 on z is the "one unit forward"
     * convention: the ray is scaled so that z = 1 at the focal plane. */
    float dx = (px * (float)frame_w - cx) / cal->focal_px;
    float dy = (py * (float)frame_h - cy) / cal->focal_px;
    float dz = 1.0f;

    float wx, wy, wz;
    prv_world_ray(dx, dy, dz, cal->cam_pitch_deg, cal->cam_yaw_deg,
                  &wx, &wy, &wz);

    /* Intersect with the floor plane z = 0. The camera is at cam_height above
     * it. A ray that is level or pointing up never lands. */
    if (wz >= -1e-4f) return false;
    float t = -cal->cam_height / wz;
    if (t <= 0.0f) return false;

    float px_world = cal->cam_x + t * wx;
    float py_world = cal->cam_y + t * wy;

    /* Aim the nozzle at that point. The nozzle's own height matters: a boom
     * that sprays down from 500 mm needs a different tilt than one at 200 mm
     * for the same spot. */
    float ox = px_world - cal->nozzle_x;
    float oy = py_world - cal->nozzle_y;
    float horiz = sqrtf(ox * ox + oy * oy);

    float pan  = atan2f(ox, oy) * 180.0f / (float)M_PI;
    float tilt = atan2f(cal->nozzle_height, horiz) * 180.0f / (float)M_PI;

    float pan_c  = prv_clamp(pan,  cal->pan_min,  cal->pan_max);
    float tilt_c = prv_clamp(tilt, cal->tilt_min, cal->tilt_max);
    out->clamped = (pan_c != pan) || (tilt_c != tilt);

    out->pan = pan_c;
    out->tilt = tilt_c;
    out->world_x = px_world;
    out->world_y = py_world;
    out->valid = true;
    return true;
}

bool geometry_solve(const geometry_cal_t *cal,
                    float nx, float ny, float nw, float nh,
                    uint16_t frame_w, uint16_t frame_h,
                    geometry_result_t *out)
{
    if (out == NULL || cal == NULL) return false;
    out->valid = false;
    out->clamped = false;

    /* Aim at the CENTRE of the box, not its corner. A box is a region of
     * floor; its corner is the edge of the feces, not the middle of it. */
    float cx = nx + nw * 0.5f;
    float cy = ny + nh * 0.5f;

    return geometry_solve_point(cal, cx, cy, frame_w, frame_h, out);
}
