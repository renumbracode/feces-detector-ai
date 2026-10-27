/*
 * Self-test for the geometry solver.
 *
 * The solver is the part of the actuator board most likely to be wrong -- a
 * transposed sign or a wrong axis sends the nozzle to the wrong place, and
 * the failure looks like "the model was wrong" rather than "the maths was
 * wrong". So it is checked, on the real target, with the real compiler.
 *
 * GET /selftest runs it and reports the result. The same assertions exist as
 * a host test in test/test_geometry.c for anyone with gcc, and the two are
 * kept in step deliberately: the expected numbers below are the SPEC.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "geometry.h"
#include "geometry_selftest.h"

#define SELFTEST_BUF 1024

static int s_pass = 0;
static int s_fail = 0;
static char s_first_failure[192];

static void ok(const char *what, int cond)
{
    if (cond) {
        s_pass++;
    } else if (s_fail == 0) {
        s_fail++;
        snprintf(s_first_failure, sizeof(s_first_failure), "%s", what);
    }
}

static void ok_close(const char *what, float got, float want, float tol)
{
    if (fabsf(got - want) <= tol) {
        s_pass++;
    } else if (s_fail == 0) {
        s_fail++;
        snprintf(s_first_failure, sizeof(s_first_failure),
                 "%s (got %.3f, want %.3f)", what, (double)got, (double)want);
    }
}

static geometry_cal_t cal_default(void)
{
    geometry_cal_t c;
    c.cam_x = 0.0f;
    c.cam_y = 2000.0f;
    c.cam_height = 2000.0f;
    c.cam_yaw_deg = 0.0f;
    c.cam_pitch_deg = 45.0f;
    c.focal_px = 640.0f / (2.0f * tanf(30.0f * 3.14159265f / 180.0f));
    c.nozzle_x = 0.0f;
    c.nozzle_y = 0.0f;
    c.nozzle_height = 1000.0f;
    c.pan_min = -90.0f;
    c.pan_max = 90.0f;
    c.tilt_min = 0.0f;
    c.tilt_max = 90.0f;
    return c;
}

void geometry_selftest(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';

    s_pass = 0;
    s_fail = 0;
    s_first_failure[0] = '\0';

    geometry_cal_t cal = cal_default();
    geometry_result_t g, far, near, left, right, lim, box, y;

    /* 1. The centre of the frame lands straight ahead. At pitch 45 the
     * centre ray is 45 degrees below horizontal, so from a camera 2000 mm up
     * it travels 2828 mm along the ray, of which 2000 mm is horizontal:
     * the point on the floor is at y = 2000 + 2000 = 4000. */
    ok("centre pixel solves", geometry_solve_point(&cal, 0.5f, 0.5f, 640, 480, &g));
    ok_close("centre world x", g.world_x, 0.0f, 1.0f);
    ok_close("centre world y", g.world_y, 4000.0f, 1.0f);
    ok_close("centre pan", g.pan, 0.0f, 0.2f);
    ok_close("centre tilt", g.tilt,
             atan2f(1000.0f, 4000.0f) * 180.0f / 3.14159265f, 0.3f);

    /* 2. A pixel above the centre is FARTHER away. This is the whole reason
     * a linear map is wrong. */
    ok("upper pixel solves", geometry_solve_point(&cal, 0.5f, 0.2f, 640, 480, &far));
    ok("lower pixel solves", geometry_solve_point(&cal, 0.5f, 0.8f, 640, 480, &near));
    ok("upper is farther than lower", far.world_y > near.world_y);
    ok("upper is beyond the centre", far.world_y > 4000.0f);
    ok("lower is nearer than the centre", near.world_y < 4000.0f);

    /* 3. A pixel left of centre lands left of the axis. */
    geometry_solve_point(&cal, 0.3f, 0.5f, 640, 480, &left);
    geometry_solve_point(&cal, 0.7f, 0.5f, 640, 480, &right);
    ok("left pixel has negative x", left.world_x < 0.0f);
    ok("right pixel has positive x", right.world_x > 0.0f);

    /* 4. A ray above the horizon does not solve. */
    geometry_cal_t up = cal;
    up.cam_pitch_deg = -10.0f;
    ok("upward ray is rejected",
       !geometry_solve_point(&up, 0.5f, 0.0f, 640, 480, &g));
    ok("rejected result is marked invalid", !g.valid);

    /* 5. Broken calibration is rejected rather than acted on. */
    geometry_cal_t broken = cal;
    broken.focal_px = 0.0f;
    ok("zero focal length is rejected",
       !geometry_solve_point(&broken, 0.5f, 0.5f, 640, 480, &g));
    broken = cal;
    broken.cam_height = 0.0f;
    ok("zero camera height is rejected",
       !geometry_solve_point(&broken, 0.5f, 0.5f, 640, 480, &g));
    ok("zero frame width is rejected",
       !geometry_solve_point(&cal, 0.5f, 0.5f, 0, 480, &g));

    /* 6. Servo limits are respected. */
    geometry_cal_t narrow = cal;
    narrow.pan_min = -10.0f;
    narrow.pan_max = 10.0f;
    geometry_solve_point(&narrow, 0.05f, 0.5f, 640, 480, &lim);
    ok("pan is clamped to the limit", lim.pan >= narrow.pan_min - 0.01f);
    ok("clamping is reported", lim.clamped);

    /* 7. The box CENTRE is aimed at, not its corner. */
    ok("box solves", geometry_solve(&cal, 0.4f, 0.4f, 0.2f, 0.2f, 640, 480, &box));
    ok_close("box centre matches the centre pixel", box.world_y, 4000.0f, 1.0f);

    /* 8. Yaw rotates the result about the camera. A POSITIVE yaw turns the
     * camera's +Y direction toward -X (right-hand rule about +Z), so the
     * point lands at x = -2000, y = 2000 -- the camera's own y, because the
     * ray has no Y component left. */
    geometry_cal_t yawed = cal;
    yawed.cam_yaw_deg = 90.0f;
    geometry_solve_point(&yawed, 0.5f, 0.5f, 640, 480, &y);
    ok_close("yawed world x", y.world_x, -2000.0f, 2.0f);
    ok_close("yawed world y", y.world_y, 2000.0f, 2.0f);

    snprintf(out, cap,
             "{\"ok\":%s,\"passed\":%d,\"failed\":%d,\"firstFailure\":\"%s\"}",
             s_fail == 0 ? "true" : "false", s_pass, s_fail,
             s_first_failure);
}
