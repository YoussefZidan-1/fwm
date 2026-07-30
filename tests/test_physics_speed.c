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

/* What the physics has to keep doing when things move fast.
 *
 * All of it came out of one outside report — "the physics breaks at high
 * speeds" — and every case here is a measurement that used to fail. None of it
 * is checkable by hand: a drag needs a mouse, a nested run cannot inject one,
 * and the failure is three frames long. See docs/physics.md.
 */

#include "test.h"
#include "physics.h"

#include <math.h>

#define SW 1920
#define SH 1080
#define WORLD (10.0 * SW)
#define DT   (1.0 / 60.0)

/* Depth of the intersection of two windows, in px; 0 when they are apart. */
static double overlap_px(const PhysicsBody *a, const PhysicsBody *b) {
    double ox = fmin(a->x + a->width,  b->x + b->width)  - fmax(a->x, b->x);
    double oy = fmin(a->y + a->height, b->y + b->height) - fmax(a->y, b->y);
    if (ox <= 0.0 || oy <= 0.0) return 0.0;
    return fmin(ox, oy);
}

/* Drag window 1 rightward at `speed` into window 2 and report the worst overlap
 * reached, the way server_drag does it: write a position every frame and let
 * physics_step derive the push from it. */
static double drag_into_neighbour(double speed, double *victim_speed) {
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;

    physics_sync_body(&w, 1, 400, 400, 400, 300, SW);
    physics_sync_body(&w, 2, 1400, 400, 400, 300, SW);

    double x = 400.0, worst = 0.0;
    for (int i = 0; i < 120; i++) {
        x += speed * DT;
        physics_sync_body(&w, 1, (int)x, 400, 400, 300, SW);
        physics_step(&w, SW, SH, 0, 0, 1 /* dragged */, DT);
        double ov = overlap_px(physics_find_body(&w, 1), physics_find_body(&w, 2));
        if (ov > worst) worst = ov;
    }
    PhysicsBody *v = physics_find_body(&w, 2);
    if (victim_speed) *victim_speed = hypot(v->vx, v->vy);
    physics_destroy(&w);
    return worst;
}

static void test_drag_does_not_pass_through(void) {
    /* The bug this file exists for. A dragged body is kinematic: its transform is
     * teleported to the cursor while its velocity used to be clamped to 600 px/s,
     * so above that the solver was resolving a contact far deeper than it
     * believed. Measured before the fix: the overlap grew to 295px — three
     * quarters of the window — and the dragged window came out the far side.
     *
     * 3000 px/s is a brisk but ordinary mouse movement; the old ceiling failed at
     * a fifth of it. */
    CASE("a drag shoves, it does not pass through");
    const double speeds[] = { 600.0, 1200.0, 2000.0, 3000.0 };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        double victim = 0.0;
        double worst = drag_into_neighbour(speeds[i], &victim);
        /* A few px of speculative contact are normal and invisible; a quarter of
         * a window is the failure. */
        CHECK(worst < 20.0);
        /* And the shove has to actually move the other window — the old clamp
         * made the push WEAKER the faster you dragged (591 px/s at a 600 px/s
         * drag, 98 px/s at 12000). */
        CHECK(victim > speeds[i] * 0.4);
    }
}

static void test_world_speed_ceiling(void) {
    /* One limit for everything: no dynamic body outruns the hardest throw the
     * config allows. It is what lets the drag hand Box2D an honest velocity
     * without a fast shove launching its neighbour across the strip. */
    CASE("nothing dynamic exceeds max_throw_speed");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 1200.0;

    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    /* physics_throw_body clamps on the way in, so ask through the back door the
     * cava row and the drag use. */
    physics_set_velocity(&w, 1, 30000.0, 12000.0);
    for (int i = 0; i < 10; i++) {
        physics_step(&w, SW, SH, 0, 0, 0, DT);
        PhysicsBody *b = physics_find_body(&w, 1);
        CHECK(hypot(b->vx, b->vy) <= 1200.0 + 1.0);
    }
    physics_destroy(&w);

    /* Zero means "no limit" — the escape hatch for anyone who wants chaos. */
    CASE("max_throw_speed = 0 lifts the ceiling");
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.max_throw_speed = 0.0;
    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    physics_set_velocity(&w, 1, 30000.0, 0.0);
    physics_step(&w, SW, SH, 0, 0, 0, DT);
    CHECK(hypot(physics_find_body(&w, 1)->vx, physics_find_body(&w, 1)->vy) > 20000.0);
    physics_destroy(&w);
}

