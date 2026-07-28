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

#include "expo.h"
#include "server.h"
#include "view.h"
#include "snapshot.h"
#include "rotate.h"
#include "wallpaper.h"
#include "physics.h"
#include "theme.h"
#include "server_internal.h"
#include "lock.h"
#include "ui/launcher.h"
#include "ui/expo_menu.h"
#include "ui/cairo_overlay.h"
#include "defines.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* One window on the strip. The card is a still picture: the live subtree it was
 * taken from is hidden for as long as the strip is up. */
typedef struct {
    FwmView *view;
    struct wlr_scene_buffer *node;   /* flat path only */
    struct wlr_texture *tex;         /* curved path only */
    struct wlr_buffer *buf;          /* our lock on the snapshot */
    double wx, wy;                   /* world position, in real (1:1) px */
    int w, h;                        /* world size */
    int desktop;
} ExpoItem;

struct FwmExpo {
    FwmServer *server;

    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *backdrop;
    struct wlr_scene_rect *hilight;             /* frame under the hovered card */
    /* One card per desktop: a tree holding the card's edge, the slot's own dark
     * base and, on top of both, one node per wallpaper layer cropped to that
     * desktop. The edge is simply a rect the card sits on top of, so only its
     * margin is ever seen. */
    struct wlr_scene_tree *cards[FWM_DESKTOPS];
    struct wlr_scene_rect *card_edge[FWM_DESKTOPS];
    struct wlr_scene_rect *card_base[FWM_DESKTOPS];
    struct wlr_scene_buffer *card_layers[FWM_DESKTOPS][EXPO_MAX_WP_LAYERS];
    int card_layer_n[FWM_DESKTOPS];

    /* Fixed slots for the same reason the rest of fwm uses them: no allocation
     * on a path that runs while the compositor is drawing. */
    ExpoItem items[MAX_WINDOWS];
    int n_items;

    /* The strip's own camera. `zoom` is how many desktops fit across the
     * screen — 1.0 is exactly what you were looking at a moment ago, which is
     * what makes entering and leaving a single continuous movement. */
    double zoom, zoom_target;
    /* How far open the seam is: 1 while the strip is a line, 0 once the ring is
     * closed, eased between the two so `x` brings the two ends together
     * instead of teleporting one of them. */
    double seam, seam_target;
    /* Free horizontal offset from the home desktop, strip px. Eased like the
     * zoom: an arrow key that teleported the strip sideways left you with no
     * idea which way it had gone. */
    double pan, pan_target;
    /* The orbit: how far the camera is lifted above the ring and how far it is
     * pulled back. Both are DEVIATIONS — they ease back to level and 1 the
     * moment the strip is asked to do anything else, because the property
     * worth protecting is that the strip and the live screen coincide exactly
     * at zoom 1.0. Only offered at the far zoom step: near the strip is
     * something you work in, and a tilted desktop is not something to work in.
     */
    double tilt, tilt_target;
    double dist, dist_target;
    int orbiting;                    /* middle button held */
    double orbit_x, orbit_y;         /* cursor at the last orbit sample */
    int home;            /* desktop the strip was entered from */
    int leaving;         /* animating out; torn down when the zoom lands */

    /* The curved path. A scene node is an axis-aligned rectangle and a turned
     * card is not, so when the renderer can do it (GLES2 — see rotate.h) the
     * whole strip is drawn by hand into one buffer instead, and `canvas` is the
     * single node that shows it. Two buffers, alternately: the scene may still
     * be reading last frame's while this one is drawn.
     *
     * When it cannot (Vulkan, pixman), `gl` stays false, the scene nodes above
     * are used, and expo_openness is forced to leave the strip flat. Everything
     * else — the geometry, the input, the animation — is shared. */
    bool gl;
    struct wlr_scene_buffer *canvas;
    struct wlr_buffer *canvas_buf[2];
    int canvas_i;
    /* What the canvas currently shows. Handing the scene a buffer damages the
     * whole screen, which schedules another frame, which would redraw and
     * damage again: a strip standing perfectly still would pin the machine at
     * 60fps forever. The flat path has no such problem — setting a node to the
     * position it already has is a no-op in wlr_scene — so this is the price of
     * drawing by hand, and it is one comparison. */
    double drawn_zoom, drawn_seam, drawn_pan, drawn_drag_x, drawn_drag_y;
    double drawn_tilt, drawn_dist;
    int drawn_cam, drawn_items, drawn_wrap;
    void *drawn_hover, *drawn_drag;
    struct wlr_texture *wp_tex[EXPO_MAX_WP_LAYERS];
    struct wlr_fbox wp_crop[FWM_DESKTOPS][EXPO_MAX_WP_LAYERS];
    int wp_n;

    ExpoItem *hover;
    ExpoItem *drag;
    double drag_off_x, drag_off_y;   /* cursor to card top-left, in world px */

    /* Right-click menu. It is parented to layer_overlay, not to the strip's own
     * tree, so it stays above the tray and does not scale with the cards. */
    struct wlr_scene_buffer *menu;
    ExpoItem *menu_item;
};

static void expo_menu_close(FwmExpo *e) {
    if (!e->menu) return;
    cairo_overlay_destroy(e->menu);
    e->menu = NULL;
    e->menu_item = NULL;
}

/* ── geometry ─────────────────────────────────────────────────────────────
 *
 * Three coordinate systems, and only one function may convert between them:
 *
 *   world  — what a window stores in view->x: absolute px across all ten
 *            desktops, exactly as the flat compositor uses it.
 *   strip  — world plus the gaps opened between the cards. Identical to world
 *            while the gap is closed, which is why zoom 1.0 is the live view.
 *   screen — output-local px, what the cursor is in.
 *
 * The inverse (expo_point) is not a convenience: input has to travel back down
 * this path, and when the strip eventually bends into a curve there must be one
 * place that learns about the curve, not one per event handler. */

/* How far the strip has opened, 0 at the live view and 1 at the near zoom step
 * (and beyond). Drives everything that must not exist at zoom 1.0: the gaps
 * between the cards, their edges, the dimming of the wallpaper behind them. */
