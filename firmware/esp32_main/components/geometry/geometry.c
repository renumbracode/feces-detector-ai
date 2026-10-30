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

/* ---------------- the pen, as the calibration target ---------------- */

/* The inverse of the 3x3 homography.
 *
 * The matrix is
 *     [ h0 h1 h2 ]
 *     [ h3 h4 h5 ]
 *     [ h6 h7  1 ]
 * and the inverse comes from the adjugate divided by the determinant, then
 * renormalised so its bottom-right term is 1 again -- the same convention
 * the matrix itself is stored in.
 *
 * The adjugate is the TRANSPOSE of the cofactor matrix, so the inverse's
 * first row is the cofactor matrix's first COLUMN. Getting that backwards
 * silently swaps the pen's two axes, which is exactly the kind of mistake
 * that produces plausible-looking wrong angles. */
bool geometry_invert_homography(const float h[8], float out[8])
{
    if (h == NULL || out == NULL) return false;

    const float a = h[0], b = h[1], c = h[2];
    const float d = h[3], e = h[4], f = h[5];
    const float g = h[6], i = h[7];
    const float m22 = 1.0f;

    /* Cofactors. */
    const float c00 = e * m22 - f * i;
    const float c01 = -(d * m22 - f * g);
    const float c02 = d * i - e * g;
    const float c10 = -(b * m22 - c * i);
    const float c11 = a * m22 - c * g;
    const float c12 = -(a * i - b * g);
    const float c20 = b * f - c * e;
    const float c21 = -(a * f - c * d);
    const float c22 = a * e - b * d;

    const float det = a * c00 + b * c01 + c * c02;
    if (fabsf(det) < 1e-12f) return false;

    float inv[9];
    inv[0] = c00 / det; inv[1] = c10 / det; inv[2] = c20 / det;
    inv[3] = c01 / det; inv[4] = c11 / det; inv[5] = c21 / det;
    inv[6] = c02 / det; inv[7] = c12 / det; inv[8] = c22 / det;

    /* Renormalise so the bottom-right term is 1. */
    const float k = inv[8];
    if (fabsf(k) < 1e-12f) return false;

    out[0] = inv[0] / k; out[1] = inv[1] / k; out[2] = inv[2] / k;
    out[3] = inv[3] / k; out[4] = inv[4] / k; out[5] = inv[5] / k;
    out[6] = inv[6] / k; out[7] = inv[7] / k;
    return true;
}

bool geometry_pen_image_to_cm(const geometry_pen_t *cal,
                              float u, float v,
                              float *cm_x, float *cm_y)
{
    if (cal == NULL || cm_x == NULL || cm_y == NULL || !cal->fitted) return false;

    float g[8];
    if (!geometry_invert_homography(cal->h, g)) return false;

    const float w = g[6] * u + g[7] * v + 1.0f;
    if (fabsf(w) < 1e-9f) return false;

    *cm_x = (g[0] * u + g[1] * v + g[2]) / w;
    *cm_y = (g[3] * u + g[4] * v + g[5]) / w;
    return true;
}

bool geometry_solve_pen(const geometry_pen_t *cal,
                        float nx, float ny, float nw, float nh,
                        geometry_result_t *out)
{
    if (out == NULL || cal == NULL || !cal->present) return false;
    out->valid = false;
    out->clamped = false;
    out->pan = 0.0f;
    out->tilt = 0.0f;
    out->world_x = 0.0f;
    out->world_y = 0.0f;

    /* Aim at the CENTRE of the box, not its corner. */
    const float u = nx + nw * 0.5f;
    const float v = ny + nh * 0.5f;

    float cx = 0.0f, cy = 0.0f;
    if (cal->fitted) {
        if (!geometry_pen_image_to_cm(cal, u, v, &cx, &cy)) return false;
    } else {
        /* The LINEAR model: the camera looks straight down, so the picture
         * IS the pen and every centimetre is the same size as every other
         * one. No fitting, no matrix, nothing to get wrong -- just the pen's
         * size and which end of the picture the camera is at.
         *
         * The picture's LEFT is the pen's left, and the picture's BOTTOM is
         * the pen's near end (the end the camera is at), so the depth runs
         * the other way up the picture. */
        if (cal->width_cm <= 0.0f || cal->length_cm <= 0.0f) return false;
        cx = u * cal->width_cm;
        cy = (cal->flip_y ? v : 1.0f - v) * cal->length_cm;
    }

    /* The nozzle faces BACK toward the camera, so its forward direction is
     * -Y in the pen frame and everything in front of it has y < nozzle_y. */
    float fwd = cal->nozzle_y_cm - cy;
    if (fwd < -0.05f) {
        /* Behind the nozzle by more than half a millimetre: no amount of
         * servo travel reaches it.
         *
         * The tolerance matters: the pen's far edge sits exactly where the
         * nozzle is, and the round trip through the matrix can land a
         * thousandth of a centimetre past it. Without the tolerance the
         * spot DIRECTLY under the nozzle -- the steadiest aim there is, and
         * the top of the travel -- would be refused as unreachable. */
        return false;
    }
    if (fwd < 0.0f) {
        fwd = 0.0f;
    }
    /* +X is to the camera's right. The nozzle faces the other way, so +X is
     * to the NOZZLE'S LEFT -- and pan_dir says whether that is the same as
     * the picture's left, which it is not by default. */
    const float lat = cx - cal->nozzle_x_cm;

    /* The horizontal distance in MILLIMETRES: the nozzle's height is in mm
     * and the pen is in cm, and mixing the two makes the tilt wrong by
     * exactly a factor of ten. */
    const float horiz_mm = 10.0f * sqrtf(lat * lat + fwd * fwd);
    /* The angle above the horizontal from the nozzle down to the spot: 90
     * degrees straight down, small when the spot is far away. */
    const float tilt_geom = cal->tilt_dir
                            * atan2f(cal->nozzle_height_mm, horiz_mm)
                            * 180.0f / (float)M_PI;
    /* Off the nozzle's own axis. Positive means to the nozzle's left. */
    const float pan_geom = -cal->pan_dir
                           * atan2f(lat, fwd) * 180.0f / (float)M_PI;

    const float pan_want = cal->pan_offset_deg + pan_geom;
    const float tilt_want = cal->tilt_offset_deg + tilt_geom;

    const float pan = prv_clamp(pan_want, cal->pan_min, cal->pan_max);
    const float tilt = prv_clamp(tilt_want, cal->tilt_min, cal->tilt_max);
    out->clamped = (pan != pan_want) || (tilt != tilt_want);

    out->pan = pan;
    out->tilt = tilt;
    /* The pen's own centimetres, as millimetres, for the record. */
    out->world_x = cx * 10.0f;
    out->world_y = cy * 10.0f;
    out->valid = true;
    return true;
}
