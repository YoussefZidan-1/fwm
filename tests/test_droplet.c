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

/* droplet.c is arithmetic over doubles — no wlroots, no GL, no window. Like
 * the wobble it only exists while a mouse button is down, and unlike the wobble
 * its failures are silent: a lattice that overshoots its buffer is clipped by
 * the edge, and one that folds through itself draws the window inside out. Both
 * look like a bad frame and neither reaches a log. So they are asserted here. */

#include "test.h"
#include "droplet.h"

#define GRID 9
#define POINTS (GRID * GRID)

#define SLOT_W 900
#define SLOT_H 700
#define DROP_W 500
#define DROP_H 300
#define DT (1.0 / 60.0)

/* The buffer the mesh is drawn into is the window plus this on every side (see
 * JELLY_MARGIN_MIN in view_effects.c). Nothing may leave it. */
#define MARGIN 48

static void identity(float *pts, double w, double h) {
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            int k = j * GRID + i;
            pts[k * 2 + 0] = (float)(w * i / (GRID - 1));
            pts[k * 2 + 1] = (float)(h * j / (GRID - 1));
        }
    }
}

static void test_round_identity(void) {
    CASE("amount 0 is the window's own rectangle");
    float pts[POINTS * 2], before[POINTS * 2];
    identity(pts, SLOT_W, SLOT_H);
    memcpy(before, pts, sizeof(pts));

    droplet_round(pts, GRID, SLOT_W, SLOT_H, 0.0);
    for (int k = 0; k < POINTS * 2; k++) CHECK_DBL(pts[k], before[k], 1e-6);
}

/* The one thing "round" means, asserted rather than described: at full amount
 * every point of the mesh's OUTLINE is the same distance from the centre. Not
 * an ellipse of the window's proportions, which is what a per-axis scale would
 * have given and what a wide window would have made obvious. */
static void test_round_is_a_circle(void) {
    CASE("a fully rounded drop is a circle, whatever shape the window was");
    const double boxes[][2] = {
        {SLOT_W, SLOT_H},   /* wide */
        {300, 900},         /* tall */
        {600, 600},         /* already square */
    };

    for (size_t b = 0; b < sizeof(boxes) / sizeof(boxes[0]); b++) {
        double w = boxes[b][0], h = boxes[b][1];
        double r = (w < h ? w : h) / 2.0;

        float pts[POINTS * 2];
        identity(pts, w, h);
        droplet_round(pts, GRID, w, h, 1.0);

        for (int j = 0; j < GRID; j++) {
            for (int i = 0; i < GRID; i++) {
                bool edge = (i == 0 || i == GRID - 1 || j == 0 || j == GRID - 1);
                if (!edge) continue;
                int k = j * GRID + i;
                double d = hypot(pts[k * 2 + 0] - w / 2.0, pts[k * 2 + 1] - h / 2.0);
                CHECK_DBL(d, r, 1e-3);
            }
        }
    }
}

static void test_round_stays_inside_the_window(void) {
    CASE("rounding only ever pulls inward");
    float pts[POINTS * 2];
    identity(pts, SLOT_W, SLOT_H);
    droplet_round(pts, GRID, SLOT_W, SLOT_H, 1.0);

    /* The corner comes in on both axes, unmistakably. */
    CHECK(pts[0] > SLOT_W * 0.10);
    CHECK(pts[1] > SLOT_H * 0.10);
    CHECK(pts[0] < SLOT_W / 2.0);
    CHECK(pts[1] < SLOT_H / 2.0);

    /* And nothing ever moves OUTWARD: a drop is the window pulled in, and a
     * mesh that bulged past its own box would be clipped by the buffer's edge. */
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            int k = j * GRID + i;
            CHECK(pts[k * 2 + 0] >= -1e-3 && pts[k * 2 + 0] <= SLOT_W + 1e-3);
            CHECK(pts[k * 2 + 1] >= -1e-3 && pts[k * 2 + 1] <= SLOT_H + 1e-3);
        }
    }
}