static double expo_openness(FwmExpo *e) {
    double t = (e->zoom - 1.0) / (EXPO_ZOOM_NEAR - 1.0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t;
}

static double expo_gap(FwmExpo *e) {
    /* Zero at zoom 1.0, so entering opens the seams rather than jumping them
     * open. Full by the near step — the near step is only a little way out, and
     * a gap that kept growing with the zoom would never be visible there. */
    return EXPO_GAP_FRAC * e->server->screen_width * expo_openness(e);
}

static double expo_pitch(FwmExpo *e) {
    return e->server->screen_width + expo_gap(e);
}

/* Screen px per world px. */
static double expo_scale(FwmExpo *e) {
    return e->server->screen_width / (e->zoom * expo_pitch(e));
}

/* Strip coordinate the middle of the screen is looking at. */
static double expo_center(FwmExpo *e) {
    return e->server->camera_x + e->server->screen_width / 2.0
         + e->home * expo_gap(e) + e->pan;
}

/* Defined further down with the input and the camera they belong to; the tick
 * and the close path need them earlier. */
static void expo_drag_to(FwmExpo *e, double lx, double ly);
static void expo_clamp_pan(FwmExpo *e);
static bool expo_can_orbit(FwmExpo *e);
static void expo_camera_home(FwmExpo *e);

/* ── the ring in space ────────────────────────────────────────────────────
 *
 * The strip lies on a circle whose circumference is the ten desktops plus the
 * seam, and each desktop is a FLAT facet of that circle — a chord, not a piece
 * of the arc. Flat is not an approximation here, it is what a desktop is; and
 * it means a card is four corners rather than a tessellated bend, because a
 * plane in perspective is reproduced exactly by two triangles with a real w.
 *
 * The camera orbits the front of the strip: `pan` turns the ring under it
 * (that degree of freedom always existed), `tilt` lifts it above the ring, and
 * `dist` pulls it back. All three are held as a DEVIATION that eases to zero,
 * because the one property worth protecting is that at zoom 1.0 the strip is
 * pixel-for-pixel the live screen — which is what makes entering and leaving
 * one movement instead of a substitution.
 *
 * Ring space: x right, y down, z toward the viewer, origin at the front of the
 * strip. At zero curvature, zero tilt and distance 1 every formula below
 * reduces to `u * scale + half a screen`, exactly as the flat strip had it. */

typedef struct { double x, y, z; } ExpoPt;

/* One lap of the ring: the ten cards, plus the seam standing between the last
 * and the first. Closing the ring shrinks the seam to nothing.
 *
 * Everything that goes round — where a card is placed, where a screen point
 * lands, how far a pan is folded — measures by THIS, not by the ten cards
 * alone. They are the same number only when the ring is shut, and the
 * difference is exactly what used to make the end card jump. */
static double expo_lap(FwmExpo *e) {
    return FWM_DESKTOPS * expo_pitch(e)
         + EXPO_SEAM_PITCHES * expo_pitch(e) * e->seam;
}

/* Radius the strip is bent to, in strip px. 0 means flat — the live view, the
 * first frames of opening, and the whole fallback path. */
static double expo_radius(FwmExpo *e) {
    /* A scene node cannot be a trapezoid, so the fallback path is flat and
     * says so here rather than in six places downstream. */
    if (!e->gl) return 0.0;
    double k = expo_openness(e);
    if (k < 0.02) return 0.0;
    /* A longer lap is a bigger circle: an open strip is bent more gently than
     * a closed one, which falls out of the seam rather than being a second
     * knob to tune. */
    return expo_lap(e) / (2.0 * M_PI) / k;
}

/* Base viewer distance, and the focal length taken from it. The focal length
 * deliberately does NOT follow `dist`: if it did, pulling the camera back
 * would narrow the lens by the same amount and nothing would appear to move. */
static double expo_dist(FwmExpo *e) {
    return EXPO_CAM_DIST * expo_pitch(e) * e->dist;
}

static double expo_focal(FwmExpo *e) {
    return expo_scale(e) * EXPO_CAM_DIST * expo_pitch(e);
}

/* A point on the strip: `u` strip px from the middle of the screen, `wy` in
 * world pixels down the desktop. */
static ExpoPt expo_ring_point(FwmExpo *e, double u, double wy) {
    double r = expo_radius(e);
    ExpoPt p = { .y = wy - e->server->screen_height / 2.0 };
    if (r <= 0.0) {
        p.x = u;
        p.z = 0.0;
    } else {
        double phi = u / r;
        p.x = r * sin(phi);
        p.z = r * cos(phi) - r;
    }
    return p;
}

/* A point on a facet, given its two ends: `s` runs 0..1 across the desktop and
 * may run past either end, which keeps a window that straddles a boundary in
 * the plane of its own desktop instead of bending it onto the arc. */
static ExpoPt expo_facet_point(FwmExpo *e, ExpoPt a, ExpoPt b, double s, double wy) {
    ExpoPt p = { .x = a.x + (b.x - a.x) * s,
                 .y = wy - e->server->screen_height / 2.0,
                 .z = a.z + (b.z - a.z) * s };
    return p;
}

/* Ring space → a projected vertex. The tilt is undone here, which is the whole
 * of the camera: everything else is one perspective divide. */
static struct scene3d_vert expo_project(FwmExpo *e, ExpoPt p, double u, double v) {
    double ct = cos(e->tilt), st = sin(e->tilt);
    double y =  p.y * ct + p.z * st;
    double z = -p.y * st + p.z * ct;

    double w = expo_dist(e) - z;
    double f = expo_focal(e);
    struct scene3d_vert out = {
        .x = (float)(e->server->screen_width / 2.0 + f * p.x / (w > 1.0 ? w : 1.0)),
        .y = (float)(e->server->screen_height / 2.0 + f * y / (w > 1.0 ? w : 1.0)),
        .w = (float)w,
        .u = (float)u, .v = (float)v,
    };
    return out;
}

/* Where a desktop's card stands along the strip. Always the nearest way round,
 * whether or not the ring is CLOSED: the strip lies on a circle either way, and
 * the seam is what says the two ends have not met. Skipping this while the
 * strip was a "line" is what made the end card appear out of nowhere as the
 * seam shrank. */
static double expo_desktop_strip_x(FwmExpo *e, int desktop) {
    double x = (double)desktop * expo_pitch(e);
    double lap = expo_lap(e);
    double centre = expo_center(e) - e->server->screen_width / 2.0;
    while (x - centre >  lap / 2.0) x -= lap;
    while (x - centre < -lap / 2.0) x += lap;
    return x;
}

/* World point on a desktop → strip offset from the middle of the screen. */
static double expo_offset(FwmExpo *e, double wx, int desktop) {
    return expo_desktop_strip_x(e, desktop)
         + (wx - (double)desktop * e->server->screen_width)
         - expo_center(e);
}

/* The two ends of a desktop's facet, in ring space. */
static void expo_facet_ends(FwmExpo *e, int desktop, ExpoPt *a, ExpoPt *b) {
    double sw = e->server->screen_width;
    *a = expo_ring_point(e, expo_offset(e, (double)desktop * sw, desktop), 0);
    *b = expo_ring_point(e, expo_offset(e, ((double)desktop + 1.0) * sw, desktop), 0);
}

/* A world point on a desktop → the screen. Through the FACET, not the arc: a
 * desktop is the chord between its two ends, and a point half way along it
 * stands a sagitta inside the circle. Placing it on the arc instead put it a
 * tenth of a desktop out — invisible while only the flat path used this, and
 * hundreds of pixels of disagreement with the ray the moment anything checked
 * one against the other. */
static void expo_to_screen(FwmExpo *e, double wx, double wy, int desktop,
                           double *sx, double *sy) {
    ExpoPt a, b;
    expo_facet_ends(e, desktop, &a, &b);
    double s = (wx - (double)desktop * e->server->screen_width)
             / e->server->screen_width;
    struct scene3d_vert v = expo_project(e, expo_facet_point(e, a, b, s, wy), 0, 0);
    if (sx) *sx = v.x;
    if (sy) *sy = v.y;
}

/* ── input back down the same path ────────────────────────────────────────
 *
 * A closed-form inverse died with the free camera, and good riddance: a ray
 * against the facets is both more general and shorter. It is still the ONE
 * place that knows how the strip is shaped — the cursor and the cards cannot
 * disagree, because they are the same function read in opposite directions. */

/* The line of sight through a screen point, in ring space. */
static void expo_ray(FwmExpo *e, double sx, double sy, ExpoPt *origin, ExpoPt *dir) {
    double f = expo_focal(e), d = expo_dist(e);
    double X = (sx - e->server->screen_width / 2.0) / f;
    double Y = (sy - e->server->screen_height / 2.0) / f;
    double ct = cos(e->tilt), st = sin(e->tilt);

    /* Camera space runs (X w, Y w, D - w) as w grows; rotating that back into
     * ring space is linear in w, so the eye is the w = 0 end and the direction
     * is what one unit of w adds. */
    origin->x = 0.0;
    origin->y = -d * st;
    origin->z =  d * ct;
    dir->x = X;
    dir->y = Y * ct + st;
    dir->z = Y * st - ct;
}

/* Screen point → desktop and the world point on it. False when the line of
 * sight misses the strip entirely (the backdrop, or straight through the
 * seam), which callers must treat as "nothing there" rather than as desktop 0. */
static bool expo_point(FwmExpo *e, double sx, double sy,
                       int *desktop, double *wx, double *wy) {
    ExpoPt o, dir;
    expo_ray(e, sx, sy, &o, &dir);

    double sw = e->server->screen_width;
    double best_t = 0.0;
    bool found = false;

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        ExpoPt a, b;
        expo_facet_ends(e, d, &a, &b);
        double ex = b.x - a.x, ez = b.z - a.z;
        /* The facet's plane: its horizontal edge crossed with straight down,
         * which points OUT of the drum. A ray running with that normal rather
         * than against it is looking at the card's back, from inside the ring:
         * it is a surface the eye cannot see, and a click must never land on
         * it — the nearer card in front of it owns that pixel. */
        double nx = -ez, nz = ex;
        double denom = nx * dir.x + nz * dir.z;
        if (denom >= -1e-9) continue;   /* edge-on, or seen from behind */
        double t = (nx * (a.x - o.x) + nz * (a.z - o.z)) / denom;
        if (t <= 0.0) continue;                       /* behind the viewer */
        if (found && t >= best_t) continue;           /* something nearer already */

        double hx = o.x + dir.x * t, hz = o.z + dir.z * t;
        double len2 = ex * ex + ez * ez;
        if (len2 < 1e-9) continue;
        double s = ((hx - a.x) * ex + (hz - a.z) * ez) / len2;
        if (s < 0.0 || s > 1.0) continue;             /* past the card's ends */

        best_t = t;
        found = true;
        *desktop = d;
        *wx = (double)d * sw + s * sw;
        *wy = o.y + dir.y * t + e->server->screen_height / 2.0;
    }
    return found;
}

