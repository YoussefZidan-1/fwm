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

/* What a touchpad decides is impossible to eyeball: by the time a swipe has
 * gone wrong the fingers are already off it. So the two judgements that make
 * gestures feel right or wrong are asserted here — which direction a sweep
 * committed to, and which desktop it comes to rest on — along with the promise
 * that an unbound gesture is left for the client to handle. */

#include "test.h"
#include "gestures.h"

#define SW 1000  /* screen width used throughout */

/* One event's worth of finger motion, `ms` after the previous one. */
#define STEP_MS 8

/* The set config.toml.example recommends. fwm binds nothing by default, so this
 * stands in for a user who turned gestures on — spelled out here so the suite
 * does not need the TOML parser to run. */
static GesturesConfig cfg_default(void) {
    GesturesConfig g = { .sensitivity = 1.0, .natural = 1, .bind_count = 0 };
    struct { int f, d; const char *a; } binds[] = {
        { 3, GESTURE_SWIPE_LEFT,  GESTURE_ACTION_PAN  },
        { 3, GESTURE_SWIPE_RIGHT, GESTURE_ACTION_PAN  },
        { 3, GESTURE_SWIPE_UP,    "launcher"          },
        { 3, GESTURE_SWIPE_DOWN,  "toggle_tray"       },
        { 4, GESTURE_SWIPE_LEFT,  "move_to_view:prev" },
        { 4, GESTURE_SWIPE_RIGHT, "move_to_view:next" },
    };
    for (int i = 0; i < (int)(sizeof(binds) / sizeof(binds[0])); i++) {
        g.binds[g.bind_count].fingers = binds[i].f;
        g.binds[g.bind_count].dir     = binds[i].d;
        snprintf(g.binds[g.bind_count].action,
                 sizeof(g.binds[g.bind_count].action), "%s", binds[i].a);
        g.bind_count++;
    }
    return g;
}

/* Sweep the fingers `dx`,`dy` px per event for `events` events, returning the
 * first action the gesture resolved to (NULL if it resolved to none). */
static const char *sweep(GestureState *st, const GesturesConfig *cfg,
                         double dx, double dy, int events, uint32_t *clock) {
    const char *fired = NULL;
    for (int i = 0; i < events; i++) {
        *clock += STEP_MS;
        const char *a = gesture_update(st, cfg, dx, dy, 1.0, *clock);
        if (a && !fired) fired = a;
    }
    return fired;
}

static void test_direction(void) {
    CASE("a sweep commits to its dominant axis");
    GesturesConfig cfg = cfg_default();
    uint32_t t = 5000;

    GestureState st;
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 4, t);
    CHECK_STR(sweep(&st, &cfg, 6.0, 0.0, 20, &t), "move_to_view:next");

    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 4, t);
    CHECK_STR(sweep(&st, &cfg, -6.0, 0.0, 20, &t), "move_to_view:prev");

    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    CHECK_STR(sweep(&st, &cfg, 0.0, -6.0, 20, &t), "launcher");

    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    CHECK_STR(sweep(&st, &cfg, 0.0, 6.0, 20, &t), "toggle_tray");

    /* A hand that drifts while sweeping still means the way it swept. */
    CASE("drift off the axis does not change the direction");
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 4, t);
    CHECK_STR(sweep(&st, &cfg, 6.0, 2.0, 20, &t), "move_to_view:next");
}

static void test_threshold(void) {
    CASE("a nudge below the threshold resolves to nothing");
    GesturesConfig cfg = cfg_default();
    uint32_t t = 0;
    GestureState st;

    /* Three fingers landing and shifting a few px — a rest, not a swipe. */
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 4, t);
    CHECK_NULL(sweep(&st, &cfg, 1.0, 1.0, 8, &t));
    CHECK_INT(st.decided, 0);
    CHECK_INT(st.pan, 0);

    /* Crossing it decides, and it decides exactly once: an action must not
     * fire again for every event that follows. */
    CASE("the action fires once, not once per event");
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 4, t);
    int fires = 0;
    for (int i = 0; i < 30; i++) {
        t += STEP_MS;
        if (gesture_update(&st, &cfg, 5.0, 0.0, 1.0, t)) fires++;
    }
    CHECK_INT(fires, 1);
}

