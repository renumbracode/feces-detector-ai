/*
 * Host-side test for the geometry solver.
 *
 * The solver is the part of the actuator board most likely to be wrong, and
 * it is a PURE function precisely so it can be checked here -- on a laptop,
 * without a board, a servo or a pig pen.
 *
 * Build and run:
 *   scripts\test_geometry.cmd      (Windows)
 *   scripts/test_geometry.sh       (POSIX)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "geometry.h"

static int g_fail = 0;
static int g_pass = 0;

static void ok(const char *what, int cond)
{
    if (cond) {
        g_pass++;
        printf("  PASS  %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL  %s\n", what);
    }
}

static void ok_close(const char *what, float got, float want, float tol)
{
    int good = fabsf(got - want) <= tol;
    if (good) {
        g_pass++;
        printf("  PASS  %s (got %.3f, want %.3f)\n", what, (double)got, (double)want);
    } else {
        g_fail++;
        printf("  FAIL  %s (got %.3f, want %.3f, tol %.3f)\n",
               what, (double)got, (double)want, (double)tol);
    }
}

/* A plausible default: camera 2 m up, 2 m in front of the pen centre,
 * looking down 45 degrees, nozzle on the floor at the pen centre. */
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

int main(void)
{
    printf("geometry solver tests\n");
    geometry_cal_t cal = cal_default();
    geometry_result_t g;

    printf("\n[1] the centre of the frame lands straight ahead\n");
    /* At pitch 45 the centre ray is 45 degrees below horizontal, so from a
     * camera 2000 mm up it travels 2000/sin45 = 2828 mm along the ray, of
     * which the horizontal part is 2000 mm. Camera at y=2000, so the point
     * on the floor is at y = 2000 + 2000 = 4000. */
    ok("centre pixel solves", geometry_solve_point(&cal, 0.5f, 0.5f, 640, 480, &g));
    ok_close("centre world x", g.world_x, 0.0f, 1.0f);
    ok_close("centre world y", g.world_y, 4000.0f, 1.0f);
    ok_close("centre pan", g.pan, 0.0f, 0.2f);
    /* The nozzle is 1000 mm up and 4000 mm away horizontally. */
    ok_close("centre tilt", g.tilt, atan2f(1000.0f, 4000.0f) * 180.0f / 3.14159265f, 0.3f);

    printf("\n[2] a pixel above the centre is FARTHER away\n");
    geometry_result_t far;
    geometry_result_t near;
    ok("upper pixel solves", geometry_solve_point(&cal, 0.5f, 0.2f, 640, 480, &far));
    ok("lower pixel solves", geometry_solve_point(&cal, 0.5f, 0.8f, 640, 480, &near));
    ok("upper is farther than lower", far.world_y > near.world_y);
    ok("upper is beyond the centre", far.world_y > 4000.0f);
    ok("lower is nearer than the centre", near.world_y < 4000.0f);

    printf("\n[3] a pixel left of centre lands left of the axis\n");
    geometry_result_t left, right;
    geometry_solve_point(&cal, 0.3f, 0.5f, 640, 480, &left);
    geometry_solve_point(&cal, 0.7f, 0.5f, 640, 480, &right);
    ok("left pixel has negative x", left.world_x < 0.0f);
    ok("right pixel has positive x", right.world_x > 0.0f);

    printf("\n[4] a ray above the horizon does not solve\n");
    /* Point the camera straight up: nothing lands on the floor. */
    geometry_cal_t up = cal;
    up.cam_pitch_deg = -10.0f;
    ok("upward ray is rejected",
       !geometry_solve_point(&up, 0.5f, 0.0f, 640, 480, &g));
    ok("rejected result is marked invalid", !g.valid);

    printf("\n[5] broken calibration is rejected rather than acted on\n");
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

    printf("\n[6] servo limits are respected\n");
    geometry_cal_t narrow = cal;
    narrow.pan_min = -10.0f;
    narrow.pan_max = 10.0f;
    geometry_result_t lim;
    geometry_solve_point(&narrow, 0.05f, 0.5f, 640, 480, &lim);
    ok("pan is clamped to the limit", lim.pan >= narrow.pan_min - 0.01f);
    ok("clamping is reported", lim.clamped);

    printf("\n[7] the box CENTRE is aimed at, not its corner\n");
    /* A box whose top-left is at (0.4, 0.4) with size 0.2x0.2 has its centre
     * at (0.5, 0.5) -- which must solve to exactly the centre pixel's point. */
    geometry_result_t box;
    ok("box solves", geometry_solve(&cal, 0.4f, 0.4f, 0.2f, 0.2f, 640, 480, &box));
    ok_close("box centre matches the centre pixel", box.world_y, 4000.0f, 1.0f);

    printf("\n[8] yaw rotates the result about the camera\n");
    geometry_cal_t yawed = cal;
    yawed.cam_yaw_deg = 90.0f;
    geometry_result_t y;
    geometry_solve_point(&yawed, 0.5f, 0.5f, 640, 480, &y);
    /* Looking 90 degrees to the left (-X) from a camera at x=0: a POSITIVE
     * yaw turns the camera's +Y direction toward -X (right-hand rule about
     * +Z), so the point lands at x = -2000, y = 2000 (the camera's own y). */
    ok_close("yawed world x", y.world_x, -2000.0f, 2.0f);
    ok_close("yawed world y", y.world_y, 2000.0f, 2.0f);

    printf("\n[9] the pen calibration maps the picture onto the pen\n");
    /* The pen is a known 20 x 30 cm rectangle, and the camera sits at one
     * end of it looking down the length. The matrix below was fitted to the
     * four corners of such a view: a camera 350 mm above the floor, 200 mm
     * behind the pen's near edge, looking down 45 degrees. */
    geometry_pen_t pen;
    memset(&pen, 0, sizeof(pen));
    pen.present = true;
    pen.h[0] = 0.022269846f;  pen.h[1] = 0.0090909091f;
    pen.h[2] = 0.2773015404f;  pen.h[3] = 0.0f;
    pen.h[4] = -0.011905303f;  pen.h[5] = 0.8149431818f;
    pen.h[6] = 0.0f;          pen.h[7] = 0.0181818182f;
    pen.width_cm = 20.0f;
    pen.length_cm = 30.0f;
    pen.nozzle_x_cm = 10.0f;      /* over the middle of the pen */
    pen.nozzle_y_cm = 30.0f;      /* at the far end, facing back down it */
    pen.nozzle_height_mm = 225.0f;
    pen.pan_offset_deg = 35.0f;   /* the travel's midpoint aims at the middle */
    pen.tilt_offset_deg = 35.0f;  /* the travel's far end points straight down */
    pen.pan_dir = 1.0f;
    pen.tilt_dir = 1.0f;
    pen.pan_min = 0.0f;  pen.pan_max = 70.0f;
    pen.tilt_min = 70.0f; pen.tilt_max = 125.0f;

    /* The pen's corners, as the picture sees them. */
    {
        float cx = 0.0f, cy = 0.0f;
        ok("near-left corner solves",
           geometry_pen_image_to_cm(&pen, 0.2773f, 0.8149f, &cx, &cy));
        ok_close("near-left cm x", cx, 0.0f, 0.05f);
        ok_close("near-left cm y", cy, 0.0f, 0.05f);

        ok("far-right corner solves",
           geometry_pen_image_to_cm(&pen, 0.6441f, 0.2962f, &cx, &cy));
        ok_close("far-right cm x", cx, 20.0f, 0.05f);
        ok_close("far-right cm y", cy, 30.0f, 0.05f);

        ok("centre solves",
           geometry_pen_image_to_cm(&pen, 0.5f, 0.5f, &cx, &cy));
        ok_close("centre cm x", cx, 10.0f, 0.05f);
        ok_close("centre cm y", cy, 15.0f, 0.05f);
    }

    printf("\n[10] aiming with the pen\n");
    {
        geometry_result_t p;
        /* The pen's centre, at the picture's centre. */
        ok("centre box solves", geometry_solve_pen(&pen, 0.49f, 0.49f, 0.02f, 0.02f, &p));
        ok_close("centre pan is the travel's midpoint", p.pan, 35.0f, 0.2f);
        /* 150 mm straight ahead of a nozzle 225 mm up. */
        ok_close("centre tilt",
                 p.tilt, 35.0f + atan2f(225.0f, 150.0f) * 180.0f / 3.14159265f, 0.3f);
        ok("centre is not clamped", !p.clamped);

        /* The far end of the pen: 300 mm away, so the nozzle leans out. */
        ok("far-end box solves", geometry_solve_pen(&pen, 0.49f, 0.8049f, 0.02f, 0.02f, &p));
        ok_close("far-end tilt",
                 p.tilt, 35.0f + atan2f(225.0f, 300.0f) * 180.0f / 3.14159265f, 0.3f);
        ok("far-end tilt is inside the travel", p.tilt >= pen.tilt_min && p.tilt <= pen.tilt_max);

        /* The near end, directly below the nozzle: the steepest aim there
         * is, and the top of the travel. */
        ok("near-end box solves", geometry_solve_pen(&pen, 0.49f, 0.2862f, 0.02f, 0.02f, &p));
        ok_close("near-end tilt is the travel's top end", p.tilt, 125.0f, 0.3f);

        /* The picture's left is the servo's left. */
        geometry_result_t lft;
        geometry_result_t rgt;
        geometry_solve_pen(&pen, 0.30f, 0.49f, 0.02f, 0.02f, &lft);
        geometry_solve_pen(&pen, 0.68f, 0.49f, 0.02f, 0.02f, &rgt);
        ok("the picture's left is the larger pan", lft.pan > rgt.pan);
        ok("both pans are inside the travel",
           lft.pan <= pen.pan_max && rgt.pan >= pen.pan_min);
    }

    printf("\n[11] a missing or broken pen calibration is refused\n");
    {
        geometry_result_t p;
        geometry_pen_t none = pen;
        none.present = false;
        ok("an absent calibration is refused",
           !geometry_solve_pen(&none, 0.5f, 0.5f, 0.02f, 0.02f, &p));

        /* Four points in a line: the matrix cannot be inverted. */
        float degenerate[8] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f };
        float inv[8];
        ok("a degenerate matrix cannot be inverted",
           !geometry_invert_homography(degenerate, inv));

        /* And a good matrix round-trips through its own inverse. */
        float back[8];
        ok("a good matrix inverts", geometry_invert_homography(pen.h, back));
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