/* Where the strip is looking, as a fractional desktop index — the same number
 * camera_x/screen_width is when the strip is closed. The tray reads it so its
 * marker keeps saying where you are while the strip pans; without that it went
 * on pointing at the desktop you entered from. */
static double expo_position_for(FwmExpo *e, double pan) {
    FwmServer *server = e->server;
    double centre = server->camera_x + server->screen_width / 2.0
                  + e->home * expo_gap(e) + pan;
    double p = (centre - server->screen_width / 2.0) / expo_pitch(e);
    if (server->config.camera.wrap) {
        p = fmod(p, (double)FWM_DESKTOPS);
        if (p < 0.0) p += FWM_DESKTOPS;
    }
    if (p < 0.0) p = 0.0;
    if (p > FWM_DESKTOPS - 1) p = FWM_DESKTOPS - 1;
    return p;
}

bool expo_view_position(FwmServer *server, double *pos) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    double p = expo_position_for(e, e->pan);
    *pos = p;
    return true;
}

static int expo_desktop_at(FwmExpo *e, double pan) {
    int d = (int)lround(expo_position_for(e, pan));
    if (d < 0) d = 0;
    if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;
    return d;
}

int expo_view_desktop(FwmServer *server) {
    FwmExpo *e = server->expo;
    return e ? expo_desktop_at(e, e->pan) : -1;
}

int expo_target_desktop(FwmServer *server) {
    /* Where the strip is HEADED, which is what a step must be measured from:
     * firing next twice in quick succession has to advance two desktops rather
     * than fight a pan still in flight. Same rule the tray's wheel follows. */
    FwmExpo *e = server->expo;
    return e ? expo_desktop_at(e, e->pan_target) : -1;
}

/* ── the pictures ─────────────────────────────────────────────────────── */

static void expo_build_cards(FwmExpo *e) {
    FwmServer *server = e->server;
    FwmWallpaper *wp = server->wallpaper;
    int layers = wallpaper_layer_count(wp);
    if (layers > EXPO_MAX_WP_LAYERS) layers = EXPO_MAX_WP_LAYERS;

    if (e->gl) {
        /* The curved path needs textures and crops, not nodes: the cards are
         * drawn from them every frame. Imported once — re-importing a buffer
         * sixty times a second is pure waste, and the wallpaper's copy never
         * changes anyway. */
        for (int i = 0; i < layers; i++) {
            struct wlr_buffer *buf = wallpaper_layer_buffer(wp, i);
            if (!buf) continue;
            struct wlr_texture *tex =
                wlr_texture_from_buffer(server->wlr_renderer, buf);
            if (!tex) continue;
            int idx = e->wp_n++;
            e->wp_tex[idx] = tex;
            for (int d = 0; d < FWM_DESKTOPS; d++)
                wallpaper_layer_crop(wp, i, d * server->screen_width,
                                     server->screen_width, server->screen_height,
                                     &e->wp_crop[d][idx]);
        }
        wlr_log(WLR_DEBUG, "expo: curved strip, %d wallpaper layer(s)", e->wp_n);
        return;
    }

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        e->cards[d] = wlr_scene_tree_create(e->tree);
        if (!e->cards[d]) continue;

        /* Bottom of the card: the edge, then the base over all but its margin.
         * A desktop with no wallpaper (or one whose image failed to load) still
         * has to read as a slot on the strip rather than as a hole in it. */
        e->card_edge[d] = wlr_scene_rect_create(e->cards[d], 1, 1,
            (float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY, EXPO_EDGE_GREY, 1.0f});
        e->card_base[d] = wlr_scene_rect_create(e->cards[d], 1, 1,
            (float[4]){EXPO_CARD_GREY, EXPO_CARD_GREY, EXPO_CARD_GREY, 1.0f});

        for (int i = 0; i < layers; i++) {
            struct wlr_buffer *buf = wallpaper_layer_buffer(wp, i);
            if (!buf) continue;
            struct wlr_scene_buffer *node = wlr_scene_buffer_create(e->cards[d], buf);
            if (!node) continue;
            /* The pan layers are wider than the screen and scroll behind it;
             * the crop is where that layer stands when the camera is parked on
             * THIS desktop, which is what makes the ten cards differ. */
            struct wlr_fbox crop;
            wallpaper_layer_crop(wp, i, d * server->screen_width,
                                 server->screen_width, server->screen_height, &crop);
            wlr_scene_buffer_set_source_box(node, &crop);
            e->card_layers[d][e->card_layer_n[d]++] = node;
        }
    }
    /* One line, because "the cards are grey" has now twice turned out to mean
     * "the wallpaper handed us nothing", and that is invisible from the outside. */
    wlr_log(WLR_DEBUG, "expo: %d cards, %d wallpaper layer(s) each",
            FWM_DESKTOPS, e->card_layer_n[0]);
}

static void expo_snap_windows(FwmExpo *e) {
    FwmServer *server = e->server;
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (e->n_items >= MAX_WINDOWS) break;
        if (!view->scene_tree || !view->scene_tree->node.enabled) continue;
        if (view->width <= 0 || view->height <= 0) continue;

        int bw = (int)lround(view->width  * EXPO_SNAP_SCALE);
        int bh = (int)lround(view->height * EXPO_SNAP_SCALE);
        struct wlr_buffer *buf = snapshot_alloc(server, bw, bh);
        if (!buf) continue;

        int lx = 0, ly = 0;
        wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
        if (!snapshot_subtree(server, buf, &view->scene_tree->node,
                              lx, ly, EXPO_SNAP_SCALE)) {
            wlr_buffer_drop(buf);
            continue;
        }

        struct wlr_scene_buffer *node = NULL;
        struct wlr_texture *tex = NULL;
        if (e->gl) {
            tex = wlr_texture_from_buffer(server->wlr_renderer, buf);
            if (!tex) { wlr_buffer_drop(buf); continue; }
        } else {
            node = wlr_scene_buffer_create(e->tree, buf);
            if (!node) { wlr_buffer_drop(buf); continue; }
        }

        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        int d = b ? b->desktop_id : view->x / server->screen_width;
        if (d < 0) d = 0;
        if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;

        ExpoItem *it = &e->items[e->n_items++];
        it->view = view;
        it->node = node;
        it->tex = tex;
        it->buf = wlr_buffer_lock(buf);
        it->wx = view->x;
        it->wy = view->y;
        it->w = view->width;
        it->h = view->height;
        it->desktop = d;
        wlr_buffer_drop(buf);
    }
}

/* ── drawing the ring ─────────────────────────────────────────────────── */

/* Vertex order everywhere below: top-left, bottom-left, top-right,
 * bottom-right — a triangle strip, and the winding the visibility test reads. */
enum { QTL = 0, QBL, QTR, QBR };

static void expo_quad(FwmExpo *e, ExpoPt a, ExpoPt b, double s0, double s1,
                      double wy0, double wy1, double u0, double u1,
                      struct scene3d_vert q[4]) {
    q[QTL] = expo_project(e, expo_facet_point(e, a, b, s0, wy0), u0, 0.0f);
    q[QBL] = expo_project(e, expo_facet_point(e, a, b, s0, wy1), u0, 1.0f);
    q[QTR] = expo_project(e, expo_facet_point(e, a, b, s1, wy0), u1, 0.0f);
    q[QBR] = expo_project(e, expo_facet_point(e, a, b, s1, wy1), u1, 1.0f);
}

/* Behind the eye, turned away, or off the screen.
 *
 * The winding test is what replaces every angle-based cull the cylinder
 * needed: a facet on the far side of the drum comes out wound backwards, and
 * so does one folded past the horizon. It is also the only test that stays
 * right once the camera can be anywhere. */
static bool expo_quad_visible(FwmExpo *e, const struct scene3d_vert q[4]) {
    for (int i = 0; i < 4; i++)
        if (q[i].w <= 1.0f) return false;

    const struct scene3d_vert *ring[4] = { &q[QTL], &q[QTR], &q[QBR], &q[QBL] };
    double area = 0.0;
    for (int i = 0; i < 4; i++) {
        const struct scene3d_vert *p = ring[i], *n = ring[(i + 1) % 4];
        area += (double)p->x * n->y - (double)n->x * p->y;
    }
    if (area <= 0.0) return false;      /* y is down, so front-facing is positive */

    float lo = q[0].x, hi = q[0].x, top = q[0].y, bot = q[0].y;
    for (int i = 1; i < 4; i++) {
        if (q[i].x < lo) lo = q[i].x;
        if (q[i].x > hi) hi = q[i].x;
        if (q[i].y < top) top = q[i].y;
        if (q[i].y > bot) bot = q[i].y;
    }
    return hi >= -8.0f && lo <= e->server->screen_width + 8.0f
        && bot >= -8.0f && top <= e->server->screen_height + 8.0f;
}

