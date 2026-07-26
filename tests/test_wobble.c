/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/* wobble.c is a spring lattice over doubles — no wlroots, no GL, no window.
 * The drag wobble is the one effect nobody can eyeball into correctness (it
 * only exists while a mouse button is down), so what it must never do is
 * asserted here instead: blow up, tear, or fail to come back to shape. */

#include "test.h"
#include "wobble.h"

#define W 800
#define H 600
#define DT (1.0 / 60.0)

static double dist_from_home(const Wobble *wb, int i, int j) {
    int k = j * WOBBLE_GRID + i;
    double hx = (double)wb->w * i / (WOBBLE_GRID - 1);
    double hy = (double)wb->h * j / (WOBBLE_GRID - 1);
    return hypot(wb->px[k] - hx, wb->py[k] - hy);
}

/* Drag the window `dx` px per frame for `frames` frames. */
static void drag(Wobble *wb, double dx, double dy, int frames) {
    for (int i = 0; i < frames; i++) {
        wobble_translate(wb, dx, dy);
        wobble_step(wb, DT);
    }
}

static void test_rest(void) {
    CASE("at rest");
    Wobble wb;
    wobble_reset(&wb, W, H);
    CHECK(wobble_at_rest(&wb));
    CHECK_DBL(wobble_max_offset(&wb), 0.0, 1e-9);

    /* The corners are the box, exactly. */
    CHECK_DBL(wb.px[0], 0.0, 1e-9);
    CHECK_DBL(wb.py[0], 0.0, 1e-9);
    CHECK_DBL(wb.px[WOBBLE_POINTS - 1], (double)W, 1e-9);
    CHECK_DBL(wb.py[WOBBLE_POINTS - 1], (double)H, 1e-9);

    /* Standing still is not a wobble, however long you wait. */
    wobble_step(&wb, DT * 100);
    CHECK_DBL(wobble_max_offset(&wb), 0.0, 1e-9);
}

static void test_lag_and_return(void) {
    CASE("lags, then comes back");
    Wobble wb;
    wobble_reset(&wb, W, H);

    drag(&wb, 1200.0 * DT, 0.0, 60);        /* a brisk one-second drag */
    double lag = wobble_max_offset(&wb);
    CHECK(lag > 20.0);                      /* it visibly trails ... */
    CHECK(lag < 300.0);                     /* ... without coming apart */
    CHECK(!wobble_at_rest(&wb));

    /* Let go and it settles, in a time a person would call "a moment". */
    int frames = 0;
    while (!wobble_at_rest(&wb) && frames < 600) { wobble_step(&wb, DT); frames++; }
    CHECK(wobble_at_rest(&wb));
    CHECK(frames < 120);                    /* under two seconds */
    CHECK_DBL(wobble_max_offset(&wb), 0.0, 1.0);
}

static void test_grab_point_leads(void) {
    CASE("the held corner leads");
    Wobble wb;
    wobble_reset(&wb, W, H);
    wobble_grab(&wb, 0, 0);                 /* held by the top-left */

    drag(&wb, 900.0 * DT, 0.0, 30);

    /* The point in your hand is exactly where the cursor put it; the far
     * corner is the one that arrives late. That difference IS the bend. */
    CHECK_DBL(dist_from_home(&wb, 0, 0), 0.0, 1e-9);
    CHECK(dist_from_home(&wb, WOBBLE_GRID - 1, WOBBLE_GRID - 1) > 10.0);
    CHECK(dist_from_home(&wb, WOBBLE_GRID - 1, WOBBLE_GRID - 1)
          > dist_from_home(&wb, 1, 1));
}

static void test_shake_stays_sane(void) {
    CASE("a hard shake does not explode");
    Wobble wb;
    wobble_reset(&wb, W, H);
    wobble_grab(&wb, W / 2.0, 0);

    /* 400px each way at 5Hz for four seconds — far past what a hand does. */
    double prev = 0.0;
    for (int i = 0; i < 240; i++) {
        double t = i * DT, tn = (i + 1) * DT;
        double now = 400.0 * sin(2 * M_PI * 5.0 * tn);
        wobble_translate(&wb, now - prev, 0.0);
        prev = now;
        wobble_step(&wb, DT);
        (void)t;

        double off = wobble_max_offset(&wb);
        CHECK(off == off);                  /* not NaN */
        CHECK(off < 4.0 * W);               /* still recognisably a window */
    }

    int frames = 0;
    while (!wobble_at_rest(&wb) && frames < 900) { wobble_step(&wb, DT); frames++; }
    CHECK(wobble_at_rest(&wb));
}

static void test_long_frame(void) {
    CASE("a dropped frame does not detonate it");
    Wobble wb;
    wobble_reset(&wb, W, H);
    drag(&wb, 20.0, 0.0, 20);
    double before = wobble_max_offset(&wb);

    /* server_animate clamps a stall at 0.25s and hands it over as one dt.
     * Forward Euler on this lattice is unstable at that step size, so the
     * sub-stepping inside wobble_step is the only thing standing between a
     * hitch and a window turned inside out. */
    wobble_translate(&wb, 20.0, 0.0);
    wobble_step(&wb, 0.25);
    double after = wobble_max_offset(&wb);
    CHECK(after == after);
    CHECK(after < before + 100.0);
}

static void test_resize_keeps_shape(void) {
    CASE("resize carries the deformation");
    Wobble wb;
    wobble_reset(&wb, W, H);
    drag(&wb, 15.0, 0.0, 20);
    double before = wobble_max_offset(&wb);
    CHECK(before > 1.0);

    wobble_resize(&wb, W * 2, H);
    CHECK_INT(wb.w, W * 2);
    /* Still bent — a resize mid-wobble must not snap the window straight. */
    CHECK(wobble_max_offset(&wb) > 1.0);
    /* And the corners still describe the new box. */
    CHECK_DBL(wb.px[WOBBLE_POINTS - 1] - wb.px[0], (double)(W * 2), 200.0);
}

static void test_points_out(void) {
    CASE("points come out with the margin applied");
    Wobble wb;
    wobble_reset(&wb, W, H);
    float pts[WOBBLE_POINTS * 2];
    wobble_points(&wb, pts, 40.0, 25.0);
    CHECK_DBL(pts[0], 40.0, 1e-3);
    CHECK_DBL(pts[1], 25.0, 1e-3);
    CHECK_DBL(pts[(WOBBLE_POINTS - 1) * 2 + 0], W + 40.0, 1e-3);
    CHECK_DBL(pts[(WOBBLE_POINTS - 1) * 2 + 1], H + 25.0, 1e-3);
}

int main(void) {
    test_rest();
    test_lag_and_return();
    test_grab_point_leads();
    test_shake_stays_sane();
    test_long_frame();
    test_resize_keeps_shape();
    test_points_out();
    return t_report("wobble");
}
