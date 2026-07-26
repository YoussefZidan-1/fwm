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

/*
 * Touchpad gestures: the wlroots half. Turns wlr_pointer swipe/pinch/hold
 * events into gestures.c's state machine and acts on what it decides — either
 * a config action, exactly as if it had been a keybind, or the live desktop
 * pan, which has no keybind equivalent because it hands the camera to the
 * fingers for as long as they are down.
 *
 * A gesture nothing is bound to is not ours: it is passed to the client under
 * the cursor through pointer-gestures-unstable-v1, which is how a browser
 * keeps its pinch-to-zoom. That decision is made once, at begin — the protocol
 * has no way to hand a gesture over halfway through, and a client that saw an
 * update without a begin would be entitled to complain.
 *
 * Split out of server_pointer.c; see server_internal.h.
 */

#include "server.h"
#include "view.h"
#include "gestures.h"
#include "lock.h"
#include "server_internal.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* The desktop strip is ten screens wide (see server.h). */
#define GESTURE_DESKTOPS 10

/* Common preamble: gestures are input like any other, and they are not for a
 * locked session. Returns 0 when the event should be dropped entirely. */
static int gesture_allowed(FwmServer *server) {
    server_notify_activity(server);
    return !lock_is_active(server);
}

/* Hand the camera whatever position the fingers have asked for. Uses the free
 * pan (not the desktop slide): the target moves with every event, and the slide
 * restarts its fixed-duration ease each time it is retargeted. */
static void pan_follow(FwmServer *server) {
    int max_x = (GESTURE_DESKTOPS - 1) * server->screen_width;
    server->target_camera_x = gesture_pan_camera(&server->gesture,
                                                 &server->config.gestures,
                                                 server->gesture_base_camera, max_x);
    server->cam_free = 1;
}

/* Fingers off: park the strip on one desktop. */
static void pan_settle(FwmServer *server, uint32_t time_msec) {
    int d = gesture_pan_target(&server->gesture, &server->config.gestures,
                               server->target_camera_x, server->screen_width,
                               GESTURE_DESKTOPS, time_msec);
    server->target_camera_x = d * server->screen_width;
    server->cam_free = 0; /* the eased slide finishes the last stretch */

    /* Released exactly where it already was: the tick's settle branch only runs
     * while the camera has somewhere to go, so nothing would re-home the
     * keyboard or tell the panels which desktop this is. */
    if (server->camera_x == server->target_camera_x && !server->cam_anim) {
        server_camera_settled(server);
    }
}

/* ── swipe ───────────────────────────────────────────────────────────── */

static void handle_swipe_begin(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_swipe_begin);
    struct wlr_pointer_swipe_begin_event *event = data;
    if (!gesture_allowed(server)) return;

    gesture_begin(&server->gesture, &server->config.gestures,
                  GESTURE_KIND_SWIPE, (int)event->fingers, event->time_msec);

    /* Where a pan would start from: where the camera is HEADED, so a swipe that
     * interrupts a desktop slide picks it up rather than dragging from behind. */
    server->gesture_base_camera = server->target_camera_x;

    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures,
                                                server->seat, event->time_msec,
                                                event->fingers);
    }
}

static void handle_swipe_update(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_swipe_update);
    struct wlr_pointer_swipe_update_event *event = data;
    if (server->gesture.kind != GESTURE_KIND_SWIPE) return;
    server_notify_activity(server);

    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures,
                                                 server->seat, event->time_msec,
                                                 event->dx, event->dy);
        return;
    }

    const char *action = gesture_update(&server->gesture, &server->config.gestures,
                                        event->dx, event->dy, 1.0, event->time_msec);
    if (action) {
        wlr_log(WLR_DEBUG, "gesture: swipe%d -> %s", server->gesture.fingers, action);
        server_dispatch_action(server, action);
        return;
    }
    if (server->gesture.pan) pan_follow(server);
}