/* The same quad grown by `m` screen px, for the frame drawn behind a card or a
 * hovered window. Pushing each corner away from the centre is exact enough at
 * these sizes and needs no plane of its own. */
static void expo_inflate(struct scene3d_vert q[4], double m) {
    double cx = 0.0, cy = 0.0;
    for (int i = 0; i < 4; i++) { cx += q[i].x; cy += q[i].y; }
    cx /= 4.0; cy /= 4.0;
    for (int i = 0; i < 4; i++) {
        double dx = q[i].x - cx, dy = q[i].y - cy;
        double len = sqrt(dx * dx + dy * dy);
        if (len < 1e-6) continue;
        q[i].x += (float)(dx / len * m);
        q[i].y += (float)(dy / len * m);
    }
}

static void expo_draw_item(FwmExpo *e, ExpoItem *it, double scale) {
    if (!it->tex) return;
    FwmServer *server = e->server;

    ExpoPt a, b;
    expo_facet_ends(e, it->desktop, &a, &b);
    double sw = server->screen_width;
    double s0 = (it->wx - (double)it->desktop * sw) / sw;
    double s1 = (it->wx + it->w - (double)it->desktop * sw) / sw;

    struct scene3d_vert q[4], frame[4];
    expo_quad(e, a, b, s0, s1, it->wy, it->wy + it->h, 0.0, 1.0, q);
    if (!expo_quad_visible(e, q)) return;

    if (e->hover == it) {
        const FwmTheme *th = theme_get();
        double m = EXPO_HILIGHT_PX * scale;
        if (m < 2.0) m = 2.0;
        memcpy(frame, q, sizeof(frame));
        expo_inflate(frame, m);
        scene3d_quad_solid((float[4]){(float)th->accent[0], (float)th->accent[1],
                                      (float)th->accent[2], 0.9f}, frame);
    }
    scene3d_quad(it->tex, q, 1.0f);
}

static void expo_draw_card(FwmExpo *e, int d, int looking_at, double open,
                           double scale) {
    ExpoPt a, b;
    expo_facet_ends(e, d, &a, &b);

    struct scene3d_vert q[4], frame[4];
    expo_quad(e, a, b, 0.0, 1.0, 0, e->server->screen_height, 0.0, 1.0, q);
    if (!expo_quad_visible(e, q)) return;

    /* The edge is a filled shape UNDER the card, so only its margin is ever
     * seen — the only way to frame a turned quad without four more of them. */
    const FwmTheme *th = theme_get();
    memcpy(frame, q, sizeof(frame));
    expo_inflate(frame, EXPO_EDGE_PX * open);
    if (d == looking_at) {
        scene3d_quad_solid((float[4]){(float)th->accent[0], (float)th->accent[1],
                                      (float)th->accent[2], 1.0f}, frame);
    } else {
        scene3d_quad_solid((float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY,
                                      EXPO_EDGE_GREY, 1.0f}, frame);
    }

    scene3d_quad_solid((float[4]){EXPO_CARD_GREY, EXPO_CARD_GREY,
                                  EXPO_CARD_GREY, 1.0f}, q);

    for (int i = 0; i < e->wp_n; i++) {
        struct wlr_texture *tex = e->wp_tex[i];
        if (!tex || tex->width <= 0) continue;
        const struct wlr_fbox *crop = &e->wp_crop[d][i];
        struct scene3d_vert w[4];
        memcpy(w, q, sizeof(w));
        float uu0 = (float)(crop->x / tex->width);
        float uu1 = (float)((crop->x + crop->width) / tex->width);
        w[QTL].u = w[QBL].u = uu0;
        w[QTR].u = w[QBR].u = uu1;
        scene3d_quad(tex, w, 1.0f);
    }

    /* The windows of this desktop, on top of it and under the next card: with
     * the camera lifted, a window on the far side must not be painted over the
     * near side, and per-facet order is what says so. */
    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop == d && it != e->drag) expo_draw_item(e, it, scale);
    }
}

/* One end of the drum, so a ring looked at from above is a lid and not a view
 * into its own far side. Drawn as separate triangles rather than one fan: a
 * segment can have a corner behind the eye, and dropping it must not tear the
 * rest apart. */
static void expo_draw_cap(FwmExpo *e, double wy) {
    double r = expo_radius(e);
    if (r <= 0.0) return;

    ExpoPt centre = { .x = 0.0, .y = wy - e->server->screen_height / 2.0,
                      .z = -r };
    struct scene3d_vert c = expo_project(e, centre, 0.0, 0.0);
    if (c.w <= 1.0f) return;

    double span = FWM_DESKTOPS * expo_pitch(e);   /* the cards, not the seam */
    double u0 = expo_offset(e, 0, 0);
    int segs = (int)ceil(span / r / EXPO_CAP_SEG);
    if (segs < 3) segs = 3;
    if (segs > SCENE3D_MAX_FAN) segs = SCENE3D_MAX_FAN;

    for (int i = 0; i < segs; i++) {
        ExpoPt p0 = expo_ring_point(e, u0 + span * i / segs, wy);
        ExpoPt p1 = expo_ring_point(e, u0 + span * (i + 1) / segs, wy);
        struct scene3d_vert tri[3] = {
            c,
            expo_project(e, p0, 0.0, 0.0),
            expo_project(e, p1, 0.0, 0.0),
        };
        if (tri[1].w <= 1.0f || tri[2].w <= 1.0f) continue;
        scene3d_fan_solid((float[4]){EXPO_CAP_GREY, EXPO_CAP_GREY,
                                     EXPO_CAP_GREY, 1.0f}, tri, 3);
    }
}

/* True when the picture would come out identical to the one already on screen. */
static bool expo_canvas_current(FwmExpo *e) {
    FwmServer *server = e->server;
    bool same = e->drawn_zoom == e->zoom
             && e->drawn_seam == e->seam
             && e->drawn_pan == e->pan
             && e->drawn_tilt == e->tilt
             && e->drawn_dist == e->dist
             && e->drawn_cam == server->camera_x
             && e->drawn_items == e->n_items
             && e->drawn_wrap == server->config.camera.wrap
             && e->drawn_hover == (void *)e->hover
             && e->drawn_drag == (void *)e->drag
             && (!e->drag || (e->drawn_drag_x == e->drag->wx
                           && e->drawn_drag_y == e->drag->wy));
    if (same) return true;

    e->drawn_zoom = e->zoom;
    e->drawn_seam = e->seam;
    e->drawn_pan = e->pan;
    e->drawn_tilt = e->tilt;
    e->drawn_dist = e->dist;
    e->drawn_cam = server->camera_x;
    e->drawn_items = e->n_items;
    e->drawn_wrap = server->config.camera.wrap;
    e->drawn_hover = e->hover;
    e->drawn_drag = e->drag;
    if (e->drag) { e->drawn_drag_x = e->drag->wx; e->drawn_drag_y = e->drag->wy; }
    return false;
}