static void test_claim(void) {
    CASE("only bound finger counts are claimed");
    GesturesConfig cfg = cfg_default();

    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_SWIPE, 3), 1);
    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_SWIPE, 4), 1);
    /* Nothing is bound to two or five fingers, and nothing to any pinch: those
     * belong to the client under the cursor (pinch-to-zoom in a browser). */
    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_SWIPE, 2), 0);
    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_SWIPE, 5), 0);
    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_PINCH, 2), 0);
    CHECK_INT(gesture_claims(&cfg, GESTURE_KIND_PINCH, 3), 0);

    CASE("a pinch bind claims pinches, not swipes");
    GesturesConfig p = { .sensitivity = 1.0, .natural = 1, .bind_count = 1 };
    p.binds[0].fingers = 3;
    p.binds[0].dir     = GESTURE_PINCH_IN;
    snprintf(p.binds[0].action, sizeof(p.binds[0].action), "%s", "calm_all");
    CHECK_INT(gesture_claims(&p, GESTURE_KIND_PINCH, 3), 1);
    CHECK_INT(gesture_claims(&p, GESTURE_KIND_SWIPE, 3), 0);

    CASE("begin records the verdict");
    GestureState st;
    gesture_begin(&st, &cfg, GESTURE_KIND_PINCH, 2, 0);
    CHECK_INT(st.claimed, 0);
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, 0);
    CHECK_INT(st.claimed, 1);
}

static void test_pinch(void) {
    CASE("a pinch commits on scale, not on distance");
    GesturesConfig cfg = { .sensitivity = 1.0, .natural = 1, .bind_count = 2 };
    cfg.binds[0].fingers = 2;
    cfg.binds[0].dir     = GESTURE_PINCH_IN;
    snprintf(cfg.binds[0].action, sizeof(cfg.binds[0].action), "%s", "calm_all");
    cfg.binds[1].fingers = 2;
    cfg.binds[1].dir     = GESTURE_PINCH_OUT;
    snprintf(cfg.binds[1].action, sizeof(cfg.binds[1].action), "%s", "launcher");

    GestureState st;
    uint32_t t = 100;

    /* Fingers that travel a long way while the scale barely moves are a
     * two-finger drag, not a pinch. */
    gesture_begin(&st, &cfg, GESTURE_KIND_PINCH, 2, t);
    CHECK_NULL(sweep(&st, &cfg, 20.0, 0.0, 10, &t));

    gesture_begin(&st, &cfg, GESTURE_KIND_PINCH, 2, t);
    t += STEP_MS;
    CHECK_NULL(gesture_update(&st, &cfg, 0.0, 0.0, 0.9, t));   /* not yet */
    t += STEP_MS;
    CHECK_STR(gesture_update(&st, &cfg, 0.0, 0.0, 0.7, t), "calm_all");

    gesture_begin(&st, &cfg, GESTURE_KIND_PINCH, 2, t);
    t += STEP_MS;
    CHECK_NULL(gesture_update(&st, &cfg, 0.0, 0.0, 1.2, t));
    t += STEP_MS;
    CHECK_STR(gesture_update(&st, &cfg, 0.0, 0.0, 1.4, t), "launcher");
}

static void test_pan_follows_fingers(void) {
    CASE("the camera follows the fingers and stops at the ends");
    GesturesConfig cfg = cfg_default();
    uint32_t t = 0;
    GestureState st;

    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    CHECK_NULL(sweep(&st, &cfg, -10.0, 0.0, 30, &t)); /* pan is not an action */
    CHECK_INT(st.pan, 1);
    /* Natural: fingers left, camera right, one px per px. */
    CHECK_INT(gesture_pan_camera(&st, &cfg, 0, 9 * SW), 300);

    /* Past the left edge of desktop 0 there is nothing to show. */
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    sweep(&st, &cfg, 40.0, 0.0, 30, &t);
    CHECK_INT(gesture_pan_camera(&st, &cfg, 0, 9 * SW), 0);
    /* Nor past the right edge of the last one. */
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    sweep(&st, &cfg, -40.0, 0.0, 30, &t);
    CHECK_INT(gesture_pan_camera(&st, &cfg, 9 * SW, 9 * SW), 9 * SW);

    CASE("sensitivity scales the travel, natural = false reverses it");
    cfg.sensitivity = 2.0;
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    sweep(&st, &cfg, -10.0, 0.0, 30, &t);
    CHECK_INT(gesture_pan_camera(&st, &cfg, 0, 9 * SW), 600);
    cfg.natural = 0;
    CHECK_INT(gesture_pan_camera(&st, &cfg, 2 * SW, 9 * SW), 2 * SW - 600);
}

/* A pan that ended `dx` px from `base`, having moved at `speed` px/s of finger
 * travel over its last events. Returns the desktop it settles on. */