static void test_engine_ceiling_follows_config(void) {
    /* Box2D enforces a speed limit of its own — 400 m/s, which is 40000 px/s at
     * this scale — silently. A config asking for a faster throw than that used to
     * get 39550 px/s and no explanation. */
    CASE("a raised max_throw_speed is not quietly clipped");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 100000.0;

    physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
    physics_throw_body(&w, 1, 60000.0, 0.0);
    physics_step(&w, SW, SH, 0, 0, 0, DT);
    PhysicsBody *b = physics_find_body(&w, 1);
    /* Damping takes a per-step bite; the old failure was a third of the speed
     * disappearing, not two percent. */
    CHECK(hypot(b->vx, b->vy) > 55000.0);
    physics_destroy(&w);
}

static void test_walls_hold_absurd_speeds(void) {
    CASE("the play area contains a throw nothing should survive");
    const double speeds[] = { 12000.0, 48000.0, 96000.0 };
    for (unsigned i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        PhysicsWorld w;
        physics_init(&w);
        w.gravity_scale = 1.0;
        w.throw_speed_multiplier = 1.0;
        w.max_throw_speed = 1.0e9;      /* no ceiling: test the WALLS */

        physics_sync_body(&w, 1, 900, 400, 400, 300, SW);
        physics_throw_body(&w, 1, speeds[i], speeds[i] / 2.0);

        for (int s = 0; s < 600; s++) {
            physics_step(&w, SW, SH, 0, 0, 0, DT);
            PhysicsBody *b = physics_find_body(&w, 1);
            CHECK(!isnan(b->x) && !isnan(b->y));
            /* Inside, or at worst a wall's thickness into one — never out. */
            CHECK(b->x + b->width > -80.0);
            CHECK(b->x < WORLD + 80.0);
            CHECK(b->y + b->height > -80.0);
            CHECK(b->y < SH + 80.0);
        }
        physics_destroy(&w);
    }
}

static void test_init_gives_a_closed_world(void) {
    /* physics_init used to leave `wrap` alone, and `wrap` decides whether the
     * strip HAS end walls. A caller with a stack-allocated world got whatever was
     * on the stack, and one value of that quietly removes the ends — windows sail
     * off the left of desktop 1 and reappear at desktop 10. The compositor never
     * saw it because its server struct is zeroed. */
    CASE("a freshly initialised world has ends");
    PhysicsWorld w;
    physics_init(&w);
    CHECK_INT(w.wrap, 0);
    CHECK_INT(w.impact_count, 0);
    w.gravity_scale = 0.0;
    w.throw_speed_multiplier = 1.0;
    w.max_throw_speed = 1.0e9;

    physics_sync_body(&w, 1, 200, 400, 400, 300, SW);
    physics_throw_body(&w, 1, -20000.0, 0.0);   /* straight at the left end */
    for (int i = 0; i < 240; i++) physics_step(&w, SW, SH, 0, 0, 0, DT);

    PhysicsBody *b = physics_find_body(&w, 1);
    CHECK(b->x > -80.0);                 /* bounced, not gone */
    CHECK(b->x < WORLD / 2.0);           /* and did not reappear at the far end */
    physics_destroy(&w);
}

int main(void) {
    test_drag_does_not_pass_through();
    test_world_speed_ceiling();
    test_engine_ceiling_follows_config();
    test_walls_hold_absurd_speeds();
    test_init_gives_a_closed_world();
    return t_report("physics_speed");
}