static void expo_draw_gl(FwmExpo *e) {
    FwmServer *server = e->server;
    if (expo_canvas_current(e)) return;
    int idx = e->canvas_i ^ 1;
    struct wlr_buffer *dst = e->canvas_buf[idx];
    if (!dst || !e->canvas) return;
    if (!scene3d_begin(server->wlr_renderer, dst)) return;  /* keep last frame */

    double open = expo_openness(e);
    double scale = expo_scale(e);
    int looking_at = expo_view_desktop(server);

    /* The lid the camera is on the outside of, first: the near cards are drawn
     * over it, and the far ones are wound backwards and never drawn at all. */
    if (e->tilt > 0.001) expo_draw_cap(e, 0);
    else if (e->tilt < -0.001) expo_draw_cap(e, server->screen_height);

    /* Far to near. A flat facet cannot intersect another, so the order of the
     * whole card — its edge, its wallpaper and its windows together — is all
     * the depth sorting a drum needs. */
    int order[FWM_DESKTOPS];
    double key[FWM_DESKTOPS];
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        ExpoPt a, b;
        expo_facet_ends(e, d, &a, &b);
        ExpoPt mid = { (a.x + b.x) / 2.0, 0.0, (a.z + b.z) / 2.0 };
        order[d] = d;
        key[d] = expo_project(e, mid, 0, 0).w;
    }
    for (int i = 1; i < FWM_DESKTOPS; i++) {
        for (int j = i; j > 0 && key[order[j]] > key[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    }
    for (int i = 0; i < FWM_DESKTOPS; i++)
        expo_draw_card(e, order[i], looking_at, open, scale);

    /* The window in hand is above everything, wherever it is being carried. */
    if (e->drag) expo_draw_item(e, e->drag, scale);

    scene3d_end();

    wlr_scene_buffer_set_buffer_with_damage(e->canvas, dst, NULL);
    e->canvas_i = idx;
}

/* ── layout ───────────────────────────────────────────────────────────── */

/* Place every node from the current zoom and pan. Cheap enough to run on every
 * frame, which is what lets the whole animation be two scalars. */
static void expo_layout(FwmExpo *e) {
    FwmServer *server = e->server;
    double s = expo_scale(e);
    double open = expo_openness(e);
    int looking_at = expo_view_desktop(server);

    if (e->backdrop) {
        /* Fade in with the zoom, so a strip that is barely open is barely
         * darker than the desktop it grew out of. */
        float a = (float)(EXPO_BACKDROP_ALPHA * open);
        wlr_scene_rect_set_color(e->backdrop, (float[4]){0.0f, 0.0f, 0.0f, a});
    }

    if (e->gl) {
        expo_draw_gl(e);
        return;
    }

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (!e->cards[d]) continue;
        /* A card's corners in WORLD coordinates: desktop d starts at
         * d * screen_width, not at zero. Passing the desktop-local box here
         * drew all ten cards on top of each other, a gap apart — a stack of
         * slivers where the strip should have been. */
        double x0, y0, x1, y1;
        expo_to_screen(e, (double)d * server->screen_width, 0, d, &x0, &y0);
        expo_to_screen(e, (double)(d + 1) * server->screen_width,
                       server->screen_height, d, &x1, &y1);
        int cw = (int)lround(x1 - x0), chh = (int)lround(y1 - y0);
        if (cw < 1) cw = 1;
        if (chh < 1) chh = 1;
        wlr_scene_node_set_position(&e->cards[d]->node,
                                    (int)lround(x0), (int)lround(y0));
        if (e->card_base[d]) wlr_scene_rect_set_size(e->card_base[d], cw, chh);
        for (int i = 0; i < e->card_layer_n[d]; i++)
            wlr_scene_buffer_set_dest_size(e->card_layers[d][i], cw, chh);

        if (e->card_edge[d]) {
            /* A fixed number of SCREEN px, not scaled with the strip: the edge
             * says where a desktop ends, and at the near zoom step a scaled one
             * would be a third of a pixel. It fades in with the gap so the live
             * view is not framed. */
            int m = (int)lround(EXPO_EDGE_PX * open);
            wlr_scene_rect_set_size(e->card_edge[d], cw + 2 * m, chh + 2 * m);
            wlr_scene_node_set_position(&e->card_edge[d]->node, -m, -m);
            const FwmTheme *th = theme_get();
            bool here = d == looking_at;
            wlr_scene_rect_set_color(e->card_edge[d], here
                ? (float[4]){(float)th->accent[0], (float)th->accent[1],
                             (float)th->accent[2], 1.0f}
                : (float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY, EXPO_EDGE_GREY, 1.0f});
        }
    }

    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        double x0, y0, x1, y1;
        expo_to_screen(e, it->wx, it->wy, it->desktop, &x0, &y0);
        expo_to_screen(e, it->wx + it->w, it->wy + it->h, it->desktop, &x1, &y1);
        wlr_scene_buffer_set_dest_size(it->node,
                                       (int)lround(x1 - x0), (int)lround(y1 - y0));
        wlr_scene_node_set_position(&it->node->node, (int)lround(x0), (int)lround(y0));
    }

    if (e->hilight) {
        if (e->hover) {
            ExpoItem *it = e->hover;
            int m = (int)lround(EXPO_HILIGHT_PX * s);
            if (m < 1) m = 1;
            double x0, y0, x1, y1;
            expo_to_screen(e, it->wx, it->wy, it->desktop, &x0, &y0);
            expo_to_screen(e, it->wx + it->w, it->wy + it->h, it->desktop, &x1, &y1);
            wlr_scene_rect_set_size(e->hilight,
                                    (int)lround(x1 - x0) + 2 * m,
                                    (int)lround(y1 - y0) + 2 * m);
            wlr_scene_node_set_position(&e->hilight->node,
                                        (int)lround(x0) - m, (int)lround(y0) - m);
            wlr_scene_node_set_enabled(&e->hilight->node, true);
        } else {
            wlr_scene_node_set_enabled(&e->hilight->node, false);
        }
    }
}

/* ── the live scene underneath ────────────────────────────────────────── */

/* What the strip replaces while it is up. Deliberately NOT the whole world:
 *
 *   layer_background stays — the live wallpaper is the strip's own backdrop,
 *   dimmed by the rect over it, so the cards float on a picture rather than on
 *   a black field;
 *   layer_overlay stays — the tray belongs to the session, not to whatever is
 *   underneath it, and a strip that swallowed the clock and the mode pills
 *   read as a different machine. The strip's tree sits BELOW it for that
 *   reason. */
static void expo_set_world_visible(FwmServer *server, bool visible) {
    struct wlr_scene_tree *trees[] = {
        server->ls_background, server->ls_bottom,
        server->layer_windows, server->ls_top, server->ls_overlay,
    };
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++)
        if (trees[i]) wlr_scene_node_set_enabled(&trees[i]->node, visible);
}

/* FWM_TEST_ORBIT=<radians> opens the strip already zoomed out and tilted, so a
 * nested run can be photographed from an angle no keypress can be injected to
 * reach; FWM_TEST_ORBIT_DIST pulls the camera back as well.
 *
 * It also round-trips the projection against the ray that inverts it, over a
 * grid of world points on every desktop. That check earns its keep: the ray is
 * the one half of this that a screenshot cannot show, and it caught the two
 * real bugs here — a facet hit from inside the drum, and expo_to_screen
 * placing a point on the arc where the facet is a chord. Points that come back
 * "missed" are the ones the eye cannot see either: occluded, or on the far
 * wall. */
static void expo_selftest(FwmExpo *e) {
    FwmServer *server = e->server;
    if (!getenv("FWM_TEST_ORBIT")) return;

    double worst = 0.0;
    int misses = 0, tried = 0;
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        for (int i = 0; i <= 4; i++) for (int j = 0; j <= 4; j++) {
            double wx = (double)d * server->screen_width
                      + server->screen_width * i / 4.0;
            double wy = server->screen_height * j / 4.0;
            double sx, sy;
            expo_to_screen(e, wx, wy, d, &sx, &sy);
            if (sx < 0 || sx > server->screen_width) continue;
            if (sy < 0 || sy > server->screen_height) continue;
            tried++;
            int gd; double gx, gy;
            if (!expo_point(e, sx, sy, &gd, &gx, &gy) || gd != d) { misses++; continue; }
            double err = fabs(gx - wx) + fabs(gy - wy);
            if (err > worst) worst = err;
        }
    }
    wlr_log(WLR_INFO, "expo: ray round-trip over %d points: worst %.3f px, %d hidden",
            tried, worst, misses);
}

/* ── open and close ───────────────────────────────────────────────────── */

static void expo_teardown(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e) return;
    server->expo = NULL;    /* before anything else: nothing may re-enter here */

    expo_menu_close(e);
    for (int i = 0; i < e->n_items; i++) {
        if (e->items[i].tex) wlr_texture_destroy(e->items[i].tex);
        if (e->items[i].buf) wlr_buffer_unlock(e->items[i].buf);
    }
    for (int i = 0; i < e->wp_n; i++)
        if (e->wp_tex[i]) wlr_texture_destroy(e->wp_tex[i]);
    for (int i = 0; i < 2; i++)
        if (e->canvas_buf[i]) wlr_buffer_unlock(e->canvas_buf[i]);
    if (e->tree) wlr_scene_node_destroy(&e->tree->node);

    /* Never hand the desktop back while the session is locked: lock.c hid
     * these trees for its own reasons and it, not the strip, decides when they
     * come back. */
    if (!lock_is_active(server)) expo_set_world_visible(server, true);
    free(e);
}

bool expo_active(FwmServer *server) {
    return server->expo != NULL;
}

bool expo_animating(FwmServer *server) {
    FwmExpo *e = server->expo;
    return e && (fabs(e->zoom - e->zoom_target) > 0.001
              || fabs(e->pan - e->pan_target) > 0.5
              || fabs(e->seam - e->seam_target) > 0.001
              || fabs(e->tilt - e->tilt_target) > 0.0005
              || fabs(e->dist - e->dist_target) > 0.0005);
}

