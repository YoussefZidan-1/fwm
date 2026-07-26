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

/* Action dispatch: the single table mapping a bind or an IPC command to what
 * the compositor actually does, plus the tiling-context helpers only it uses.
 * Split out of server.c; see server_internal.h. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"

/* Directional tile navigation: among the leaves of `desktop`, find the one
 * nearest to `from` in direction `dir` ('l','r','u','d'), judged by tile
 * centers. Returns NULL if there is nothing that way. */
static BspNode *tile_neighbor(FwmServer *server, int desktop, BspNode *from, char dir) {
    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(server->bsp_roots[desktop], leaves, &count, MAX_WINDOWS);

    double fx = from->x + from->w / 2.0;
    double fy = from->y + from->h / 2.0;

    BspNode *best = NULL;
    double best_dist = 0;
    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        if (n == from) continue;
        double cx = n->x + n->w / 2.0;
        double cy = n->y + n->h / 2.0;
        double dx = cx - fx, dy = cy - fy;

        int ok = 0;
        switch (dir) {
        case 'l': ok = dx < -1; break;
        case 'r': ok = dx >  1; break;
        case 'u': ok = dy < -1; break;
        case 'd': ok = dy >  1; break;
        }
        if (!ok) continue;

        // Prefer the closest tile, weighting the off-axis offset heavier so
        // "focus left" picks the tile actually beside us, not one far diagonal.
        double axis  = (dir == 'l' || dir == 'r') ? fabs(dx) : fabs(dy);
        double cross = (dir == 'l' || dir == 'r') ? fabs(dy) : fabs(dx);
        double dist = axis + cross * 2.0;
        if (!best || dist < best_dist) {
            best = n;
            best_dist = dist;
        }
    }
    return best;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include "server_internal.h"



/* A body physics is free to turn. Every state on this list makes the body an
 * immovable anchor in physics_step, so a "spinning" window in one of them
 * would hold an angle of zero forever — and since the effect swaps the live
 * window for a snapshot, that reads as the window having frozen. Refusing is
 * the honest answer; the same test also ends a spin the moment a window is
 * tiled or made fullscreen underneath it. */
bool server_can_spin(const PhysicsBody *b) {
    return b && !b->pinned && !b->fullscreen && !b->tiled && !b->floating;
}

/* Angular velocity a spin_window press hands the window, in rad/s. Deliberately
 * a drift — a tenth of a turn a second, gone in a few seconds — because the
 * press is not the effect: it is what puts the window in physics' hands. The
 * spinning itself comes from what happens to the window afterwards: what it is
 * thrown into, and stirring the mouse while dragging it. Scaled by
 * effects.spin. */
#define SPIN_KICK 0.6

/* The desktop an action's argument names: a number ("view:3"), or "next" /
 * "prev" relative to where the camera is HEADED — which is what the user is
 * aiming at when a gesture or a held key fires this twice in a row. Returns -1
 * when it names nothing that exists, including the ends of the strip, so the
 * caller does nothing rather than wrapping around. */
static int resolve_desktop(FwmServer *server, const char *arg) {
    int here = server->target_camera_x / server->screen_width;
    int d;
    if (strcmp(arg, "next") == 0) {
        d = here + 1;
    } else if (strcmp(arg, "prev") == 0) {
        d = here - 1;
    } else {
        char *end;
        long v = strtol(arg, &end, 10);
        if (end == arg) return -1;
        d = (int)v;
    }
    return (d >= 0 && d < 10) ? d : -1;
}

/* Shared setup for the tile_* actions: resolves the focused view's desktop,
 * checks it is tiling, and finds its leaf. Returns 0 if not applicable. */
static int tile_action_ctx(FwmServer *server, int *out_d, BspNode **out_leaf) {
    if (!server->focused_view) return 0;
    PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
    if (!pb) return 0;
    int d = pb->desktop_id;
    if (server->desktop_mode[d] != DESKTOP_MODE_TILING) return 0;
    BspNode *leaf = bsp_find(server->bsp_roots[d], server->focused_view->id);
    if (!leaf) return 0;
    *out_d = d;
    *out_leaf = leaf;
    return 1;
}