static void test_round_keeps_the_wobble(void) {
    CASE("rounding is a displacement, so a bent sheet stays bent");
    float flat[POINTS * 2], bent[POINTS * 2];
    identity(flat, SLOT_W, SLOT_H);
    identity(bent, SLOT_W, SLOT_H);
    /* One point dragged well off its rest position, as a wobble would. */
    bent[0] += 37.0f;
    bent[1] -= 21.0f;

    droplet_round(flat, GRID, SLOT_W, SLOT_H, 0.7);
    droplet_round(bent, GRID, SLOT_W, SLOT_H, 0.7);

    CHECK_DBL(bent[0] - flat[0], 37.0, 1e-3);
    CHECK_DBL(bent[1] - flat[1], -21.0, 1e-3);
}

static void test_fill_ends_flush(void) {
    CASE("a finished fill is the slot, exactly");
    DropletFill f;
    droplet_fill_begin(&f, SLOT_W, SLOT_H, DROP_W, DROP_H, 120.0, 90.0, 0.4);

    while (droplet_fill_step(&f, DT)) { }
    CHECK_DBL(f.t, 1.0, 1e-9);

    float pts[POINTS * 2];
    droplet_fill_points(&f, pts, GRID, MARGIN, MARGIN);
    CHECK_DBL(pts[0], MARGIN, 1e-3);
    CHECK_DBL(pts[1], MARGIN, 1e-3);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 0], MARGIN + SLOT_W, 1e-3);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 1], MARGIN + SLOT_H, 1e-3);
}

static void test_fill_starts_at_the_cursor(void) {
    CASE("a fill starts as the drop, where the hand let go");
    DropletFill f;
    droplet_fill_begin(&f, SLOT_W, SLOT_H, DROP_W, DROP_H, 700.0, 120.0, 0.4);
    /* Pulled in far enough that the whole drop is inside the slot — 120 is less
     * than half the drop's height from the top edge. */
    double ox = f.ox, oy = f.oy;
    CHECK_DBL(ox, 650.0, 1e-9);
    CHECK_DBL(oy, DROP_H / 2.0, 1e-9);

    float pts[POINTS * 2];
    droplet_fill_points(&f, pts, GRID, 0.0, 0.0);

    /* It has not started spreading, so this is the drop itself: a circle of the
     * drop's smaller half-side, centred where the hand let go. */
    double r = DROP_H / 2.0;   /* the drop is wider than it is tall */
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            int k = j * GRID + i;
            double d = hypot(pts[k * 2 + 0] - ox, pts[k * 2 + 1] - oy);
            CHECK(d <= r + 1e-3);
            if (i == 0 || i == GRID - 1 || j == 0 || j == GRID - 1) {
                CHECK_DBL(d, r, 1e-3);
            }
        }
    }
}

static void test_fill_near_corner_leads(void) {
    CASE("the near corner arrives before the far one");
    /* Let go in the top-left: that corner has the least distance to cover and
     * the least delay, so it must be home while the opposite one is still out. */
    DropletFill f;
    droplet_fill_begin(&f, SLOT_W, SLOT_H, DROP_W, DROP_H, 40.0, 40.0, 0.4);

    for (int i = 0; i < 14; i++) droplet_fill_step(&f, DT);

    float pts[POINTS * 2];
    droplet_fill_points(&f, pts, GRID, 0.0, 0.0);

    double near_err = hypot(pts[0], pts[1]);
    double far_err  = hypot(pts[(POINTS - 1) * 2 + 0] - SLOT_W,
                            pts[(POINTS - 1) * 2 + 1] - SLOT_H);
    CHECK(near_err < far_err);
}

/* The one that would be invisible in review and obvious on screen: every point
 * of every frame of a fill has to stay inside the buffer it is drawn into, and
 * the grid has to stay a grid — rows and columns in order, never folded back
 * through each other. */
