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

/* The ends of the world, and what happens at them.
 *
 * A window thrown off the edge is the one interaction nobody can check by
 * hand in a nested session — throwing needs a mouse, and a nested run cannot
 * move the pointer. physics.c reaches Box2D but not wlroots, so the throw can
 * be asserted here instead, which is also the only reason this file exists:
 * the ring shipped once without it and "windows cannot move between 1 and 10"
 * was the report that came back. */

#include "test.h"
#include "physics.h"

#define SW  1920
#define SH  1080
#define SPAN (10 * SW)
#define DT  (1.0 / 60.0)

static PhysicsBody *spawn(PhysicsWorld *w, int x, int y) {
    return physics_sync_body(w, 1, x, y, 400, 300, SW);
}

static void run(PhysicsWorld *w, int frames) {
    for (int i = 0; i < frames; i++)
        physics_step(w, SW, SH, 0, 0, 0, DT);
}

/* Thrown at the left end of a STRIP: the wall is there, so it stays inside. */
static void test_wall_holds_a_line(void) {
    CASE("line");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 0;

    PhysicsBody *b = spawn(&w, 60, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, -4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x >= -1.0);                 /* inside the world */
    CHECK_INT(b->desktop_id, 0);
    physics_destroy(&w);
}

/* The same throw on a RING arrives at the far end, still moving. */
static void test_ring_carries_a_throw_round(void) {
    CASE("ring, leftward");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, 60, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, -4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x > SPAN - 2 * SW);          /* came out at the far end */
    CHECK_INT(b->desktop_id, 9);
    CHECK(b->vx < -100.0);                /* and is still flying */
    physics_destroy(&w);
}

static void test_ring_carries_a_throw_the_other_way(void) {
    CASE("ring, rightward");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, SPAN - 460, 400);
    CHECK_NOT_NULL(b);
    physics_throw_body(&w, 1, 4000.0, 0.0);
    run(&w, 60);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x < 2 * SW);
    CHECK_INT(b->desktop_id, 0);
    CHECK(b->vx > 100.0);
    physics_destroy(&w);
}

/* The crossing must not be visible as a jump WITHIN the world: a window is
 * only carried once it has left entirely, so it is never half at each end. */
static void test_ring_crosses_only_when_clear(void) {
    CASE("ring, straddling");
    PhysicsWorld w;
    physics_init(&w);
    w.gravity_scale = 0.0;
    w.wrap = 1;

    PhysicsBody *b = spawn(&w, -100, 400);   /* half off the left end */
    CHECK_NOT_NULL(b);
    physics_set_velocity(&w, 1, 0.0, 0.0);
    run(&w, 2);

    b = physics_find_body(&w, 1);
    CHECK_NOT_NULL(b);
    CHECK(b->x < SW);                        /* still at this end, not teleported */
    physics_destroy(&w);
}

int main(void) {
    test_wall_holds_a_line();
    test_ring_carries_a_throw_round();
    test_ring_carries_a_throw_the_other_way();
    test_ring_crosses_only_when_clear();
    return t_report("physics");
}
