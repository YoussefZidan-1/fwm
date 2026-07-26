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

/* Touchpad gesture state machine. See gestures.h for why it is a file of its
 * own and knows nothing about wlroots. */

#include "gestures.h"

#include <math.h>
#include <string.h>

/* How much of a velocity sample is new. libinput sends swipe updates at the
 * device's rate (~125 Hz on most touchpads), so a single event's delta is a
 * couple of pixels and far too noisy to flick on; this averages over roughly
 * the last three. */
#define VEL_SMOOTH 0.35

const char *gesture_bind_lookup(const GesturesConfig *cfg, int fingers, int dir) {
    if (!cfg) return NULL;
    for (int i = 0; i < cfg->bind_count; i++) {
        if (cfg->binds[i].fingers == fingers && cfg->binds[i].dir == dir)
            return cfg->binds[i].action;
    }
    return NULL;
}

static int dir_is_pinch(int dir) {
    return dir == GESTURE_PINCH_IN || dir == GESTURE_PINCH_OUT;
}

int gesture_claims(const GesturesConfig *cfg, GestureKind kind, int fingers) {
    if (!cfg || kind == GESTURE_KIND_NONE) return 0;
    for (int i = 0; i < cfg->bind_count; i++) {
        if (cfg->binds[i].fingers != fingers) continue;
        int pinch = dir_is_pinch(cfg->binds[i].dir);
        if (pinch == (kind == GESTURE_KIND_PINCH)) return 1;
    }
    return 0;
}

void gesture_begin(GestureState *st, const GesturesConfig *cfg,
                   GestureKind kind, int fingers, uint32_t time_msec) {
    memset(st, 0, sizeof(*st));
    st->kind    = kind;
    st->fingers = fingers;
    st->scale   = 1.0;
    st->last_ms = time_msec;
    st->claimed = gesture_claims(cfg, kind, fingers);
}

void gesture_end(GestureState *st) {
    memset(st, 0, sizeof(*st));
}

/* The direction the fingers have committed to, or -1 if they have not moved
 * far enough yet. */
static int resolve_dir(const GestureState *st) {
    if (st->kind == GESTURE_KIND_PINCH) {
        if (st->scale >= GESTURE_PINCH_OUT_SCALE) return GESTURE_PINCH_OUT;
        if (st->scale <= GESTURE_PINCH_IN_SCALE)  return GESTURE_PINCH_IN;
        return -1;
    }
    /* A swipe is one direction, not two: the dominant axis decides, so a hand
     * that drifts up while sweeping right still switches desktops. */
    if (fabs(st->dx) >= fabs(st->dy)) {
        if (fabs(st->dx) < GESTURE_SWIPE_THRESHOLD) return -1;
        return st->dx > 0.0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
    }
    if (fabs(st->dy) < GESTURE_SWIPE_THRESHOLD) return -1;
    return st->dy > 0.0 ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
}

const char *gesture_update(GestureState *st, const GesturesConfig *cfg,
                           double dx, double dy, double scale, uint32_t time_msec) {
    if (st->kind == GESTURE_KIND_NONE) return NULL;

    st->dx += dx;
    st->dy += dy;
    if (st->kind == GESTURE_KIND_PINCH) st->scale = scale;

    /* Unsigned arithmetic, so a timestamp that wrapped or went backwards comes
     * out as an absurd interval rather than a negative one; either way the
     * sample is dropped and the last velocity stands. */
    uint32_t elapsed = time_msec - st->last_ms;
    st->last_ms = time_msec;
    if (elapsed > 0 && elapsed < 200) {
        double dt = elapsed / 1000.0;
        st->vx += (dx / dt - st->vx) * VEL_SMOOTH;
        st->vy += (dy / dt - st->vy) * VEL_SMOOTH;
    }

    if (st->decided) return NULL;

    int dir = resolve_dir(st);
    if (dir < 0) return NULL;
    st->decided = 1;

    const char *action = gesture_bind_lookup(cfg, st->fingers, dir);
    if (!action) return NULL;

    /* The pan is not a one-shot action: it hands the camera to the fingers for
     * as long as they are down, and the caller drives it from st->pan. */
    if (strcmp(action, GESTURE_ACTION_PAN) == 0) {
        st->pan = 1;
        return NULL;
    }
    return action;
}

/* ── the live desktop pan ────────────────────────────────────────────── */

/* Camera travel asked for by the fingers so far, in px. Natural means the
 * strip follows the fingers — sweeping right shows what was to the left, so
 * the camera moves the other way. */
static double pan_offset(const GestureState *st, const GesturesConfig *cfg) {
    double sens = cfg->sensitivity;
    return (cfg->natural ? -st->dx : st->dx) * sens;
}

int gesture_pan_camera(const GestureState *st, const GesturesConfig *cfg,
                       int base_x, int max_x) {
    double x = base_x + pan_offset(st, cfg);
    if (x < 0.0) x = 0.0;
    if (x > (double)max_x) x = (double)max_x;
    return (int)lround(x);
}

int gesture_pan_target(const GestureState *st, const GesturesConfig *cfg,
                       int target_x, int screen_width, int desktops,
                       uint32_t time_msec) {
    if (screen_width <= 0 || desktops <= 0) return 0;

    /* Camera velocity, not finger velocity: which desktop a flick is heading
     * for depends on the sign the sensitivity and natural-scroll settings
     * leave it with. */
    double cam_v = (cfg->natural ? -st->vx : st->vx) * cfg->sensitivity;
    if (time_msec - st->last_ms > GESTURE_FLICK_STALE_MS) cam_v = 0.0;

    int d;
    if (target_x < 0) {
        d = 0;
    } else if (cam_v > GESTURE_FLICK_SPEED) {
        d = target_x / screen_width + 1;
    } else if (cam_v < -GESTURE_FLICK_SPEED) {
        /* Floor: a flick left leaves the desktop the camera is standing in
         * even when it has barely entered it. */
        d = target_x / screen_width;
        if (target_x % screen_width == 0) d -= 1;
    } else {
        d = (target_x + screen_width / 2) / screen_width;
    }

    if (d < 0) d = 0;
    if (d > desktops - 1) d = desktops - 1;
    return d;
}
