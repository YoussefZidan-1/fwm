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

#ifndef FWM_PHYSICS_H
#define FWM_PHYSICS_H

#include <stdint.h>
#include "defines.h"

#define CORNER_SHARP   0
#define CORNER_CHAMFER 1
#define CORNER_ROUND   2

typedef struct {
    uint32_t id;         /* Unique ID of the view/window */
    int active;
    int flying;
    double vx, vy;
    double mass;
    double x, y;
    int width, height;
    int desktop_id;
    int fullscreen;
    int shaped;
    double sav_x, sav_y;
    int sav_w, sav_h;
    int pinned;
    int no_collide;
    int tiled;     /* window is managed by the tiling layout: physics never moves it */
    int floating;  /* desktop is in floating mode: immovable AND non-colliding,
                    * i.e. pinned + no_collide without touching those per-window
                    * flags, so leaving the mode restores what the user set */
    double tile_sav_x, tile_sav_y;
    int tile_sav_w, tile_sav_h;
    int tiling_saved;
    int corner_mode;

    /* Free rotation (experimental). Off by default: with `spin` clear the body
     * keeps Box2D's fixedRotation and behaves exactly as it always has. With it
     * set the collision box really turns — a spinning window wedges into a
     * corner and bounces off its own edges, it is not a picture spinning over
     * an upright box. `angle` is radians, y-down like every other coordinate
     * here, so it feeds the renderer's rotation matrix unchanged; x/y stay the
     * top-left of the UNROTATED box, i.e. the rotation is about the center. */
    int spin;
    double angle;
    double angvel;   /* rad/s */

    /* Per-window material, straight from [[rule]]. NAN means "the rule said
     * nothing about it", so the desktop's profile stands — the same tri-state
     * the rest of the rule properties use, spelled for doubles because 0 is a
     * meaningful value for every one of them (a weightless window, a window
     * that does not bounce). Resolved against the profile in physics_step. */
    double rule_mass;      /* multiplies the profile's mass_density; 1 = normal */
    double rule_gravity;   /* multiplies the profile's gravity; -1 floats up */
    double rule_bounce;    /* restitution 0..1, absolute */
    double rule_friction;  /* per-tick velocity retention, absolute */
} PhysicsBody;

/* The physics of one desktop. Every field starts as the [physics] value and is
 * overridden by the [physics.<name>] profile the desktop was assigned, so a
 * desktop nobody configured behaves exactly as the whole world used to. */
typedef struct {
    double gravity;      /* px/s^2 for this desktop, before gravity_scale */
    double friction;     /* per-tick velocity retention */
    double restitution;
    double mass_density;
} PhysicsProfile;

/* One collision hard enough to be worth reacting to, reported through the
 * mirror so the compositor never has to talk to Box2D. Valid only until the
 * next physics_step, which refills the list from scratch. */
#define PHYSICS_MAX_IMPACTS 32

typedef struct {
    uint32_t id_a, id_b;   /* window ids; 0 means a play-area wall */
    double x, y;           /* impact point, world px */
    double nx, ny;         /* contact normal, pointing from A to B */
    double speed;          /* approach speed, px/s — always positive */
} PhysicsImpact;

typedef struct {
    PhysicsBody bodies[MAX_WINDOWS];
    int body_count;

    PhysicsImpact impacts[PHYSICS_MAX_IMPACTS];
    int impact_count;
    double gravity_scale;
    
    /* Configurable physics parameters */
    double friction;
    double mass_density;
    double throw_speed_multiplier;
    double max_throw_speed;
    double stop_speed_threshold;
    double restitution;
    double gravity;

    /* Per-desktop physics. Filled from the config at load and reload; the
     * defaults above are what an unassigned desktop gets. Indexed by
     * PhysicsBody.desktop_id, so a window carries its desktop's physics with it
     * the moment it is dragged across the edge. */
    PhysicsProfile desktops[FWM_DESKTOPS];

    /* Opaque Box2D engine state (see physics.c). The struct above is the
     * authoritative "mirror" that the rest of the compositor reads and writes;
     * physics_step pushes it into Box2D, steps, and pulls results back. */
    void *engine;
} PhysicsWorld;

void physics_init(PhysicsWorld *world);
/* Point every desktop at the world's own scalars. Call it after those change
 * (config load, reload, `fwmctl set`), then write the desktops a
 * [physics.<name>] profile claims over the top. */
void physics_reset_profiles(PhysicsWorld *world);
void physics_destroy(PhysicsWorld *world);
PhysicsBody *physics_sync_body(PhysicsWorld *world, uint32_t id, int x, int y, int width, int height, int screen_width);
void physics_stop_body(PhysicsWorld *world, uint32_t id);
void physics_throw_body(PhysicsWorld *world, uint32_t id, double vx, double vy);
void physics_step(PhysicsWorld *world, int screen_width, int screen_height,
                  uint32_t skip_a, uint32_t skip_b, uint32_t dragged_id, double dt);
void physics_set_velocity(PhysicsWorld *world, uint32_t id, double vx, double vy);
PhysicsBody *physics_find_body(PhysicsWorld *world, uint32_t id);
void physics_remove_body(PhysicsWorld *world, uint32_t id);
/* Free rotation (experimental). physics_spin_body turns it on and gives the
 * window a kick of `angvel` rad/s; physics_unspin_body stops it and settles the
 * body back upright. Both are no-ops for an unknown id. */
void physics_spin_body(PhysicsWorld *world, uint32_t id, double angvel);
void physics_unspin_body(PhysicsWorld *world, uint32_t id);

void physics_push_away(PhysicsWorld *world, uint32_t pushed, uint32_t pusher, double speed);
void physics_push_overlapping(PhysicsWorld *world, uint32_t pusher, double speed);

/* ── audio visualiser bars ───────────────────────────────────────────── */

/* Ceiling on the bar row. Each bar is a real body in the world, so this is a
 * cost and not just an array bound; it matches CONFIG_MAX_BARS. */
#define PHYSICS_MAX_BARS 128

/* Stand a row of `count` bars on the floor, spanning `span_w` px starting at
 * world x `origin_x`, with bar i rising `levels[i]` (0..1) of `max_h` px.
 *
 * The bars are KINEMATIC, not static: a static body moved by teleporting it
 * passes through a resting window without ever touching it, because the solver
 * only sees the overlap after the fact and pushes the window out sideways —
 * whichever way is nearer. A kinematic body given a velocity is what the
 * contact solver actually integrates against, so a rising bar hands the window
 * above it real upward momentum. `dt` is the step the velocity is aimed at, and
 * it must be the same one physics_step is about to take.
 *
 * `max_rise` caps that velocity in the upward direction only, because the whole
 * of it lands in whatever is standing on the bar — see BAR_MAX_RISE_SPEED.
 *
 * Call once per step before physics_step. `count` 0 tears the row down, which
 * is also what an unconfigured compositor does forever. */
void physics_set_bars(PhysicsWorld *world, const float *levels, int count,
                      double origin_x, double span_w, double max_h,
                      int screen_h, double max_rise, double dt);

#endif /* FWM_PHYSICS_H */