void server_dispatch_action(FwmServer *server, const char *action) {
    if (strcmp(action, "killclient") == 0) {
        if (server->focused_view) {
            view_send_close(server->focused_view);
        }
    } else if (strcmp(action, "toggle_tiling") == 0) {
        server_toggle_desktop_tiling(server, server->target_camera_x / server->screen_width);
    } else if (strncmp(action, "tile_focus:", 11) == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf)) {
            BspNode *n = tile_neighbor(server, d, leaf, action[11]);
            if (n) {
                FwmView *v = server_find_view(server, n->id);
                if (v) server_focus_view(server, v);
            }
        }
    } else if (strncmp(action, "tile_move:", 10) == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf)) {
            BspNode *n = tile_neighbor(server, d, leaf, action[10]);
            if (n) {
                bsp_swap(server->bsp_roots[d], leaf->id, n->id);
                server_apply_tiling(server, d);
            }
        }
    } else if (strcmp(action, "toggle_split") == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf) && leaf->parent) {
            leaf->parent->split_h = !leaf->parent->split_h;
            server_apply_tiling(server, d);
        }
    } else if (strcmp(action, "EXIT") == 0) {
        server->running = 0;
        wl_display_terminate(server->wl_display);
    } else if (strcmp(action, "show_hints") == 0) {
        if (server->hints_buffer) {
            cairo_overlay_destroy(server->hints_buffer);
            server->hints_buffer = NULL;
        } else {
            server->hints_buffer = hints_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);
        }
    } else if (strcmp(action, "wallpaper_picker") == 0) {
        bool was_open = launcher_is_open(server->launcher);
        launcher_toggle_wallpapers(server->launcher);
        launcher_grab_sync(server, was_open);
    } else if (strcmp(action, "reload_config") == 0) {
        server_reload_config(server);
    } else if (strcmp(action, "show_errors") == 0) {
        if (server->errors_buffer) {
            server_close_errors_panel(server);
        } else {
            server->errors_buffer = errors_show(server->layer_overlay, server->screen_width,
                                                server->screen_height, &server->config);
        }
        server_request_tray_redraw(server);
    } else if (strcmp(action, "group_toggle") == 0) {
        FwmView *v = server->focused_view;
        if (v) {
            if (v->group) group_dissolve(server, v->group);
            else group_create(server, v);
        }
    } else if (strcmp(action, "group_next") == 0) {
        if (server->focused_view && server->focused_view->group) {
            group_cycle(server, server->focused_view->group, 1);
        }
    } else if (strcmp(action, "group_prev") == 0) {
        if (server->focused_view && server->focused_view->group) {
            group_cycle(server, server->focused_view->group, -1);
        }
    } else if (strcmp(action, "group_add") == 0) {
        // Join the focused window into the group of any grouped window it
        // overlaps (drag-dropping onto a tab bar does the same with the mouse).
        FwmView *v = server->focused_view;
        if (v && !v->group) {
            FwmView *o;
            wl_list_for_each(o, &server->views, link) {
                if (o == v || !o->group || !o->scene_tree ||
                    !o->scene_tree->node.enabled) continue;
                if (v->x < o->x + o->width && v->x + v->width > o->x &&
                    v->y < o->y + o->height && v->y + v->height > o->y) {
                    group_add(server, o->group, v);
                    break;
                }
            }
        }
    } else if (strncmp(action, FWM_MODE_ACTION, strlen(FWM_MODE_ACTION)) == 0) {
        /* Step into a submap, or back out of one. Unknown names return to the
         * root map rather than leaving the keyboard in a mode that does not
         * exist — the safe direction when a config reload has just removed the
         * mode the user was standing in. */
        const char *name = action + strlen(FWM_MODE_ACTION);
        if (strcmp(name, FWM_MODE_DEFAULT) == 0) server->key_mode = -1;
        else                                     server->key_mode = config_mode_find(&server->config, name);
        server_request_tray_redraw(server);
    } else if (strcmp(action, "cycle_gravity") == 0) {
        /* Walk the ladder [physics] gravity_steps sets (zero-g, a lick of it,
         * earth by default). The current value is matched by proximity rather
         * than equality: it may have come from `fwmctl set` or a config reload
         * and land between two steps, and the sensible answer to "cycle from
         * here" is then the step after the nearest one. */
        const PhysicsConfig *pc = &server->config.physics;
        int n = pc->gravity_step_count;
        if (n > 0) {
            int nearest = 0;
            double best = fabs(server->physics.gravity_scale - pc->gravity_steps[0]);
            for (int i = 1; i < n; i++) {
                double d = fabs(server->physics.gravity_scale - pc->gravity_steps[i]);
                if (d < best) { best = d; nearest = i; }
            }
            server->physics.gravity_scale = pc->gravity_steps[(nearest + 1) % n];
            ipc_emit_gravity(server->ipc, server->physics.gravity_scale);
        }
    } else if (strcmp(action, "pin_window") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb) {
                pb->pinned ^= 1;
                pb->vx = 0; pb->vy = 0; pb->flying = 0;
            }
        }
    } else if (strcmp(action, "spin_window") == 0) {
        /* Experimental. Frees the focused window's rotation and kicks it, or
         * settles it back upright if it is already spinning. The kick's
         * direction alternates so pressing the bind twice in a row on two
         * windows does not produce a pair of synchronised clones. */
        double strength = server->config.effects.spin;
        if (strength > 0.0 && server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb && pb->spin) {
                physics_unspin_body(&server->physics, server->focused_view->id);
                view_stop_spin(server->focused_view);
            } else if (pb && server_can_spin(pb)) {
                static int flip = 0;
                flip = !flip;
                physics_spin_body(&server->physics, server->focused_view->id,
                                  (flip ? 1.0 : -1.0) * SPIN_KICK * strength);
            }
        }
    } else if (strcmp(action, "spin_all") == 0) {
        /* Same rule as toggle_nocollide_all: uniform, not per-window XOR — the
         * only predictable meaning of "all" is that after a press everything is
         * in the same state. Spinning wins unless everything already spins.
         * Alternating the direction per window keeps the desktop from looking
         * like a set of gears turning in lockstep. */
        double strength = server->config.effects.spin;
        if (strength > 0.0) {
            int all_spinning = 1;
            for (int i = 0; i < server->physics.body_count; i++) {
                const PhysicsBody *b = &server->physics.bodies[i];
                if (b->active && server_can_spin(b) && !b->spin) { all_spinning = 0; break; }
            }
            int sign = 1;
            for (int i = 0; i < server->physics.body_count; i++) {
                PhysicsBody *b = &server->physics.bodies[i];
                if (!b->active) continue;
                if (all_spinning || !server_can_spin(b)) {
                    if (b->spin) {
                        physics_unspin_body(&server->physics, b->id);
                        FwmView *sv = server_find_view(server, b->id);
                        if (sv) view_stop_spin(sv);
                    }
                } else if (!b->spin) {
                    physics_spin_body(&server->physics, b->id, sign * SPIN_KICK * strength);
                    sign = -sign;
                }
            }
        }
    } else if (strcmp(action, "toggle_nocollide") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb) pb->no_collide ^= 1;
        }
    } else if (strcmp(action, "toggle_nocollide_all") == 0) {
        /* For app launchers that spit out a pile of windows at once, where
         * turning collision off one window at a time is hopeless. Uniform
         * rather than per-window XOR: after any press every window is in the
         * same state, which is the only predictable meaning of "all". */
        int all_off = 1;
        for (int i = 0; i < server->physics.body_count; i++) {
            const PhysicsBody *b = &server->physics.bodies[i];
            if (b->active && !b->no_collide) { all_off = 0; break; }
        }
        int want = all_off ? 0 : 1;
        for (int i = 0; i < server->physics.body_count; i++) {
            PhysicsBody *b = &server->physics.bodies[i];
            if (b->active) b->no_collide = want;
        }
    } else if (strcmp(action, "toggle_tiling_all") == 0) {
        /* Same rule: bring every desktop to one mode rather than flipping each
         * independently. Tiling wins unless everything is already tiled. */
        int all_tiled = 1;
        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] != DESKTOP_MODE_TILING) { all_tiled = 0; break; }
        }
        int want = all_tiled ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_TILING;
        /* Set the mode outright instead of toggling: a floating desktop would
         * otherwise be flipped INTO tiling on the "everything back to physics"
         * pass, which is the opposite of what was asked. */
        for (int d = 0; d < 10; d++) server_set_desktop_mode(server, d, want);
    } else if (strcmp(action, "toggle_tray") == 0) {
        /* Hide the tray outright: the node goes away AND the strip it reserved
         * comes back, so tiles and fake-fullscreen windows grow into the top of
         * the screen instead of leaving a bar-shaped hole. Physics windows need
         * no help — nothing ever kept them out of that strip. */
        server->tray_hidden = !server->tray_hidden;
        if (server->tray_buffer)
            wlr_scene_node_set_enabled(&server->tray_buffer->node, !server->tray_hidden);

        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING)
                server_apply_tiling(server, d);
        }
        /* Re-run fake fullscreen for the geometry, not the state: the call is a
         * no-op on a body that is already fullscreen except for recomputing the
         * work area, which is exactly what changed. Real fullscreen never used
         * the strip, so it is left alone. */
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            PhysicsBody *fb = physics_find_body(&server->physics, fv->id);
            if (fb && fb->fullscreen && !fv->fs_real)
                server_set_fullscreen(server, fv, true, false);
        }
        server_request_tray_redraw(server);
    } else if (strcmp(action, "toggle_floating") == 0) {
        server_toggle_desktop_floating(server, server->target_camera_x / server->screen_width);
    } else if (strcmp(action, "toggle_floating_all") == 0) {
        int all_floating = 1;
        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] != DESKTOP_MODE_FLOATING) { all_floating = 0; break; }
        }
        int want = all_floating ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING;
        for (int d = 0; d < 10; d++) server_set_desktop_mode(server, d, want);
    } else if (strcmp(action, "calm_all") == 0) {
        for (int i = 0; i < server->physics.body_count; i++) {
            PhysicsBody *b = &server->physics.bodies[i];
            if (!b->active) continue;
            b->vx = 0; b->vy = 0; b->flying = 0;
        }
    } else if (strcmp(action, "fake_fullscreen") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            bool on = pb && pb->fullscreen;
            server_set_fullscreen(server, server->focused_view, !on, false);
        }
    } else if (strcmp(action, "real_fullscreen") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            bool on = pb && pb->fullscreen;
            server_set_fullscreen(server, server->focused_view, !on, true);
        }
    } else if (strncmp(action, "spawn:", 6) == 0) {
        const char *cmd = action + 6;
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            exit(1);
        }
    } else if (strncmp(action, "move_camera:", 12) == 0) {
        int amt = atoi(action + 12);
        int new_target = server->target_camera_x + amt;
        if (new_target < 0) new_target = 0;
        if (new_target > 9 * server->screen_width) new_target = 9 * server->screen_width;
        server->target_camera_x = new_target;
        server->cam_free = 1; // continuous pan, not a desktop jump
    } else if (strcmp(action, "launcher") == 0) {
        bool was_open = launcher_is_open(server->launcher);
        launcher_toggle(server->launcher);
        launcher_grab_sync(server, was_open);
    } else if (strncmp(action, "view:", 5) == 0) {
        int desktop = resolve_desktop(server, action + 5);
        if (desktop >= 0) {
            server->target_camera_x = desktop * server->screen_width;
            server->cam_free = 0; // discrete jump: use the eased slide
        }
    } else if (strncmp(action, "move_to:", 8) == 0) {
        int desktop = resolve_desktop(server, action + 8);
        if (desktop >= 0)
            server_move_view_to_desktop(server, server->focused_view, desktop, 0);
    } else if (strncmp(action, "move_to_view:", 13) == 0) {
        int desktop = resolve_desktop(server, action + 13);
        if (desktop >= 0)
            server_move_view_to_desktop(server, server->focused_view, desktop, 1);
    }
}

/* Run a config action on behalf of something that is not the keyboard (the
 * control socket). Deliberately the SAME entry point as a keybind, so an
 * action never behaves differently depending on how it was triggered.
 *
 * The caller is responsible for the locked-session check; the keyboard path
 * does its own before it ever reaches here. */
void server_dispatch_action_external(FwmServer *server, const char *action) {
    wlr_log(WLR_DEBUG, "ipc: dispatch %s", action);
    server_dispatch_action(server, action);
}