static void expo_open(FwmServer *server) {
    if (server->expo || server->screen_width <= 0) return;
    /* Not over a lock, and not in the middle of a drag or a resize: the strip
     * takes the pointer away, and the interactive gesture would be left holding
     * a window nobody can reach. The launcher is the same argument the other
     * way round — it owns the keyboard, and two things cannot. */
    if (lock_is_active(server)) return;
    if (server->interactive.action != FWM_ACTION_NONE) return;
    if (launcher_is_open(server->launcher)) return;

    FwmExpo *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->server = server;
    e->zoom = 1.0;
    e->zoom_target = EXPO_ZOOM_NEAR;
    e->drawn_zoom = -1.0;   /* nothing drawn yet; the first layout must run */
    e->seam = e->seam_target = server->config.camera.wrap ? 0.0 : 1.0;
    e->dist = e->dist_target = 1.0;
    { const char *t = getenv("FWM_TEST_ORBIT");
      if (t) { e->zoom = e->zoom_target = EXPO_ZOOM_FAR; e->tilt = e->tilt_target = atof(t);
               const char *dd = getenv("FWM_TEST_ORBIT_DIST");
               if (dd) e->dist = e->dist_target = atof(dd); } }
    e->home = (server->camera_x + server->screen_width / 2) / server->screen_width;
    if (e->home < 0) e->home = 0;
    if (e->home >= FWM_DESKTOPS) e->home = FWM_DESKTOPS - 1;

    e->tree = wlr_scene_tree_create(&server->scene->tree);
    if (!e->tree) { free(e); return; }
    /* Above the windows it is replacing, below the tray it is not. */
    wlr_scene_node_place_below(&e->tree->node, &server->layer_overlay->node);

    e->backdrop = wlr_scene_rect_create(e->tree, server->screen_width,
                                        server->screen_height,
                                        (float[4]){0.0f, 0.0f, 0.0f, 0.0f});

    /* Turned cards need a renderer that can draw a trapezoid. Where there is
     * one, the strip is one hand-drawn buffer over the backdrop; where there is
     * not, it stays the flat set of scene nodes and nothing else changes. */
    e->gl = rotate_supported(server->wlr_renderer);
    if (e->gl) {
        for (int i = 0; i < 2; i++) {
            struct wlr_buffer *b = snapshot_alloc(server, server->screen_width,
                                                  server->screen_height);
            if (!b) { e->gl = false; break; }
            e->canvas_buf[i] = wlr_buffer_lock(b);
            wlr_buffer_drop(b);
        }
        if (e->gl) {
            e->canvas = wlr_scene_buffer_create(e->tree, NULL);
            if (!e->canvas) e->gl = false;
            else wlr_scene_node_set_position(&e->canvas->node, 0, 0);
        }
        if (!e->gl) wlr_log(WLR_INFO, "expo: no offscreen buffer, strip stays flat");
    }

    if (!e->gl) {
        e->hilight = wlr_scene_rect_create(e->tree, 1, 1,
                                           (float[4]){0.35f, 0.62f, 0.95f, 0.9f});
        if (e->hilight) wlr_scene_node_set_enabled(&e->hilight->node, false);
    }

    server->expo = e;
    expo_build_cards(e);
    /* The highlight is a frame BEHIND the window it marks, so the window's own
     * picture covers all but the border. Windows are created after it and
     * therefore already sit above; the cards must not. */
    if (e->hilight) wlr_scene_node_raise_to_top(&e->hilight->node);
    expo_snap_windows(e);

    expo_set_world_visible(server, false);
    expo_layout(e);

    expo_selftest(e);

    /* Nothing under the cursor belongs to a client any more, and a client that
     * keeps pointer focus would go on receiving motion through the strip. */
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    /* The tray stays on screen, but its menu hangs off a pill that clicks can
     * no longer reach while the strip owns the pointer. */
    server_close_modes_menu(server);
}

/* Start leaving, landing on `desktop`. */
static void expo_close(FwmServer *server, int desktop) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return;
    if (desktop < 0) desktop = 0;
    if (desktop >= FWM_DESKTOPS) desktop = FWM_DESKTOPS - 1;

    e->leaving = 1;
    e->zoom_target = 1.0;
    expo_camera_home(e);
    e->hover = NULL;
    e->drag = NULL;
    expo_menu_close(e);

    /* Put the live camera on the destination NOW, while it is still hidden:
     * the closing animation is written against camera_x (expo_center), so the
     * strip flies to exactly the view that is revealed underneath it, and the
     * two cannot disagree by a frame at the moment of the swap.
     *
     * Moving the camera moves the strip's own centre with it, so `pan` is
     * rebased to cancel that out exactly: the picture must not jump on the
     * frame the destination is chosen, only travel there. */
    double was_looking_at = expo_center(e);
    e->home = desktop;
    server->camera_x = desktop * server->screen_width;
    e->pan = was_looking_at - (server->camera_x + server->screen_width / 2.0
                               + e->home * expo_gap(e));
    if (server->config.camera.wrap) {
        /* On a ring the rebased offset can be most of a circle even though the
         * picture did not move; fly the short way home. */
        double lap = expo_lap(e);
        while (e->pan >  lap / 2.0) e->pan -= lap;
        while (e->pan < -lap / 2.0) e->pan += lap;
    }
    e->pan_target = 0.0;
    server->target_camera_x = server->camera_x;
    server->cam_anim = 0;
    server->cam_free = 0;
    server_camera_settled(server);
    if (server->wallpaper) wallpaper_update(server->wallpaper, server->camera_x);
    server_request_tray_redraw(server);
}

void expo_toggle(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e) {
        expo_open(server);
    } else if (e->leaving) {
        /* Caught on the way out: turn round rather than stacking a second
         * strip on top of the one still animating. */
        e->leaving = 0;
        e->zoom_target = EXPO_ZOOM_NEAR;
    } else {
        expo_close(server, expo_view_desktop(server));
    }
}

/* The far zoom step is the looking-at-it step, and the only one the camera may
 * leave the canonical view in. Read from the TARGET, so the controls come
 * alive as soon as `z` is pressed rather than when the zoom finishes. */
static bool expo_can_orbit(FwmExpo *e) {
    /* Not on the fallback path: a scene node is an axis-aligned rectangle, and
     * a strip that cannot even be curved certainly cannot be flown around. */
    return e->gl && !e->leaving
        && e->zoom_target > (EXPO_ZOOM_NEAR + EXPO_ZOOM_FAR) / 2.0;
}

/* Back to level, from anywhere. Everything that means "act on a desktop"
 * rather than "look at the ring" goes through here. */
static void expo_camera_home(FwmExpo *e) {
    e->tilt_target = 0.0;
    e->dist_target = 1.0;
    e->orbiting = 0;
}

void expo_zoom_step(FwmServer *server) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return;
    e->zoom_target = e->zoom_target > (EXPO_ZOOM_NEAR + EXPO_ZOOM_FAR) / 2.0
                   ? EXPO_ZOOM_NEAR : EXPO_ZOOM_FAR;
}

void expo_destroy(FwmServer *server) {
    expo_teardown(server);
}

void expo_forget_view(FwmServer *server, FwmView *view) {
    FwmExpo *e = server->expo;
    if (!e) return;
    for (int i = 0; i < e->n_items; i++) {
        if (e->items[i].view != view) continue;
        ExpoItem *it = &e->items[i];
        if (e->hover == it) e->hover = NULL;
        if (e->drag == it) e->drag = NULL;
        if (e->menu_item == it) expo_menu_close(e);
        if (it->node) wlr_scene_node_destroy(&it->node->node);
        if (it->tex) wlr_texture_destroy(it->tex);
        if (it->buf) wlr_buffer_unlock(it->buf);
        /* Compact the slots: the hover/drag pointers are into this array, so
         * the last item moving into the hole has to be followed. */
        ExpoItem *last = &e->items[--e->n_items];
        if (last != it) {
            if (e->hover == last) e->hover = it;
            if (e->drag == last) e->drag = it;
            if (e->menu_item == last) e->menu_item = it;
            *it = *last;
        }
        memset(last, 0, sizeof(*last));
        return;
    }
}

/* ── animation ────────────────────────────────────────────────────────── */