static void handle_swipe_end(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_swipe_end);
    struct wlr_pointer_swipe_end_event *event = data;
    if (server->gesture.kind != GESTURE_KIND_SWIPE) return;

    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures,
                                               server->seat, event->time_msec,
                                               event->cancelled);
    } else if (server->gesture.pan) {
        /* Cancelled counts as released: libinput cancels when it stops
         * believing the finger count, and either way the camera must not be
         * left parked between two desktops. */
        pan_settle(server, event->time_msec);
    }
    gesture_end(&server->gesture);
}

/* ── pinch ───────────────────────────────────────────────────────────── */

static void handle_pinch_begin(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_pinch_begin);
    struct wlr_pointer_pinch_begin_event *event = data;
    if (!gesture_allowed(server)) return;

    gesture_begin(&server->gesture, &server->config.gestures,
                  GESTURE_KIND_PINCH, (int)event->fingers, event->time_msec);
    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures,
                                                server->seat, event->time_msec,
                                                event->fingers);
    }
}

static void handle_pinch_update(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_pinch_update);
    struct wlr_pointer_pinch_update_event *event = data;
    if (server->gesture.kind != GESTURE_KIND_PINCH) return;
    server_notify_activity(server);

    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_pinch_update(server->pointer_gestures,
                                                  server->seat, event->time_msec,
                                                  event->dx, event->dy,
                                                  event->scale, event->rotation);
        return;
    }

    const char *action = gesture_update(&server->gesture, &server->config.gestures,
                                        event->dx, event->dy, event->scale,
                                        event->time_msec);
    if (action) {
        wlr_log(WLR_DEBUG, "gesture: pinch%d -> %s", server->gesture.fingers, action);
        server_dispatch_action(server, action);
    }
}

static void handle_pinch_end(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_pinch_end);
    struct wlr_pointer_pinch_end_event *event = data;
    if (server->gesture.kind != GESTURE_KIND_PINCH) return;

    if (!server->gesture.claimed) {
        wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures,
                                               server->seat, event->time_msec,
                                               event->cancelled);
    }
    gesture_end(&server->gesture);
}

/* ── hold ────────────────────────────────────────────────────────────── */

/* Nothing binds a hold — it carries no direction and no distance, so there is
 * nothing for it to mean here. It is forwarded whole: clients use it to stop
 * kinetic scrolling the moment fingers land. */
static void handle_hold_begin(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_hold_begin);
    struct wlr_pointer_hold_begin_event *event = data;
    if (!gesture_allowed(server)) return;
    wlr_pointer_gestures_v1_send_hold_begin(server->pointer_gestures, server->seat,
                                            event->time_msec, event->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_hold_end);
    struct wlr_pointer_hold_end_event *event = data;
    if (lock_is_active(server)) return;
    wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures, server->seat,
                                          event->time_msec, event->cancelled);
}

void server_gestures_register(FwmServer *server) {
    if (!server->pointer_gestures) return;

    server->cursor_swipe_begin.notify = handle_swipe_begin;
    wl_signal_add(&server->cursor->events.swipe_begin, &server->cursor_swipe_begin);
    server->cursor_swipe_update.notify = handle_swipe_update;
    wl_signal_add(&server->cursor->events.swipe_update, &server->cursor_swipe_update);
    server->cursor_swipe_end.notify = handle_swipe_end;
    wl_signal_add(&server->cursor->events.swipe_end, &server->cursor_swipe_end);

    server->cursor_pinch_begin.notify = handle_pinch_begin;
    wl_signal_add(&server->cursor->events.pinch_begin, &server->cursor_pinch_begin);
    server->cursor_pinch_update.notify = handle_pinch_update;
    wl_signal_add(&server->cursor->events.pinch_update, &server->cursor_pinch_update);
    server->cursor_pinch_end.notify = handle_pinch_end;
    wl_signal_add(&server->cursor->events.pinch_end, &server->cursor_pinch_end);

    server->cursor_hold_begin.notify = handle_hold_begin;
    wl_signal_add(&server->cursor->events.hold_begin, &server->cursor_hold_begin);
    server->cursor_hold_end.notify = handle_hold_end;
    wl_signal_add(&server->cursor->events.hold_end, &server->cursor_hold_end);
}
