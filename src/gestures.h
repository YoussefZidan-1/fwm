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

#ifndef FWM_GESTURES_H
#define FWM_GESTURES_H

/*
 * Touchpad gestures: the state machine, and nothing else.
 *
 * libinput hands a compositor swipe/pinch deltas; what a compositor has to
 * decide is (a) whether it wants the gesture at all or the client under the
 * cursor should see it, (b) which of the four directions the fingers meant,
 * and (c) where a live desktop pan should come to rest when the fingers lift.
 * None of that needs a window, a display or wlroots, so it lives here and is
 * asserted in tests/test_gestures.c. src/server_gestures.c is the thin half
 * that turns wlr_pointer events into these calls and acts on the answers.
 *
 * The one gesture that is not a keybind in disguise is the desktop pan: the
 * camera follows the fingers across the 10-desktop strip while they move, so
 * it cannot be expressed as "fire this action". It is spelled `pan_desktop`
 * in the config and drives gesture_pan_camera / gesture_pan_target instead.
 */

#include <stdint.h>

#include "config.h"

/* Finger travel (px) before a swipe commits to a direction. Low enough that a
 * deliberate flick is caught, high enough that a two-finger scroll wandering
 * into a third finger does not switch desktops. */
#define GESTURE_SWIPE_THRESHOLD 24.0

/* Absolute pinch scale (relative to the gesture's start) that commits. */
#define GESTURE_PINCH_IN_SCALE  0.80
#define GESTURE_PINCH_OUT_SCALE 1.25

/* Camera speed (px/s) at the end of a pan that counts as a flick: the strip
 * carries on to the next desktop instead of snapping to the nearest one. */
#define GESTURE_FLICK_SPEED 450.0

/* How long after the last finger motion a flick stops counting as one (ms). */
#define GESTURE_FLICK_STALE_MS 80

typedef enum {
    GESTURE_KIND_NONE = 0,
    GESTURE_KIND_SWIPE,
    GESTURE_KIND_PINCH,
} GestureKind;

typedef struct {
    GestureKind kind;    /* GESTURE_KIND_NONE while no gesture is in flight */
    int    fingers;
    int    claimed;      /* fwm owns this one; the client sees nothing of it */
    int    decided;      /* a direction has been resolved — fire once only */
    int    pan;          /* the resolved action is the live desktop pan */
    double dx, dy;       /* finger travel since begin, px */
    double vx, vy;       /* smoothed finger speed, px/s */
    double scale;        /* pinch: last absolute scale */
    uint32_t last_ms;    /* previous event's timestamp, for the velocity */
} GestureState;

/* The action bound to (fingers, dir), or NULL. `dir` is a GESTURE_SWIPE_* /
 * GESTURE_PINCH_* constant from config.h. */
const char *gesture_bind_lookup(const GesturesConfig *cfg, int fingers, int dir);

/* Whether anything at all is bound to this many fingers of this kind. Asked
 * once per gesture, at begin: a gesture fwm has no use for must be handed to
 * the client whole (browsers pinch-zoom, GTK swipes back), and the protocol
 * gives no way to change our mind halfway through. */
int gesture_claims(const GesturesConfig *cfg, GestureKind kind, int fingers);

/* Start tracking. Sets st->claimed; when that is 0 the caller should forward
 * the gesture to the seat and not call gesture_update at all. */
void gesture_begin(GestureState *st, const GesturesConfig *cfg,
                   GestureKind kind, int fingers, uint32_t time_msec);

/* Feed one libinput update. `dx`/`dy` are this event's deltas (px), `scale`
 * the pinch's absolute scale (ignored for swipes), `time_msec` the event's
 * timestamp — the interval between events is what turns the deltas into the
 * speed a flick is judged on.
 *
 * Returns the action to fire, or NULL. It returns non-NULL at most once per
 * gesture: everything after the direction is resolved is either a live pan
 * (st->pan) or nothing. */
const char *gesture_update(GestureState *st, const GesturesConfig *cfg,
                           double dx, double dy, double scale, uint32_t time_msec);

/* Stop tracking. */
void gesture_end(GestureState *st);

/* ── the live desktop pan ────────────────────────────────────────────── */

/* Where the fingers have asked the camera to be, clamped to [0, max_x].
 * `base_x` is the camera position the pan started from. */
int gesture_pan_camera(const GestureState *st, const GesturesConfig *cfg,
                       int base_x, int max_x);

/* Which desktop the pan should settle on when the fingers lift. `target_x` is
 * where the camera was last asked to go (not where it has got to — the fingers
 * are what the user was aiming with). A fast enough flick carries one desktop
 * further in the direction of travel; otherwise the nearest one wins.
 *
 * `time_msec` is when the fingers came off. Fingers that stopped moving before
 * they lifted send no further updates, so the stored velocity would keep
 * claiming a flick that ended long ago — going by the gap since the last
 * update is what makes "drag halfway, hold, let go" stay put. */
int gesture_pan_target(const GestureState *st, const GesturesConfig *cfg,
                       int target_x, int screen_width, int desktops,
                       uint32_t time_msec);

#endif /* FWM_GESTURES_H */