void expo_tick(FwmServer *server, double dt) {
    FwmExpo *e = server->expo;
    if (!e) return;

    /* One 60Hz frame at most. The tick drops to a 200ms heartbeat whenever
     * nothing is moving, and the frame that opens the strip is usually the
     * first after such a beat: fed that dt, an exponential chase covers 94% of
     * the distance in a single step and the strip appears rather than opens. */
    if (dt > 1.0 / 60.0) dt = 1.0 / 60.0;

    /* Exponential chase, framerate-independent: a step retargeted mid-flight is
     * simply a new target, with no ease to restart. */
    double gap = e->zoom_target - e->zoom;
    if (fabs(gap) > 0.0005) e->zoom += gap * (1.0 - exp(-EXPO_ZOOM_SPEED * dt));
    else                    e->zoom = e->zoom_target;

    double pgap = e->pan_target - e->pan;
    if (fabs(pgap) > 0.5) e->pan += pgap * (1.0 - exp(-EXPO_PAN_SPEED * dt));
    else                  e->pan = e->pan_target;

    /* Carrying a window to a desktop that is not on screen. The hand is at the
     * edge and cannot go further, so the strip turns under it instead — and on
     * a closed ring that means a window really can be walked all the way round,
     * which is most of what closing the ring was for. */
    if (e->drag && !e->leaving) {
        double lx = server->cursor->x;
        double band = EXPO_DRAG_EDGE_PX;
        int dir = lx >= server->screen_width - band ? 1 : (lx <= band ? -1 : 0);
        if (dir) {
            /* Faster the closer to the edge: a window nudged into the band
             * drifts, one pressed against the screen edge travels. */
            double into = dir > 0 ? (lx - (server->screen_width - band)) / band
                                  : (band - lx) / band;
            if (into < 0.0) into = 0.0;
            if (into > 1.0) into = 1.0;
            e->pan_target += dir * into * EXPO_DRAG_PAN_SPEED * expo_pitch(e) * dt;
            expo_clamp_pan(e);
            /* No easing while a hand is steering: the window would lag the
             * strip it is being carried over. */
            e->pan = e->pan_target;
            expo_drag_to(e, lx, server->cursor->y);
        }
    }

    /* Read from the config every frame rather than tracked: `x`, a `fwmctl
     * set` and a config reload all mean the same thing here, and none of them
     * has to know the strip is open. */
    /* The orbit is offered at the far zoom step only, and taken away the
     * instant the strip comes back in — a tilted desktop is a thing to look
     * at, not a thing to work in. */
    if (!expo_can_orbit(e)) {
        e->tilt_target = 0.0;
        e->dist_target = 1.0;
        e->orbiting = 0;
    }
    double tgap = e->tilt_target - e->tilt;
    if (fabs(tgap) > 0.0005) e->tilt += tgap * (1.0 - exp(-EXPO_ORBIT_SPEED * dt));
    else                     e->tilt = e->tilt_target;
    double dgap = e->dist_target - e->dist;
    if (fabs(dgap) > 0.0005) e->dist += dgap * (1.0 - exp(-EXPO_ORBIT_SPEED * dt));
    else                     e->dist = e->dist_target;

    e->seam_target = server->config.camera.wrap ? 0.0 : 1.0;
    double sgap = e->seam_target - e->seam;
    if (fabs(sgap) > 0.001) e->seam += sgap * (1.0 - exp(-EXPO_RING_SPEED * dt));
    else                    e->seam = e->seam_target;

    if (e->leaving && e->zoom <= 1.0005) {
        expo_teardown(server);
        return;
    }
    expo_layout(e);
}

/* ── input ────────────────────────────────────────────────────────────── */

static ExpoItem *expo_item_at(FwmExpo *e, double lx, double ly) {
    /* Through the inverse projection, not by testing projected boxes: a turned
     * card's window is a trapezoid on screen, and there is exactly one place
     * that knows how the strip is bent. Back in world coordinates the test is
     * the same rectangle it always was.
     *
     * Later items are drawn on top, so the last match is the one the cursor is
     * actually pointing at. */
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);

    ExpoItem *hit = NULL;
    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop != d) continue;
        if (wx >= it->wx && wx < it->wx + it->w &&
            wy >= it->wy && wy < it->wy + it->h) hit = it;
    }
    return hit;
}

/* Keep the strip within reach: half a screen of overscroll past either end,
 * which is enough to see that there is nothing further and not enough to get
 * lost in. */
static void expo_clamp_pan(FwmExpo *e) {
    double pitch = expo_pitch(e);

    if (e->server->config.camera.wrap) {
        /* A ring has no ends to stop at: keep going and the tenth desktop is
         * followed by the first. Only the accumulated number is bounded, and
         * folding it by a whole circumference is invisible — every card is
         * placed modulo that same circumference. */
        double lap = expo_lap(e);
        while (e->pan_target >  lap / 2.0) { e->pan_target -= lap; e->pan -= lap; }
        while (e->pan_target < -lap / 2.0) { e->pan_target += lap; e->pan += lap; }
        return;
    }

    double half = e->server->screen_width / (2.0 * expo_scale(e));
    double lo = -half - e->home * pitch;
    double hi = (FWM_DESKTOPS - 1 - e->home) * pitch + half;
    if (e->pan_target < lo) e->pan_target = lo;
    if (e->pan_target > hi) e->pan_target = hi;
}

/* Put the dragged window where the cursor is. Through expo_point, so it works
 * at any curvature and on either side of the seam. */
static void expo_drag_to(FwmExpo *e, double lx, double ly) {
    if (!e->drag) return;
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);
    e->drag->wx = wx - e->drag_off_x;
    e->drag->wy = wy - e->drag_off_y;
    e->drag->desktop = d;
}

/* Sends the strip; expo_tick brings it. */
static void expo_pan_by(FwmExpo *e, double strip_px) {
    e->pan_target += strip_px;
    expo_clamp_pan(e);
}

bool expo_goto_desktop(FwmServer *server, int d) {
    FwmExpo *e = server->expo;
    if (!e || e->leaving) return false;
    if (d < 0 || d >= FWM_DESKTOPS) return true;   /* consumed; nowhere to go */
    /* Asking for a desktop is asking to work, so the camera comes level. */
    expo_camera_home(e);

    /* Centre desktop d, by panning rather than by moving camera_x.
     *
     * Moving the camera is what everything ELSE that jumps desktops does, and
     * it would half work here — the strip rides camera_x, so it would slide the
     * right way — but the gaps are anchored on the desktop the strip was
     * entered from, so the landing would be off by (d - home) gaps: most of a
     * card by the far end of the strip. Pan is exact, and it is already eased.
     *
     * strip x of desktop d's centre is d * pitch + screen_width / 2, and
     * expo_center is camera_x + screen_width/2 + home * gap + pan, with
     * camera_x == home * screen_width — which leaves this. */
    e->pan_target = (d - e->home) * expo_pitch(e);
    if (server->config.camera.wrap) {
        /* Round the short way. Stepping from desktop 9 to 0 must travel one
         * card, not nine backwards — the same rule server_goto_desktop follows
         * outside the strip, except that here it is a movement and not a jump,
         * because on a ring there is a picture of the way round. */
        double lap = expo_lap(e);
        while (e->pan_target - e->pan >  lap / 2.0) e->pan_target -= lap;
        while (e->pan_target - e->pan < -lap / 2.0) e->pan_target += lap;
    }
    expo_clamp_pan(e);
    return true;
}

bool expo_handle_key(FwmServer *server, xkb_keysym_t sym) {
    FwmExpo *e = server->expo;
    if (!e) return false;

    switch (sym) {
    case XKB_KEY_Escape:
        expo_toggle(server);
        return true;
    case XKB_KEY_z:
    case XKB_KEY_Z:
        expo_zoom_step(server);
        return true;
    case XKB_KEY_x:
    case XKB_KEY_X:
        /* Close the strip into a ring, or open it back into a line. The same
         * action the `toggle_wrap` bind runs outside the strip — this is a
         * shortcut to it, not a second meaning for it. */
        server_dispatch_action(server, "toggle_wrap");
        return true;
    case XKB_KEY_Left:
        expo_pan_by(e, -expo_pitch(e));
        return true;
    case XKB_KEY_Right:
        expo_pan_by(e, expo_pitch(e));
        return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down: {
        /* Lift the camera over the ring. Only at the far step, where the strip
         * is something to look at — nearer, the arrows have nothing to do and
         * saying so by ignoring them is better than tilting the desktop the
         * user is about to click on. */
        if (!expo_can_orbit(e)) return true;
        double step = sym == XKB_KEY_Up ? EXPO_TILT_STEP : -EXPO_TILT_STEP;
        e->tilt_target += step;
        if (e->tilt_target >  EXPO_TILT_MAX) e->tilt_target =  EXPO_TILT_MAX;
        if (e->tilt_target < -EXPO_TILT_MAX) e->tilt_target = -EXPO_TILT_MAX;
        return true;
    }
    case XKB_KEY_Page_Up:
    case XKB_KEY_Page_Down: {
        if (!expo_can_orbit(e)) return true;
        e->dist_target *= sym == XKB_KEY_Page_Up ? 1.0 / EXPO_DIST_STEP
                                                 : EXPO_DIST_STEP;
        if (e->dist_target < EXPO_DIST_MIN) e->dist_target = EXPO_DIST_MIN;
        if (e->dist_target > EXPO_DIST_MAX) e->dist_target = EXPO_DIST_MAX;
        return true;
    }
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        expo_close(server, expo_view_desktop(server));
        return true;
    default:
        break;
    }
    /* Everything else is swallowed: the strip owns the keyboard while it is up,
     * exactly as the launcher does, so no stray key reaches a client that
     * cannot be seen. */
    return true;
}