static int settle(const GesturesConfig *cfg, int base, double dx, double speed) {
    uint32_t t = 10000;
    GestureState st;
    gesture_begin(&st, cfg, GESTURE_KIND_SWIPE, 3, t);

    /* Enough events for the smoothed velocity to reach `speed`. */
    double per_event = speed * STEP_MS / 1000.0;
    if (dx < 0) per_event = -per_event;
    int events = per_event != 0.0 ? (int)(dx / per_event) : 0;
    for (int i = 0; i < events; i++) {
        t += STEP_MS;
        gesture_update(&st, cfg, per_event, 0.0, 1.0, t);
    }
    int target = gesture_pan_camera(&st, cfg, base, 9 * SW);
    return gesture_pan_target(&st, cfg, target, SW, 10, t);
}

static void test_pan_settles(void) {
    GesturesConfig cfg = cfg_default();

    CASE("a slow drag snaps to the nearest desktop");
    /* Sweeping left moves the camera right (natural), so a short slow drag
     * from desktop 3 comes back to 3 and a long one lands on 4. */
    CHECK_INT(settle(&cfg, 3 * SW, -200.0, 100.0), 3);
    CHECK_INT(settle(&cfg, 3 * SW, -700.0, 100.0), 4);
    CHECK_INT(settle(&cfg, 3 * SW,  200.0, 100.0), 3);
    CHECK_INT(settle(&cfg, 3 * SW,  700.0, 100.0), 2);

    CASE("a flick carries on to the next desktop");
    /* Same short distance, thrown instead of dragged. */
    CHECK_INT(settle(&cfg, 3 * SW, -200.0, 1500.0), 4);
    CHECK_INT(settle(&cfg, 3 * SW,  200.0, 1500.0), 2);

    CASE("holding still before letting go cancels the flick");
    uint32_t t = 10000;
    GestureState st;
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    for (int i = 0; i < 20; i++) { t += STEP_MS; gesture_update(&st, &cfg, -12.0, 0.0, 1.0, t); }
    int target = gesture_pan_camera(&st, &cfg, 3 * SW, 9 * SW);
    CHECK_INT(gesture_pan_target(&st, &cfg, target, SW, 10, t), 4);       /* flicked */
    CHECK_INT(gesture_pan_target(&st, &cfg, target, SW, 10, t + 400), 3); /* rested */

    CASE("the strip never runs off either end");
    CHECK_INT(settle(&cfg, 0, 900.0, 3000.0), 0);
    CHECK_INT(settle(&cfg, 9 * SW, -900.0, 3000.0), 9);
}

static void test_lookup(void) {
    CASE("lookup finds the bind, and only the right one");
    GesturesConfig cfg = cfg_default();
    CHECK_STR(gesture_bind_lookup(&cfg, 3, GESTURE_SWIPE_UP), "launcher");
    CHECK_STR(gesture_bind_lookup(&cfg, 3, GESTURE_SWIPE_LEFT), GESTURE_ACTION_PAN);
    CHECK_NULL(gesture_bind_lookup(&cfg, 4, GESTURE_SWIPE_UP));
    CHECK_NULL(gesture_bind_lookup(&cfg, 2, GESTURE_SWIPE_LEFT));
    CHECK_NULL(gesture_bind_lookup(&cfg, 3, GESTURE_PINCH_IN));
}

static void test_end_clears(void) {
    CASE("end leaves nothing behind for the next gesture");
    GesturesConfig cfg = cfg_default();
    uint32_t t = 0;
    GestureState st;
    gesture_begin(&st, &cfg, GESTURE_KIND_SWIPE, 3, t);
    sweep(&st, &cfg, -10.0, 0.0, 30, &t);
    CHECK_INT(st.pan, 1);
    gesture_end(&st);
    CHECK_INT(st.kind, GESTURE_KIND_NONE);
    CHECK_INT(st.pan, 0);
    CHECK_DBL(st.dx, 0.0, 1e-9);
    /* An update that arrives after the end (or before any begin) is ignored. */
    CHECK_NULL(gesture_update(&st, &cfg, -50.0, 0.0, 1.0, t + 100));
    CHECK_DBL(st.dx, 0.0, 1e-9);
}

int main(void) {
    test_direction();
    test_threshold();
    test_claim();
    test_pinch();
    test_pan_follows_fingers();
    test_pan_settles();
    test_lookup();
    test_end_clears();
    return t_report("gestures");
}