static void test_fill_stays_in_its_buffer(void) {
    CASE("no frame of a fill leaves the buffer or folds over");
    const double origins[][2] = {
        {0.0, 0.0}, {SLOT_W, SLOT_H}, {SLOT_W / 2.0, SLOT_H / 2.0},
        /* Let go outside the slot entirely — dropping on the gap between two
         * tiles puts the cursor in neither of them. */
        {-200.0, -150.0}, {SLOT_W + 300.0, SLOT_H / 2.0},
    };

    /* Including a slot smaller than the drop itself, which is what a five-way
     * split leaves you dropping into. */
    const double slots[][2] = { {SLOT_W, SLOT_H}, {240.0, 160.0} };

    for (size_t s = 0; s < sizeof(slots) / sizeof(slots[0]); s++)
    for (size_t o = 0; o < sizeof(origins) / sizeof(origins[0]); o++) {
        double sw = slots[s][0], sh = slots[s][1];
        DropletFill f;
        droplet_fill_begin(&f, sw, sh, DROP_W, DROP_H,
                           origins[o][0], origins[o][1], 0.4);
        bool more = true;
        while (more) {
            float pts[POINTS * 2];
            droplet_fill_points(&f, pts, GRID, MARGIN, MARGIN);

            for (int j = 0; j < GRID; j++) {
                for (int i = 0; i < GRID; i++) {
                    int k = j * GRID + i;
                    CHECK(pts[k * 2 + 0] >= 0.0 && pts[k * 2 + 0] <= sw + 2 * MARGIN);
                    CHECK(pts[k * 2 + 1] >= 0.0 && pts[k * 2 + 1] <= sh + 2 * MARGIN);
                    /* Monotone across and down: the mesh may bend as much as it
                     * likes, but a column that has overtaken the one to its
                     * left is a picture turned inside out. */
                    if (i > 0) CHECK(pts[k * 2 + 0] >= pts[(k - 1) * 2 + 0] - 1e-3);
                    if (j > 0) CHECK(pts[k * 2 + 1] >= pts[(k - GRID) * 2 + 1] - 1e-3);
                }
            }
            more = droplet_fill_step(&f, DT);
        }
    }
}

static void test_fill_retarget(void) {
    CASE("a slot that changed size is re-aimed, not restarted");
    DropletFill f;
    droplet_fill_begin(&f, SLOT_W, SLOT_H, DROP_W, DROP_H, 100.0, 100.0, 0.4);
    for (int i = 0; i < 10; i++) droplet_fill_step(&f, DT);
    double t = f.t;

    droplet_fill_retarget(&f, SLOT_W - 13, SLOT_H - 7);
    CHECK_DBL(f.t, t, 1e-9);

    while (droplet_fill_step(&f, DT)) { }
    float pts[POINTS * 2];
    droplet_fill_points(&f, pts, GRID, 0.0, 0.0);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 0], SLOT_W - 13, 1e-3);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 1], SLOT_H - 7, 1e-3);
}

static void test_fill_long_frame(void) {
    CASE("one very long frame finishes the fill instead of overrunning it");
    DropletFill f;
    droplet_fill_begin(&f, SLOT_W, SLOT_H, DROP_W, DROP_H, 100.0, 100.0, 0.4);
    CHECK(!droplet_fill_step(&f, 5.0));
    CHECK_DBL(f.t, 1.0, 1e-9);

    float pts[POINTS * 2];
    droplet_fill_points(&f, pts, GRID, 0.0, 0.0);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 0], SLOT_W, 1e-3);
    CHECK_DBL(pts[(POINTS - 1) * 2 + 1], SLOT_H, 1e-3);
}

int main(void) {
    test_round_identity();
    test_round_is_a_circle();
    test_round_stays_inside_the_window();
    test_round_keeps_the_wobble();
    test_fill_ends_flush();
    test_fill_starts_at_the_cursor();
    test_fill_near_corner_leads();
    test_fill_stays_in_its_buffer();
    test_fill_retarget();
    test_fill_long_frame();
    return t_report("droplet");
}