bool expo_handle_motion(FwmServer *server, double lx, double ly) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->leaving) return true;

    if (e->orbiting) {
        double dx = lx - e->orbit_x, dy = ly - e->orbit_y;
        e->orbit_x = lx;
        e->orbit_y = ly;

        /* Sideways is the pan the strip always had — turning the ring under a
         * fixed camera is the same thing as flying round it, and reusing it
         * keeps one notion of where the strip is looking. */
        double s = expo_scale(e);
        if (s > 1e-6) {
            e->pan_target -= dx / s;
            expo_clamp_pan(e);
            e->pan = e->pan_target;      /* a hand is steering; no easing */
        }
        e->tilt_target += dy * EXPO_TILT_STEP / 40.0;
        if (e->tilt_target >  EXPO_TILT_MAX) e->tilt_target =  EXPO_TILT_MAX;
        if (e->tilt_target < -EXPO_TILT_MAX) e->tilt_target = -EXPO_TILT_MAX;
        e->tilt = e->tilt_target;
        return true;
    }

    if (e->menu) {
        /* The menu is modal over the cards: while it is open the cursor is
         * choosing a row, not pointing at windows. */
        int mw, mh;
        expo_menu_size(&mw, &mh);
        double mx = lx - e->menu->node.x, my = ly - e->menu->node.y;
        expo_menu_hover(e->menu, (mx >= 0 && mx < mw && my >= 0 && my < mh)
                                     ? expo_menu_hit(mx, my) : EXPO_MENU_ROW_NONE);
        return true;
    }

    if (e->drag) {
        expo_drag_to(e, lx, ly);
    } else {
        e->hover = expo_item_at(e, lx, ly);
    }
    expo_layout(e);
    return true;
}

/* Put a dragged window where its card was dropped. */
static void expo_drop(FwmServer *server, ExpoItem *it) {
    FwmView *view = it->view;
    int d = it->desktop;

    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int src = b ? b->desktop_id : view->x / server->screen_width;
    if (d != src) server_move_view_to_desktop(server, view, d, 0);

    /* A tiled desktop owns its geometry — server_move_view_to_desktop has
     * already inserted the window into the layout, and placing it by hand here
     * would only fight the tiler on the next tick. */
    if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
        /* Snap the card back onto whatever the layout decided. */
        it->wx = view->x;
        it->wy = view->y;
        return;
    }

    double max_y = server->screen_height - it->h;
    if (it->wy < 0) it->wy = 0;
    if (it->wy > max_y) it->wy = max_y > 0 ? max_y : 0;
    double min_x = (double)d * server->screen_width;
    double max_x = min_x + server->screen_width - it->w;
    if (it->wx < min_x) it->wx = min_x;
    if (it->wx > max_x) it->wx = max_x > min_x ? max_x : min_x;

    view->x = (int)lround(it->wx);
    view->y = (int)lround(it->wy);
    view->tile_anim = 0;
    if (b) {
        b->x = it->wx;
        b->y = it->wy;
        b->vx = 0; b->vy = 0; b->flying = 0;
    }
    view_sync_position(view);
}

bool expo_handle_button(FwmServer *server, uint32_t button, bool pressed,
                        double lx, double ly) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->leaving) return true;

    /* A menu on screen owns the next click wherever it lands: on a row it acts,
     * anywhere else it dismisses — and dismissing does NOT also do whatever the
     * click would have meant, which is the rule every other menu here follows. */
    if (e->menu) {
        if (!pressed) return true;
        int mw, mh;
        expo_menu_size(&mw, &mh);
        double mx = lx - e->menu->node.x, my = ly - e->menu->node.y;
        int row = (mx >= 0 && mx < mw && my >= 0 && my < mh)
                      ? expo_menu_hit(mx, my) : EXPO_MENU_ROW_NONE;
        ExpoItem *target = e->menu_item;
        expo_menu_close(e);
        if (row == EXPO_MENU_ROW_GOTO && target) {
            server_focus_view(server, target->view);
            server->focus_desktop = target->desktop;
            expo_close(server, target->desktop);
        } else if (row == EXPO_MENU_ROW_CLOSE && target) {
            /* Ask, do not force: the card stays until the client actually
             * unmaps, and expo_forget_view is what takes it off the strip. */
            view_send_close(target->view);
        }
        return true;
    }

    if (button == BTN_MIDDLE) {
        /* Fly round the ring: sideways turns it, up and down lifts the camera
         * over it. The middle button because the left one carries windows and
         * the right one opens their menu — and because a gesture nobody knows
         * about is better off on a button nobody presses by accident. */
        if (!pressed) { e->orbiting = 0; return true; }
        if (!expo_can_orbit(e)) return true;
        e->orbiting = 1;
        e->orbit_x = lx;
        e->orbit_y = ly;
        return true;
    }

    if (button == BTN_RIGHT) {
        if (pressed) {
            ExpoItem *it = expo_item_at(e, lx, ly);
            if (it) {
                e->menu = expo_menu_show(server->layer_overlay,
                                         server->screen_width, server->screen_height,
                                         lx, ly, view_title(it->view));
                e->menu_item = e->menu ? it : NULL;
                e->hover = it;
                expo_layout(e);
            }
        }
        return true;
    }

    if (button != BTN_LEFT) return true;

    if (!pressed) {
        if (e->drag) {
            expo_drop(server, e->drag);
            e->drag = NULL;
            expo_layout(e);
        }
        return true;
    }

    ExpoItem *it = expo_item_at(e, lx, ly);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
    if (it && (mods & WLR_MODIFIER_LOGO)) {
        /* Super+drag moves the window, including across desktops — the reason
         * the strip is a window manager and not a screenshot. */
        int d;
        double wx, wy;
        expo_point(e, lx, ly, &d, &wx, &wy);
        e->drag = it;
        e->drag_off_x = wx - it->wx;
        e->drag_off_y = wy - it->wy;
        e->hover = it;
        /* Only the flat path has a node to raise. On the curved one there is
         * no scene node for a window at all — the strip is drawn by hand, and
         * expo_draw_gl already paints the dragged card last. Raising the NULL
         * was a segfault the moment a window was picked up in 3D, and one that
         * left nothing in the log to find it by. */
        if (it->node) wlr_scene_node_raise_to_top(&it->node->node);
        return true;
    }

    if (it) {
        server_focus_view(server, it->view);
        server->focus_desktop = it->desktop;
        expo_close(server, it->desktop);
        return true;
    }

    /* Empty space: go to the desktop that was clicked. */
    int d;
    double wx, wy;
    expo_point(e, lx, ly, &d, &wx, &wy);
    expo_close(server, d);
    return true;
}

bool expo_handle_axis(FwmServer *server, double delta) {
    FwmExpo *e = server->expo;
    if (!e) return false;
    if (e->orbiting) {
        /* Mid-flight the wheel is the one thing left to ask for: closer or
         * further. Panning as well would fight the hand already turning it. */
        e->dist_target *= delta > 0.0 ? EXPO_DIST_STEP : 1.0 / EXPO_DIST_STEP;
        if (e->dist_target < EXPO_DIST_MIN) e->dist_target = EXPO_DIST_MIN;
        if (e->dist_target > EXPO_DIST_MAX) e->dist_target = EXPO_DIST_MAX;
        return true;
    }
    /* A wheel notch is ~15 units, so a notch is a third of a desktop —
     * enough to feel like scrolling a strip, small enough to aim with. */
    if (!e->leaving) expo_pan_by(e, delta * expo_pitch(e) / 45.0);
    return true;
}
