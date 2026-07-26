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

#include "view.h"
#include "rotate.h"
#include "theme.h"
#include "server.h"
#include "physics.h"
#include "bsp.h"
#include "group.h"
#include "session.h"
#include "foreign.h"
#include "ipc.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>
#include <drm_fourcc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void handle_map(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, map);
    view_map(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, unmap);
    view_unmap(view);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, commit);
    // The compositor must reply to the xdg_surface's initial commit with a
    // configure before the client is allowed to map its surface. Let the
    // client pick its own initial size. (X11 windows size themselves.)
    if (view->type == FWM_VIEW_XDG && view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }
    // The mapping commit lands here immediately after view_map; the one after
    // it is the first with content the client actually drew, which is when the
    // fade may start.
    if (view->open_hold > 0) view->open_hold--;

    // Track the actual committed surface size so borders hug the real window.
    view_update_border_geometry(view);

    /* A tiled client that commits a size other than the one it was given
     * shifts where its neighbours belong — terminals do this on every resize,
     * rounding to whole character cells. Re-run the layout's positioning pass,
     * but only when the size actually moved: this runs on every commit. */
    {
        int cw, ch;
        view_committed_size(view, &cw, &ch);
        if (cw != view->aligned_w || ch != view->aligned_h) {
            view->aligned_w = cw;
            view->aligned_h = ch;
            PhysicsBody *pb = physics_find_body(&view->server->physics, view->id);
            if (pb && pb->tiled) server_align_tiles(view->server, pb->desktop_id);
        }

        /* Adopt the size the client actually took. A resize ASKS for a size;
         * what the window ends up being is the client's answer, and terminals
         * answer with the next size down that is a whole number of character
         * cells. Until this, the compositor kept believing the number it asked
         * for: the physics box stayed up to a cell taller and wider than the
         * window drawn inside it, and since clients paint from their top-left
         * corner, all of that slack showed as a gap along the bottom and right
         * — a resized window came to rest visibly above the floor, while the
         * top and left, where box and window still met, looked correct.
         *
         * Only our bookkeeping changes; the client is not configured back at
         * its own size, which is how this stays a single exchange and not a
         * loop. */
        if (cw > 0 && ch > 0 && (cw != view->width || ch != view->height)) {
            view->width = cw;
            view->height = ch;
            physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                              cw, ch, view->server->screen_width);
        }
    }

    /* New content. An effect standing in front of the window reads this to
     * decide whether its picture is stale (view_spin_tick, view_jelly_tick). */
    view->content_dirty = 1;

    // Keep our own lock on the latest committed buffer: at unmap time the
    // client's buffer may already be gone, but the close animation needs the
    // last frame as a snapshot.
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->buffer) {
        wlr_buffer_lock(&surface->buffer->base);
        if (view->last_buffer) wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = &surface->buffer->base;
    }
}

static void view_place_borders(FwmView *view, int x, int y, int w, int h);
static void view_border_box(FwmView *view, int *w, int *h);

/* ── impact squash & stretch ──────────────────────────────────────────── */

/* Tuned for a single soft press rather than a jelly wobble (the user asked for
 * "поспокойнее"). At omega 26 the window crossed its resting size 2-3 times
 * with a -3.7% rebound, which reads as vibration; at 14 it compresses, returns
 * once and is done, rebound about -1%. Keep omega well under the decay's reach
 * or the wobble comes back. */
#define SQUASH_DECAY  12.0   /* 1/s */
#define SQUASH_OMEGA  14.0   /* rad/s — one compression, then rest */
#define SQUASH_BULGE  0.45   /* how much the perpendicular axis bulges */
#define SQUASH_MAX_S  0.45   /* hard cap on deformation, both directions */

/* ── drag wobble ──────────────────────────────────────────────────────────
 *
 * The lattice itself is wobble.c; these are the numbers that turn it into
 * pixels. See the jelly_* fields for how it is drawn. */

/* Slack around the window in the warp target, as a fraction of its larger
 * side, and the bounds on it. A wobbling sheet overshoots its own box, and
 * whatever leaves the buffer is clipped by a straight invisible edge — so this
 * wants to be comfortably past the ~80px an ordinary drag reaches, without
 * paying for a shake nobody performs. */
#define JELLY_MARGIN_FRAC  0.22
#define JELLY_MARGIN_MIN   48
#define JELLY_MARGIN_MAX   192

/* ── composited snapshot of a window ──────────────────────────────────────
 *
 * Deforming `view->last_buffer` — the TOPLEVEL surface's buffer — is wrong for
 * any client that paints through subsurfaces: their content lives in a
 * different buffer entirely and is simply absent from the snapshot, while the
 * toplevel's own ARGB alpha gets blended over the hole. That is why Firefox
 * turned see-through during an impact and kitty (no subsurfaces) never did.
 *
 * So render the window's whole scene subtree into a buffer of our own, exactly
 * as the compositor would draw it on screen, and deform THAT. All public
 * wlroots API — no raw GLES, no scene-graph internals.
 *
 * (wlr_scene_node_snapshot does not exist in 0.20; if a future wlroots grows
 * one, it replaces this wholesale.) */

struct snapshot_ctx {
    struct wlr_render_pass *pass;
    struct wlr_renderer *renderer;
    int origin_x, origin_y;      /* subtree's top-left in layout coords */
};

static void snapshot_add_buffer(struct wlr_scene_buffer *scene_buffer,
                                int sx, int sy, void *data) {
    struct snapshot_ctx *ctx = data;
    if (!scene_buffer->buffer) return;

    /* A buffer that belongs to a client surface already HAS a texture: wlroots
     * imported it when the client committed, and the scene draws the window
     * from it sixty times a second. Importing the same dmabuf again for our own
     * pass — and throwing the import away at the end of it — was costing about
     * as much as the pass itself, which at one pass every 150ms is a 5ms frame
     * every 150ms: a hitch you can see, and the whole reason a slow spin
     * juddered. Borrow the cached one and only import what is genuinely ours
     * (an effect's own buffer, a ghost). */
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(scene_buffer);
    struct wlr_texture *cached = ss ? wlr_surface_get_texture(ss->surface) : NULL;
    struct wlr_texture *tex = cached
        ? cached : wlr_texture_from_buffer(ctx->renderer, scene_buffer->buffer);
    if (!tex) return;

    /* dest_size 0 means "use the buffer size", the same rule the scene follows. */
    int w = scene_buffer->dst_width  ? scene_buffer->dst_width  : (int)tex->width;
    int h = scene_buffer->dst_height ? scene_buffer->dst_height : (int)tex->height;

    wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
        .texture = tex,
        .dst_box = { .x = sx - ctx->origin_x, .y = sy - ctx->origin_y,
                     .width = w, .height = h },
        .alpha = &scene_buffer->opacity,
        .transform = scene_buffer->transform,
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    });
    if (!cached) wlr_texture_destroy(tex);   /* only ever destroy our own import */
}

/* An empty ARGB8888 buffer the renderer can draw into. */
static struct wlr_buffer *view_alloc_buffer(FwmServer *server, int w, int h) {
    if (!server->wlr_allocator || w <= 0 || h <= 0) return NULL;
    struct wlr_buffer *buf = NULL;
    struct wlr_drm_format_set fmts = {0};
    if (wlr_drm_format_set_add(&fmts, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID)) {
        const struct wlr_drm_format *fmt = wlr_drm_format_set_get(&fmts, DRM_FORMAT_ARGB8888);
        if (fmt) buf = wlr_allocator_create_buffer(server->wlr_allocator, w, h, fmt);
    }
    wlr_drm_format_set_finish(&fmts);
    return buf;
}

/* Paint the window's subtree into `buf`, which must be the window's size.
 * Split out of view_snapshot_content so the spin can refresh a snapshot it
 * already owns instead of allocating a new buffer several times a second. */
static bool view_snapshot_into(FwmView *view, struct wlr_buffer *buf) {
    FwmServer *server = view->server;
    if (!server->wlr_renderer || !view->scene_tree || !buf) return false;

    int w = buf->width, h = buf->height;

    /* The borders are our own nodes and must not be baked in — view_place_borders
     * redraws them around the deformed box on every tick. */
    bool border_was_enabled[4] = {false};
    for (int i = 0; i < 4; i++) {
        if (view->border[i]) {
            border_was_enabled[i] = view->border[i]->node.enabled;
            wlr_scene_node_set_enabled(&view->border[i]->node, false);
        }
    }

    bool ok = false;
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->wlr_renderer, buf, NULL);
    if (!pass) goto restore;

    /* Start from transparent: a window whose content does not cover the whole
     * box must not pick up whatever the allocator handed us. */
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .x = 0, .y = 0, .width = w, .height = h },
        .color = { .r = 0, .g = 0, .b = 0, .a = 0 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    int lx = 0, ly = 0;
    wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
    struct snapshot_ctx ctx = {
        .pass = pass, .renderer = server->wlr_renderer,
        .origin_x = lx, .origin_y = ly,
    };
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, snapshot_add_buffer, &ctx);

    ok = wlr_render_pass_submit(pass);

restore:
    for (int i = 0; i < 4; i++) {
        if (view->border[i])
            wlr_scene_node_set_enabled(&view->border[i]->node, border_was_enabled[i]);
    }
    return ok;
}

/* Returns a buffer holding the window as currently composited, or NULL. The
 * caller owns the reference that wlr_allocator_create_buffer hands back. */
static struct wlr_buffer *view_snapshot_content(FwmView *view) {
    if (!view->scene_tree) return NULL;
    struct wlr_buffer *buf = view_alloc_buffer(view->server, view->width, view->height);
    if (!buf) return NULL;
    if (!view_snapshot_into(view, buf)) {
        wlr_buffer_drop(buf);
        return NULL;
    }
    return buf;
}

/* How often a COMPOSITED picture is retaken, in seconds.
 *
 * With effects.live on, whenever the client has drawn something new — capped,
 * because the pass is cheap but not free (~0.5ms for a large window here, once
 * the textures stopped being re-imported; it was ten times that before, which
 * is what a slow spin juddered on). With it off, the old fixed cadence and no
 * frame callbacks, so the client sleeps and the picture is a still frame. */
#define SNAP_LIVE_S    (1.0 / 30.0)
#define SNAP_REFRESH_S 0.15

/* ── live content behind an effect ────────────────────────────────────────
 *
 * An effect that shows a picture of the window has two ways to go stale, and
 * the difference in what they COST is what decides how far to chase each.
 *
 * The first is the picture. Most windows are ONE surface — a terminal, a video
 * player, anything without subsurfaces or an open menu — and for those the
 * composited snapshot is a copy of a texture the client already handed us.
 * view_live_texture returns that texture instead, so the spin and the wobble
 * draw live content every frame at no cost at all: no allocation, no
 * flatten-the-subtree pass, nothing to keep up to date. That is the whole win,
 * and it is free.
 *
 * The second is the client, and this one is NOT free. Hiding the live nodes
 * takes the window out of the scene's frame-done sweep, so a client waiting on
 * a frame callback stops drawing; view_send_frame_done keeps the callbacks
 * coming. But a client redrawing 60 times a second while it is hidden costs
 * real work on the machine, and on modest graphics that showed up exactly where
 * you would expect — as a spin that juddered under a slow hand, where there is
 * no motion to hide a dropped frame behind.
 *
 * So liveness is spent only where it is cheap. A single-surface window gets
 * both halves: live texture, live client. A window that must be composited gets
 * neither — no frame callbacks, and its picture retaken on the old
 * SNAP_REFRESH_S timer — because there the second half buys a full render pass
 * per client frame, which is precisely the cost that was juddering. Those
 * windows behave exactly as they always have.
 *
 * effects.live = 0 turns the whole thing off and puts every window on the old
 * still-frame path, for hardware where even the free half is not free. */

/* Is this window ONE surface, so that its texture is the whole picture?
 *
 * Kept apart from fetching the texture, because the two say different things
 * and confusing them costs a visible hitch: a window that has grown a
 * subsurface must move to the composited path for good, while a window that
 * merely has no texture this instant (a client between buffers) should keep
 * showing the frame it already has and try again in 16ms. Tearing the effect
 * down and rebuilding it for the latter is several allocations and a full
 * snapshot pass, in the middle of an animation. */
static bool view_is_single_surface(FwmView *view) {
    if (view->server->config.effects.live <= 0.0) return false;
    struct wlr_surface *s = view_surface(view);
    if (!s) return false;
    /* Anything drawn beside the toplevel's own buffer has to be composited, or
     * it is simply missing from the picture — the very bug the snapshot pass
     * was written for (see above). */
    if (!wl_list_empty(&s->current.subsurfaces_above) ||
        !wl_list_empty(&s->current.subsurfaces_below)) return false;
    if (view->type == FWM_VIEW_XDG && view->xdg_toplevel &&
        !wl_list_empty(&view->xdg_toplevel->base->popups)) return false;
    return true;
}

static struct wlr_texture *view_live_texture(FwmView *view) {
    if (!view_is_single_surface(view)) return NULL;
    return wlr_surface_get_texture(view_surface(view));
}

/* Is the composited picture stale enough to be worth retaking? */
static bool view_snapshot_due(FwmView *view, double since) {
    if (view->server->config.effects.live > 0.0)
        return view->content_dirty && since >= SNAP_LIVE_S;
    return since >= SNAP_REFRESH_S;
}

/* Say which path an effect took, once, when it starts (FWM_DEBUG_EFFECTS).
 * Which one a given window lands on is not obvious from the outside — plenty
 * of ordinary clients draw through a subsurface without ever showing one — and
 * that is exactly what has to be known before any judder can be argued about. */
static void view_log_effect_path(FwmView *view, const char *what, bool live) {
    if (!view->server->fx_debug) return;
    struct wlr_surface *s = view_surface(view);
    const char *why = "";
    if (!live && s) {
        if (!wl_list_empty(&s->current.subsurfaces_above) ||
            !wl_list_empty(&s->current.subsurfaces_below)) why = " (subsurfaces)";
        else if (view->type == FWM_VIEW_XDG && view->xdg_toplevel &&
                 !wl_list_empty(&view->xdg_toplevel->base->popups)) why = " (open popup)";
        else if (view->server->config.effects.live <= 0.0) why = " (effects.live = 0)";
        else why = " (no texture yet)";
    }
    wlr_log(WLR_INFO, "%s on \"%s\": %s path%s", what,
            view_title(view) ? view_title(view) : "?",
            live ? "live" : "composited", why);
}

static void frame_done_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
    (void)sx; (void)sy;
    wlr_surface_send_frame_done(surface, data);
}

static void view_send_frame_done(FwmView *view) {
    struct wlr_surface *s = view_surface(view);
    if (!s) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_for_each_surface(s, frame_done_iter, &now);
}

/* Show or hide the live content while keeping the borders and whichever
 * snapshot is standing in for the window visible.
 *
 * EVERY node of our own has to be listed here. This is the call that hides the
 * window behind an effect, and each effect makes it immediately after creating
 * the node that replaces it — so a node missing from this list is switched off
 * by the very call that was supposed to reveal it, and the window simply
 * vanishes for as long as the effect runs. */
static void view_set_content_enabled(FwmView *view, bool enabled) {
    if (!view->scene_tree) return;
    struct wlr_scene_node *node;
    wl_list_for_each(node, &view->scene_tree->children, link) {
        bool ours = false;
        for (int i = 0; i < 4; i++) {
            if (view->border[i] && node == &view->border[i]->node) ours = true;
        }
        if (view->squash_buf && node == &view->squash_buf->node) ours = true;
        if (view->spin_buf && node == &view->spin_buf->node) ours = true;
        if (view->jelly_buf && node == &view->jelly_buf->node) ours = true;
        if (!ours) wlr_scene_node_set_enabled(node, enabled);
    }
}

void view_stop_squash(FwmView *view) {
    if (!view->squash_buf) return;
    wlr_scene_node_destroy(&view->squash_buf->node);
    view->squash_buf = NULL;
    if (view->squash_lock) {
        wlr_buffer_unlock(view->squash_lock);
        view->squash_lock = NULL;
    }
    view->squash_t = 0.0;
    view->squash_amount = 0.0;
    view_set_content_enabled(view, true);
    view_update_border_geometry(view); /* back to the real box */
}

/* Put the deformable snapshot in place and hide the live content behind it.
 * False if there is nothing to snapshot. */
static bool view_take_deform_snapshot(FwmView *view) {
    /* A composite of the whole subtree, not the toplevel's raw buffer: see
     * view_snapshot_content. We hold the reference the allocator gave us until
     * the scene node has taken its own lock. */
    struct wlr_buffer *snap = view_snapshot_content(view);
    if (!snap) return false;

    view->squash_buf = wlr_scene_buffer_create(view->scene_tree, snap);
    if (!view->squash_buf) {
        wlr_buffer_drop(snap);
        return false;
    }
    view->squash_lock = wlr_buffer_lock(snap);
    wlr_buffer_drop(snap);
    /* Under the borders, so the frame still reads as the window's outline. */
    wlr_scene_node_lower_to_bottom(&view->squash_buf->node);
    view_set_content_enabled(view, false);
    return true;
}

void view_start_squash(FwmView *view, double nx, double ny, double amount) {
    if (!view->scene_tree || !view->last_buffer) return;
    if (amount <= 0.001) return;
    /* A spinning window already has the picture, and a deformation along a
     * screen-axis normal would be visibly wrong on a tilted one. */
    if (view->spin_buf) return;

    /* A window still in your hand keeps its wobble: the drag owns the picture,
     * and the impact is not lost anyway — a collision moves the window, and
     * moving the window is exactly what the sheet responds to.
     *
     * One that has been let go is a different matter. Its wobble is ringing
     * out, it is not being driven by anything any more, and landing is the
     * loudest thing that will happen to it — so the landing takes the picture
     * over. Without this the wobble swallowed every impact for as long as it
     * lasted: windows arrived at the floor soft and squashed nothing. */
    if (view->jelly) {
        if (!view->jelly_settling) return;
        view_jelly_stop(view);
    }

    if (view->squash_buf) {
        /* Already deforming: retarget rather than stacking a second snapshot,
         * and keep whichever impact was stronger. */
        if (amount > view->squash_amount) {
            view->squash_amount = amount;
            view->squash_nx = nx;
            view->squash_ny = ny;
            view->squash_t = 0.0;
        }
        return;
    }

    if (!view_take_deform_snapshot(view)) return;

    view->squash_t = 0.0;
    view->squash_amount = amount;
    view->squash_nx = nx;
    view->squash_ny = ny;
    wlr_log(WLR_DEBUG, "squash: view %u amount %.3f normal (%.2f,%.2f)",
            view->id, amount, nx, ny);
}

void view_squash_tick(FwmView *view, double dt) {
    if (!view->squash_buf) return;
    view->squash_t += dt;

    /* Damped oscillation: a hard squash that springs back through a smaller
     * overshoot, rather than a single linear dent.
     * The end test MUST look at the envelope, not at `a`: the cosine crosses
     * zero on every half-wobble, so testing `a` ended the animation ~60ms in,
     * at the exact instant of zero deformation — the spring-back never ran. */
    double env = view->squash_amount * exp(-SQUASH_DECAY * view->squash_t);
    if (env < 0.004) { view_stop_squash(view); return; }
    double a = env * cos(SQUASH_OMEGA * view->squash_t);
    if (a >  SQUASH_MAX_S) a =  SQUASH_MAX_S;
    if (a < -SQUASH_MAX_S) a = -SQUASH_MAX_S;

    int w, h;
    view_border_box(view, &w, &h);
    if (w <= 0 || h <= 0) { view_stop_squash(view); return; }

    /* Compress along the contact normal, bulge across it. */
    double ax = fabs(view->squash_nx), ay = fabs(view->squash_ny);
    double sx = 1.0 - a * ax + a * SQUASH_BULGE * ay;
    double sy = 1.0 - a * ay + a * SQUASH_BULGE * ax;

    int dw = (int)lround(w * sx), dh = (int)lround(h * sy);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    /* Keep the edge that took the hit planted: a window landing on the floor
     * must compress into the floor, not hover above it. */
    int ox = view->squash_nx > 0 ? w - dw : 0;
    int oy = view->squash_ny > 0 ? h - dh : 0;

    wlr_scene_buffer_set_dest_size(view->squash_buf, dw, dh);
    wlr_scene_node_set_position(&view->squash_buf->node, ox, oy);
    view_place_borders(view, ox, oy, dw, dh);
}

/* ── drag wobble ──────────────────────────────────────────────────────── */

/* The warp target is the window plus slack on every side; the mesh is drawn
 * into it and the node sits at -margin so the window's own box still lines up
 * where it always did. */
static int jelly_margin_for(int w, int h) {
    int big = w > h ? w : h;
    int m = (int)lround(JELLY_MARGIN_FRAC * big);
    if (m < JELLY_MARGIN_MIN) m = JELLY_MARGIN_MIN;
    if (m > JELLY_MARGIN_MAX) m = JELLY_MARGIN_MAX;
    return m;
}

/* The warped picture covers the window plus its margin, so left to itself it
 * would swallow clicks in a transparent frame all around — including a good
 * part of whatever is next to it. The lattice's own outline is the honest
 * answer but a point-in-polygon test against 64 quads per motion event is not;
 * the window's box, offset by how far the sheet has actually moved, is within
 * a few px of it and is what the cursor is aiming at anyway. */
static bool jelly_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    FwmView *view = buffer->node.data;
    if (!view || !view->jelly_buf) return true;
    double lx = *sx - view->jelly_margin, ly = *sy - view->jelly_margin;
    double dx = view->jelly_wob.px[0], dy = view->jelly_wob.py[0];   /* top-left point */
    return lx >= dx && ly >= dy &&
           lx <= dx + view->jelly_w && ly <= dy + view->jelly_h;
}

/* Tear the machinery down without touching what it hid — view_jelly_stop does
 * that part, and a mid-wobble resize deliberately does not. */
static void view_jelly_free(FwmView *view) {
    if (view->jelly_buf) {
        wlr_scene_node_destroy(&view->jelly_buf->node);
        view->jelly_buf = NULL;
    }
    if (view->jelly_tex) {
        wlr_texture_destroy(view->jelly_tex);
        view->jelly_tex = NULL;
    }
    if (view->jelly_src) {
        wlr_buffer_unlock(view->jelly_src);
        view->jelly_src = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (view->jelly_dst[i]) {
            wlr_buffer_unlock(view->jelly_dst[i]);
            view->jelly_dst[i] = NULL;
        }
    }
    view->jelly_w = view->jelly_h = view->jelly_margin = 0;
    view->jelly_flip = 0;
    view->jelly_snap_t = 0.0;
    view->jelly_live = 0;
}

void view_jelly_stop(FwmView *view) {
    if (!view->jelly) return;
    int border = view->jelly_border;
    view->jelly = 0;
    view->jelly_settling = 0;
    view_jelly_free(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    view_update_border_geometry(view);
}

/* Build (or rebuild) the snapshot, the two warp targets and the scene node.
 * Called when the wobble starts and again whenever the window changes size. */
static bool view_jelly_setup(FwmView *view) {
    FwmServer *server = view->server;
    if (!view->scene_tree || !server->wlr_renderer) return false;

    int w = view->width, h = view->height;
    if (w <= 0 || h <= 0) return false;

    bool restarting = view->jelly_buf != NULL;
    int border = restarting ? view->jelly_border
                            : (view->border[0] && view->border[0]->node.enabled);
    view_jelly_free(view);

    /* The live content has to be visible to be photographed, so the snapshot
     * comes first and the window is hidden only once it succeeded — a failed
     * setup must leave the window on screen, not blank. */
    if (restarting) view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);

    int margin = jelly_margin_for(w, h);

    /* One surface and nothing else: bend the client's own texture, so the
     * window stays live while it is dragged. The wobble only ever runs on the
     * GLES2 path (view_jelly_begin refuses otherwise), so unlike the spin there
     * is no buffer-only fallback to keep a snapshot around for. */
    view->jelly_live = view_live_texture(view) != NULL;
    view_log_effect_path(view, "wobble", view->jelly_live);

    if (!view->jelly_live) {
        struct wlr_buffer *src = view_snapshot_content(view);
        if (!src) goto fail;
        view->jelly_src = wlr_buffer_lock(src);
        wlr_buffer_drop(src);

        view->jelly_tex = wlr_texture_from_buffer(server->wlr_renderer, view->jelly_src);
        if (!view->jelly_tex) goto fail;
    }

    /* Two targets used alternately: the scene may still be reading last
     * frame's while this one is drawn, and overwriting it in place would tear. */
    for (int i = 0; i < 2; i++) {
        struct wlr_buffer *d = view_alloc_buffer(server, w + 2 * margin, h + 2 * margin);
        if (!d) goto fail;
        view->jelly_dst[i] = wlr_buffer_lock(d);
        wlr_buffer_drop(d);
    }

    view->jelly_buf = wlr_scene_buffer_create(view->scene_tree, NULL);
    if (!view->jelly_buf) goto fail;
    wlr_scene_node_lower_to_bottom(&view->jelly_buf->node);
    /* view_at() walks up from the node's PARENT to find the view, so this is
     * free for the hit test to use. */
    view->jelly_buf->node.data = view;
    view->jelly_buf->point_accepts_input = jelly_accepts_input;
    wlr_scene_node_set_position(&view->jelly_buf->node, -margin, -margin);

    view->jelly_w = w;
    view->jelly_h = h;
    view->jelly_margin = margin;
    view->jelly_border = border;

    /* The borders are scene rectangles: they cannot bend any more than they
     * can tilt, so they go away for the duration rather than framing a window
     * that is no longer where they say it is. */
    view_set_content_enabled(view, false);
    view_set_border_enabled(view, 0);
    return true;

fail:
    view_jelly_free(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    return false;
}

/* Draw the lattice as it stands. Returns false if the mesh could not be drawn
 * at all, which is the caller's cue to give the live window back. */
static bool view_jelly_draw(FwmView *view, double strength) {
    float pts[WOBBLE_POINTS * 2];
    wobble_points(&view->jelly_wob, pts, view->jelly_margin, view->jelly_margin);

    if (strength != 1.0) {
        /* Move every point toward (or past) where it would rest. Scaling the
         * SHAPE rather than the springs is what keeps the wobble's timing the
         * same at every setting — only its size changes. */
        for (int j = 0; j < WOBBLE_GRID; j++) {
            for (int i = 0; i < WOBBLE_GRID; i++) {
                int k = j * WOBBLE_GRID + i;
                float hx = (float)(view->jelly_margin + (double)view->jelly_w * i / (WOBBLE_GRID - 1));
                float hy = (float)(view->jelly_margin + (double)view->jelly_h * j / (WOBBLE_GRID - 1));
                pts[k * 2 + 0] = hx + (pts[k * 2 + 0] - hx) * (float)strength;
                pts[k * 2 + 1] = hy + (pts[k * 2 + 1] - hy) * (float)strength;
            }
        }
    }

    /* Live path: whatever the client has on screen this instant. It may be a
     * different texture than last frame — that is the whole point — and NULL
     * only if the window stopped being a single surface, which the tick
     * notices and rebuilds for. */
    struct wlr_texture *tex = view->jelly_live ? view_live_texture(view) : view->jelly_tex;
    /* No texture this instant (a client between buffers). Keep the frame that
     * is already on screen: the next tick is 16ms away and tearing the whole
     * wobble down mid-drag would be a far bigger jump than one repeated frame. */
    if (!tex) return true;

    struct wlr_buffer *dst = view->jelly_dst[view->jelly_flip];
    if (!warp_blit(view->server->wlr_renderer, dst, tex, WOBBLE_GRID, pts)) {
        return false;
    }
    view->jelly_flip ^= 1;
    wlr_scene_buffer_set_buffer(view->jelly_buf, dst);
    /* Stated rather than left to default off the buffer, exactly as the spin
     * states it: the node was created empty, so it starts with no size of its
     * own at all. */
    wlr_scene_buffer_set_dest_size(view->jelly_buf, dst->width, dst->height);
    return true;
}

void view_jelly_begin(FwmView *view, double strength, double grab_lx, double grab_ly) {
    if (!view->scene_tree || !view->last_buffer) return;
    if (strength <= 0.0) return;
    /* Rotation wins, exactly as it does over the impact squash: it owns the
     * picture, and a sheet bending along the screen axes says nothing useful
     * about one that is tilted. */
    if (view->spin_buf) return;
    /* No mesh without the GLES2 path. Unlike the spin there is no degraded
     * version worth showing — a window that does not wobble is just a window. */
    if (!rotate_supported(view->server->wlr_renderer)) return;

    if (view->jelly) {
        view->jelly_settling = 0;
        wobble_grab(&view->jelly_wob, grab_lx, grab_ly);
        return;
    }
    /* A dent from a landing a moment ago is replaced by the wobble rather than
     * fought with — same picture, and the drag is the newer intent. */
    if (view->squash_buf) view_stop_squash(view);
    if (!view_jelly_setup(view)) return;

    view->jelly = 1;
    view->jelly_settling = 0;
    view->jelly_px = view->x;
    view->jelly_py = view->y;
    wobble_reset(&view->jelly_wob, view->jelly_w, view->jelly_h);
    wobble_grab(&view->jelly_wob, grab_lx, grab_ly);

    /* The setup above hid the live window behind a scene node with no picture
     * in it yet, and the first tick is a frame away. Draw the undeformed sheet
     * now — it is a plain copy of the window — or the press would blink it out
     * of existence for that frame. */
    if (!view_jelly_draw(view, strength)) view_jelly_stop(view);
}

void view_jelly_release(FwmView *view) {
    if (!view->jelly) return;
    view->jelly_settling = 1;
    wobble_release(&view->jelly_wob);
}

/* Retake the frozen picture. The live nodes come back for the length of the
 * pass and the warped one goes away, or each refresh would bake the last bent
 * frame into the next. Nothing is presented in between, so the window never
 * flashes back to its undeformed self. */
static void view_jelly_resnap(FwmView *view) {
    view_set_content_enabled(view, true);
    wlr_scene_node_set_enabled(&view->jelly_buf->node, false);
    view_snapshot_into(view, view->jelly_src);
    wlr_scene_node_set_enabled(&view->jelly_buf->node, true);
    view_set_content_enabled(view, false);
}

void view_jelly_tick(FwmView *view, double strength, double dt) {
    if (!view->jelly) return;
    if (view->spin_buf || strength <= 0.0) { view_jelly_stop(view); return; }
    if (dt <= 0.0) return;

    if (!view->jelly_buf || view->jelly_w != view->width || view->jelly_h != view->height) {
        if (!view_jelly_setup(view)) { view_jelly_stop(view); return; }
        wobble_resize(&view->jelly_wob, view->jelly_w, view->jelly_h);
    }

    /* For as long as the picture is on screen, whether the hand is still on the
     * window or the wobble is ringing out. Skipping the ring-out was a mistake
     * you could watch: a released window is thrown, and a thrown window keeps
     * moving, so the sheet kept being driven and the effect outlasted the whole
     * flight with a still frame in it. */
    view->jelly_snap_t += dt;
    /* The client is hidden behind the sheet and gets no frame callbacks from
     * the scene; without these it would stop drawing and "live" would mean a
     * live view of a frozen window. */
    if (view->server->config.effects.live > 0.0) view_send_frame_done(view);

    if (view->jelly_live) {

        /* Nothing to retake — view_jelly_draw reads the client's texture as it
         * goes. Only a window that has STOPPED being a single surface (a menu
         * opened under the hand) needs rebuilding onto the snapshot path; a
         * missing texture for one frame is not that, and view_jelly_draw simply
         * keeps the frame already on screen. */
        if (!view_is_single_surface(view)) {
            if (!view_jelly_setup(view)) { view_jelly_stop(view); return; }
            wobble_resize(&view->jelly_wob, view->jelly_w, view->jelly_h);
        }
        view->content_dirty = 0;
    } else if (view_snapshot_due(view, view->jelly_snap_t)) {
        view->content_dirty = 0;
        view->jelly_snap_t = 0.0;
        view->server->fx_snaps++;
        view_jelly_resnap(view);
    }

    /* The window's own movement is the only thing that ever drives the sheet:
     * the rest positions go where the window went, the points stay where they
     * were, and the springs take it from there.
     *
     * Which stops the moment the hand is off it. The lattice lives in the
     * window's own frame, so simply not translating IS the sheet riding along
     * with the flight — and it has to, because a window sailing at a steady
     * speed holds a steady lag, and a steady lag is never at rest. Left driven,
     * the wobble ran until the physics did, and everything below it (the impact
     * squash, the live window) waited that long too. */
    if (!view->jelly_settling) {
        wobble_translate(&view->jelly_wob, view->x - view->jelly_px, view->y - view->jelly_py);
    }
    view->jelly_px = view->x;
    view->jelly_py = view->y;

    /* `strength` scales how far the sheet is allowed to be from its rest shape,
     * not the springs themselves — the wobble keeps its timing at any setting
     * and only gets bigger or smaller. */
    wobble_step(&view->jelly_wob, dt);

    if (view->jelly_settling && wobble_at_rest(&view->jelly_wob)) {
        view_jelly_stop(view);
        return;
    }

    view->server->fx_moved++;
    if (!view_jelly_draw(view, strength)) view_jelly_stop(view);
}

/* ── free rotation ────────────────────────────────────────────────────── */

bool view_is_spinning(FwmView *view) {
    return view->spin_buf != NULL;
}

/* The rotated snapshot is a square as wide as the window's diagonal, so left
 * to itself it would swallow clicks in a fat transparent border around the
 * window — including, at 45 degrees, most of a neighbouring window's corner.
 * Rotating the point back and testing it against the upright rectangle gives
 * the cursor the tilted window it can actually see. */
static bool spin_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    FwmView *view = buffer->node.data;
    if (!view || !view->spin_buf) return true;

    double half = view->spin_size / 2.0;
    double lx = *sx - half, ly = *sy - half;   /* relative to the center */
    double c = cos(-view->spin_angle), s = sin(-view->spin_angle);
    double ux = c * lx - s * ly;
    double uy = s * lx + c * ly;
    return fabs(ux) <= view->spin_w / 2.0 && fabs(uy) <= view->spin_h / 2.0;
}

/* Tear down the machinery WITHOUT touching what it hid — view_stop_spin does
 * that part, and a mid-spin resize deliberately does not. */
static void view_spin_release(FwmView *view) {
    if (view->spin_buf) {
        wlr_scene_node_destroy(&view->spin_buf->node);
        view->spin_buf = NULL;
    }
    if (view->spin_tex) {
        wlr_texture_destroy(view->spin_tex);
        view->spin_tex = NULL;
    }
    if (view->spin_src) {
        wlr_buffer_unlock(view->spin_src);
        view->spin_src = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (view->spin_dst[i]) {
            wlr_buffer_unlock(view->spin_dst[i]);
            view->spin_dst[i] = NULL;
        }
    }
    view->spin_w = view->spin_h = view->spin_size = 0;
    view->spin_flip = 0;
    view->spin_snap_t = 0.0;
    view->spin_angle = 0.0;
    view->spin_live = 0;
    view->spin_seen = NULL;
}

void view_stop_spin(FwmView *view) {
    if (!view->spin_buf) return;
    int border = view->spin_border;
    view_spin_release(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    view_update_border_geometry(view);
}

/* Build (or rebuild) the snapshot, the two rotation targets and the scene node.
 * Called on the first tick and again whenever the window changes size. */
static bool view_spin_setup(FwmView *view) {
    FwmServer *server = view->server;
    if (!view->scene_tree || !server->wlr_renderer) return false;

    int w = view->width, h = view->height;
    if (w <= 0 || h <= 0) return false;

    bool restarting = view->spin_buf != NULL;
    int border = restarting ? view->spin_border
                            : (view->border[0] && view->border[0]->node.enabled);
    view_spin_release(view);

    /* The live content has to be visible to be photographed, so the snapshot
     * comes first and the window is hidden only once it succeeded — a failed
     * setup must leave the window on screen, not blank. */
    if (restarting) view_set_content_enabled(view, true);

    /* One surface and nothing else: rotate the client's own texture and skip
     * the snapshot machinery entirely. The window then turns LIVE — a video
     * keeps playing as it tumbles — and costs less than the frozen version did.
     * Decided once per spin, and re-decided if the window grows a subsurface or
     * opens a menu while it turns (view_spin_tick).
     *
     * Only where arbitrary rotation actually works: the quarter-turn fallback
     * below hands the SNAPSHOT BUFFER to the scene graph, and a client texture
     * is not a buffer we may hand over. On such a renderer the snapshot is the
     * only path, live or not. */
    view->spin_live = rotate_supported(server->wlr_renderer)
                   && view_live_texture(view) != NULL;
    view_log_effect_path(view, "spin", view->spin_live);

    if (!view->spin_live) {
        struct wlr_buffer *src = view_alloc_buffer(server, w, h);
        if (!src) return false;
        if (!view_snapshot_into(view, src)) {
            wlr_buffer_drop(src);
            return false;
        }
        view->spin_src = wlr_buffer_lock(src);
        wlr_buffer_drop(src);

        /* Imported once and kept: the rotation redraws from this texture every
         * frame, and re-importing a dmabuf 60 times a second is pure waste. The
         * refresh below renders into the same buffer, so the texture stays valid
         * across snapshots too. */
        view->spin_tex = wlr_texture_from_buffer(server->wlr_renderer, view->spin_src);
        if (!view->spin_tex) goto fail;
    }

    /* A square of the diagonal holds the window at every angle, so the target
     * never has to be reallocated as it turns. Two of them, used alternately:
     * the scene may still be reading last frame's buffer while this one is
     * drawn, and overwriting it in place would tear. */
    int size = (int)ceil(hypot(w, h)) + 2;
    for (int i = 0; i < 2; i++) {
        struct wlr_buffer *d = view_alloc_buffer(server, size, size);
        if (!d) goto fail;
        view->spin_dst[i] = wlr_buffer_lock(d);
        wlr_buffer_drop(d);
    }

    view->spin_buf = wlr_scene_buffer_create(view->scene_tree, NULL);
    if (!view->spin_buf) goto fail;
    wlr_scene_node_lower_to_bottom(&view->spin_buf->node);
    /* view_at() walks up from the node's PARENT to find the view, so this is
     * free for the hit test to use. */
    view->spin_buf->node.data = view;
    view->spin_buf->point_accepts_input = spin_accepts_input;

    view->spin_w = w;
    view->spin_h = h;
    view->spin_size = size;
    view->spin_border = border;

    /* Everything the scene draws upright goes away: the client's own surfaces,
     * and the border rects, which are scene rectangles and cannot be tilted at
     * all (they are not even in the snapshot — it only collects buffers). */
    view_set_content_enabled(view, false);
    view_set_border_enabled(view, 0);
    return true;

fail:
    view_spin_release(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    return false;
}

void view_spin_tick(FwmView *view, double angle, double dt) {
    /* The squash owns the same snapshot slot and deforms an upright window;
     * the two cannot both be showing. Rotation wins — it is the bigger, longer
     * lasting effect, and an impact that arrives mid-spin already shows itself
     * in the way the window tumbles. The drag wobble is a picture of the window
     * too, and gives way for the same reason. */
    if (view->squash_buf) view_stop_squash(view);
    view_jelly_stop(view);

    bool redraw = false;

    if (!view->spin_buf || view->spin_w != view->width || view->spin_h != view->height) {
        if (!view_spin_setup(view)) {
            view_stop_spin(view);
            return;
        }
        redraw = true;   /* the node has no picture in it yet */
    }

    struct wlr_texture *src = view->spin_tex;

    view->spin_snap_t += dt;
    /* Either path keeps the client drawing while it is hidden; what differs is
     * what showing its frames costs us (view_snapshot_due). */
    if (view->server->config.effects.live > 0.0) view_send_frame_done(view);

    if (view->spin_live) {
        if (!view_is_single_surface(view)) {
            /* A menu opened, or the client grew a subsurface, mid-spin. The
             * live path cannot show that, so rebuild onto the snapshot one —
             * setup re-decides and lands on the snapshot for the same reason
             * we are here. */
            if (!view_spin_setup(view)) { view_stop_spin(view); return; }
            src = view->spin_tex;
            redraw = true;
        } else {
            src = wlr_surface_get_texture(view_surface(view));
            /* A new frame from the client: same window, new texture. */
            if (src && src != view->spin_seen) {
                view->spin_seen = src;
                redraw = true;
            }
        }
        view->content_dirty = 0;
    } else if (view_snapshot_due(view, view->spin_snap_t)) {
        /* The composited path. The live nodes have to come back for the length
         * of the pass; nothing is presented in between, so the window never
         * flashes upright. */
        view->content_dirty = 0;
        view->spin_snap_t = 0.0;
        view->server->fx_snaps++;
        redraw = true;
        view_set_content_enabled(view, true);
        /* The rotated picture is itself a buffer in this subtree, and the
         * snapshot pass collects every enabled buffer it finds — leave it on
         * and each refresh bakes the previous tilted frame into the next one,
         * one ghost image deeper every time. */
        wlr_scene_node_set_enabled(&view->spin_buf->node, false);
        view_snapshot_into(view, view->spin_src);
        wlr_scene_node_set_enabled(&view->spin_buf->node, true);
        view_set_content_enabled(view, false);
    }

    /* A window that has come to rest keeps the angle it stopped at, and the
     * effect then costs nothing per frame: redrawing an unchanged rotation of
     * an unchanged snapshot would just burn the GPU for an identical picture.
     * (Half a milliradian is well under a pixel of travel at any window size.) */
    if (fabs(angle - view->spin_angle) > 5e-4) redraw = true;
    if (!redraw) return;

    int size = view->spin_size;
    struct wlr_buffer *dst = view->spin_dst[view->spin_flip];

    if (rotate_blit(view->server->wlr_renderer, dst, view->spin_tex,
                    view->spin_w, view->spin_h, angle)) {
        view->spin_flip ^= 1;
        wlr_scene_buffer_set_buffer(view->spin_buf, dst);
        wlr_scene_buffer_set_dest_size(view->spin_buf, size, size);
        wlr_scene_buffer_set_transform(view->spin_buf, WL_OUTPUT_TRANSFORM_NORMAL);
        /* The target is centered on the window: half the slack on each side. */
        wlr_scene_node_set_position(&view->spin_buf->node,
                                    -(size - view->spin_w) / 2,
                                    -(size - view->spin_h) / 2);
    } else {
        /* No arbitrary rotation available (a non-GLES2 renderer, a shader that
         * would not build). Rather than dropping the effect entirely, show the
         * snapshot at the nearest quarter turn — those four angles ARE
         * expressible in the scene graph. The window then turns in steps
         * instead of smoothly, which still reads as a window that rotates.
         *
         * This path hands a BUFFER to the scene, which the live path does not
         * have — it draws from the client's texture, and that is not ours to
         * give away. Setup only chooses live when rotation is supported, so
         * arriving here with no snapshot means the blit failed for some other
         * reason; keep the last picture rather than blanking the window. */
        if (!view->spin_src) return;
        int quarter = ((int)lround(angle / (M_PI / 2.0)) % 4 + 4) % 4;
        static const enum wl_output_transform steps[4] = {
            WL_OUTPUT_TRANSFORM_NORMAL, WL_OUTPUT_TRANSFORM_90,
            WL_OUTPUT_TRANSFORM_180,    WL_OUTPUT_TRANSFORM_270,
        };
        /* A quarter turn swaps the window's width and height. */
        int qw = (quarter % 2) ? view->spin_h : view->spin_w;
        int qh = (quarter % 2) ? view->spin_w : view->spin_h;
        wlr_scene_buffer_set_buffer(view->spin_buf, view->spin_src);
        wlr_scene_buffer_set_transform(view->spin_buf, steps[quarter]);
        wlr_scene_buffer_set_dest_size(view->spin_buf, qw, qh);
        wlr_scene_node_set_position(&view->spin_buf->node,
                                    (view->spin_w - qw) / 2,
                                    (view->spin_h - qh) / 2);
    }
    view->spin_angle = angle;
}

/* ── shell-agnostic accessors ─────────────────────────────────────────── */

struct wlr_surface *view_surface(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) return view->xdg_toplevel->base->surface;
    return view->xwl_surface->surface; /* NULL until the X11 window associates */
}

const char *view_title(FwmView *view) {
    return view->type == FWM_VIEW_XDG ? view->xdg_toplevel->title
                                      : view->xwl_surface->title;
}

/* X11's closest equivalent of an app id is the WM_CLASS class. */
const char *view_app_id(FwmView *view) {
    return view->type == FWM_VIEW_XDG ? view->xdg_toplevel->app_id
                                      : view->xwl_surface->class;
}

void view_set_size(FwmView *view, int width, int height) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
    } else {
        // X11 configure carries position too; send screen coords (X clients
        // use them as global root coordinates for e.g. popup placement).
        wlr_xwayland_surface_configure(view->xwl_surface,
            (int16_t)(view->x - view->server->camera_x), (int16_t)view->y,
            (uint16_t)width, (uint16_t)height);
    }
}

void view_sync_position(FwmView *view) {
    if (view->type != FWM_VIEW_XWAYLAND) return;
    view_set_size(view, view->width, view->height);
}

void view_send_close(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    else wlr_xwayland_surface_close(view->xwl_surface);
}

void view_set_activated(FwmView *view, bool activated) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
    } else {
        wlr_xwayland_surface_activate(view->xwl_surface, activated);
        if (activated) {
            wlr_xwayland_surface_restack(view->xwl_surface, NULL, XCB_STACK_MODE_ABOVE);
        }
    }
}

void view_set_fullscreen_hint(FwmView *view, bool fullscreen) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
    } else {
        wlr_xwayland_surface_set_fullscreen(view->xwl_surface, fullscreen);
    }
}

/* ── focus border ─────────────────────────────────────────────────────── */

/* Border box in scene-tree-local coordinates. Split out so the squash can hug
 * a deformed, offset box instead of the window's real geometry. */
static void view_place_borders(FwmView *view, int x, int y, int w, int h) {
    if (!view->border[0]) return;
    int bw = view->server->config.decor.border_width;

    // top, bottom, left, right — hugging the outside of the window
    wlr_scene_node_set_position(&view->border[0]->node, x - bw, y - bw);
    wlr_scene_rect_set_size(view->border[0], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[1]->node, x - bw, y + h);
    wlr_scene_rect_set_size(view->border[1], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[2]->node, x - bw, y);
    wlr_scene_rect_set_size(view->border[2], bw, h);
    wlr_scene_node_set_position(&view->border[3]->node, x + w, y);
    wlr_scene_rect_set_size(view->border[3], bw, h);
}

/* The window's own box, as committed. */
static void view_border_box(FwmView *view, int *w, int *h) {
    if (view->type == FWM_VIEW_XDG) {
        *w = view->xdg_toplevel->base->current.geometry.width;
        *h = view->xdg_toplevel->base->current.geometry.height;
    } else {
        struct wlr_surface *s = view->xwl_surface->surface;
        *w = s ? s->current.width : 0;
        *h = s ? s->current.height : 0;
    }
    if (*w <= 0) *w = view->width;
    if (*h <= 0) *h = view->height;
}

void view_committed_size(FwmView *view, int *w, int *h) {
    view_border_box(view, w, h);
}

void view_update_border_geometry(FwmView *view) {
    if (!view->border[0]) return;
    if (view->squash_buf) return; /* the squash owns the border box meanwhile */
    int w, h;
    view_border_box(view, &w, &h);
    view_place_borders(view, 0, 0, w, h);
}

void view_set_border_color(FwmView *view, const float color[4]) {
    if (!view->border[0]) return;

    /* No fade compensation needed: during the open animation the window is
     * either hidden entirely or fully opaque under our own cover rect. */
    for (int i = 0; i < 4; i++) {
        wlr_scene_rect_set_color(view->border[i], color);
    }
}

void view_set_border_enabled(FwmView *view, int enabled) {
    if (!view->border[0]) return;
    for (int i = 0; i < 4; i++) {
        wlr_scene_node_set_enabled(&view->border[i]->node, enabled);
    }
}

/* ── fade-in ──────────────────────────────────────────────────────────── */

static void handle_destroy(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, destroy);
    view_destroy(view);
}

static void handle_request_move(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;
    server_start_interactive_move(view->server, view, event->serial);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    server_start_interactive_resize(view->server, view, event->edges, event->serial);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_fullscreen);
    // Under wlroots-0.20, check the requested state on the shell object.
    // A client-initiated fullscreen request maps to real (whole-output) fullscreen.
    bool fullscreen = view->type == FWM_VIEW_XDG
        ? view->xdg_toplevel->requested.fullscreen
        : view->xwl_surface->fullscreen;
    server_set_fullscreen(view->server, view, fullscreen, true);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, set_title);
    if (view->group) group_redraw(view->server, view->group);
    foreign_view_title_changed(view);
    server_request_tray_redraw(view->server);

    /* Clients announce a title before mapping too; reporting those would have
     * a subscriber hear about windows it was never told had opened. */
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->mapped)
        ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_TITLE, view);
}

/* ── X11 (Xwayland) handlers ──────────────────────────────────────────── */

static void xwl_handle_associate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_associate);
    // The wlr_surface exists only now: hook map/unmap/commit here, where the
    // xdg path hooks them at create time.
    wl_signal_add(&view->xwl_surface->surface->events.map, &view->map);
    wl_signal_add(&view->xwl_surface->surface->events.unmap, &view->unmap);
    wl_signal_add(&view->xwl_surface->surface->events.commit, &view->commit);
}

static void xwl_handle_dissociate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_dissociate);
    // Re-init after removal so view_destroy can remove them again safely.
    wl_list_remove(&view->map.link);    wl_list_init(&view->map.link);
    wl_list_remove(&view->unmap.link);  wl_list_init(&view->unmap.link);
    wl_list_remove(&view->commit.link); wl_list_init(&view->commit.link);
}

static void xwl_handle_request_configure(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wlr_surface *surface = view->xwl_surface->surface;
    if (!surface || !surface->mapped) {
        // Not mapped yet: let the client have exactly what it asked for.
        wlr_xwayland_surface_configure(view->xwl_surface, ev->x, ev->y, ev->width, ev->height);
        return;
    }
    // Mapped: the compositor owns the position, honor only the size.
    view->width = ev->width;
    view->height = ev->height;
    physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                      view->width, view->height, view->server->screen_width);
    view_sync_position(view);
}

static void xwl_handle_request_move(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_move);
    server_start_interactive_move(view->server, view, 0);
}

static void xwl_handle_request_resize(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xwayland_resize_event *event = data;
    server_start_interactive_resize(view->server, view, event->edges, 0);
}

static uint32_t next_view_id = 1;

FwmView *view_xwl_create(struct wlr_xwayland_surface *xsurface, struct FwmServer *server) {
    FwmView *view = calloc(1, sizeof(FwmView));
    if (!view) return NULL;

    view->id = next_view_id++;
    view->type = FWM_VIEW_XWAYLAND;
    view->xwl_surface = xsurface;
    view->server = server;

    // map/unmap/commit attach on associate (no wlr_surface yet); init the
    // links so removal in view_destroy is safe even if it never associates.
    view->map.notify = handle_map;       wl_list_init(&view->map.link);
    view->unmap.notify = handle_unmap;   wl_list_init(&view->unmap.link);
    view->commit.notify = handle_commit; wl_list_init(&view->commit.link);

    view->destroy.notify = handle_destroy;
    wl_signal_add(&xsurface->events.destroy, &view->destroy);
    view->xwl_associate.notify = xwl_handle_associate;
    wl_signal_add(&xsurface->events.associate, &view->xwl_associate);
    view->xwl_dissociate.notify = xwl_handle_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &view->xwl_dissociate);
    view->xwl_request_configure.notify = xwl_handle_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &view->xwl_request_configure);
    view->request_move.notify = xwl_handle_request_move;
    wl_signal_add(&xsurface->events.request_move, &view->request_move);
    view->request_resize.notify = xwl_handle_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &view->request_resize);
    view->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &view->request_fullscreen);
    view->set_title.notify = handle_set_title;
    wl_signal_add(&xsurface->events.set_title, &view->set_title);

    wl_list_insert(&server->views, &view->link);
    return view;
}

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server) {
    FwmView *view = calloc(1, sizeof(FwmView));
    if (!view) return NULL;
    
    view->id = next_view_id++;
    view->type = FWM_VIEW_XDG;
    view->xdg_toplevel = toplevel;
    view->server = server;
    
    view->map.notify = handle_map;
    view->unmap.notify = handle_unmap;
    view->commit.notify = handle_commit;
    view->destroy.notify = handle_destroy;
    view->request_move.notify = handle_request_move;
    view->request_resize.notify = handle_request_resize;
    view->request_fullscreen.notify = handle_request_fullscreen;
    view->set_title.notify = handle_set_title;
    
    // In wlroots-0.20, map/unmap are on wlr_surface
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    // Must be the toplevel's own destroy event, not the xdg_surface's: wlroots
    // asserts all toplevel listeners (e.g. request_fullscreen) are removed
    // before the toplevel itself is destroyed, and that happens before the
    // underlying xdg_surface's destroy event fires.
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_move, &view->request_move);
    wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    wl_signal_add(&toplevel->events.set_title, &view->set_title);
    
    wl_list_insert(&server->views, &view->link);
    
    return view;
}

void view_destroy(FwmView *view) {
    if (!view) return;
    
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->set_title.link);
    if (view->type == FWM_VIEW_XWAYLAND) {
        wl_list_remove(&view->xwl_associate.link);
        wl_list_remove(&view->xwl_dissociate.link);
        wl_list_remove(&view->xwl_request_configure.link);
    }
    wl_list_remove(&view->link);
    
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->mapped) {
        view_unmap(view);
    }
    if (view->last_buffer) {
        wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = NULL;
    }
    
    free(view);
}

void view_map(FwmView *view) {
    // Parented to the windows layer (below the overlay layer) so raising a
    // view to top on focus can never cover the tray/hints/welcome overlays.
    if (view->type == FWM_VIEW_XDG) {
        // In wlroots-0.20, use wlr_scene_xdg_surface_create on xdg_toplevel->base.
        view->scene_tree = wlr_scene_xdg_surface_create(view->server->layer_windows, view->xdg_toplevel->base);
    } else {
        view->scene_tree = wlr_scene_tree_create(view->server->layer_windows);
        if (view->scene_tree &&
            !wlr_scene_surface_create(view->scene_tree, view->xwl_surface->surface)) {
            wlr_scene_node_destroy(&view->scene_tree->node);
            view->scene_tree = NULL;
        }
    }
    if (!view->scene_tree) {
        fprintf(stderr, "Failed to create scene tree for view\n");
        return;
    }
    
    view->scene_tree->node.data = view;
    if (view->type == FWM_VIEW_XDG) {
        // Popups look their parent's scene tree up through xdg_surface->data
        // (see handle_new_xdg_popup in server.c).
        view->xdg_toplevel->base->data = view->scene_tree;
    }

    // Focus border rects (created disabled-color as "inactive"; focus recolors).
    int bw = view->server->config.decor.border_width;
    if (bw > 0) {
        for (int i = 0; i < 4; i++) {
            view->border[i] = wlr_scene_rect_create(view->scene_tree, 1, 1,
                                                    theme_get()->border_inactive);
        }
    }

    // Open animation: hide the window outright until the client has painted
    // something real. Disabling the node is absolute — unlike opacity 0 it
    // cannot be undone by a new scene node appearing on a client commit.
    if (view->server->config.decor.fade_in_ms > 0.0) {
        view->open_anim = 1;
        view->open_t = 0.0;
        view->open_hold = 2;
        view->open_hold_ms = 0.0;
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    int initial_w, initial_h;
    if (view->type == FWM_VIEW_XDG) {
        initial_w = view->xdg_toplevel->base->current.geometry.width;
        initial_h = view->xdg_toplevel->base->current.geometry.height;
    } else {
        initial_w = view->xwl_surface->width;
        initial_h = view->xwl_surface->height;
    }
    if (initial_w <= 0) initial_w = view->server->screen_width / 2;
    if (initial_h <= 0) initial_h = view->server->screen_height / 2;
    
    view->width = initial_w;
    view->height = initial_h;

    /* Window rules ([[rule]] in config.toml) are matched once, here, because
     * app_id and title are what the client announced before mapping. Matching
     * BEFORE the desktop is chosen means the position, the physics body and
     * bsp_insert below all agree on where the window lives — nothing
     * downstream needs to know a rule was involved. */
    ConfigRule rule;
    int have_rule = config_match_rules(&view->server->config,
                                       view_app_id(view), view_title(view), &rule);

    int current_desktop = view->server->target_camera_x / view->server->screen_width;
    if (have_rule && rule.desktop >= 0) current_desktop = rule.desktop;

    /* A window from an application this session relaunched goes back where it
     * was. Checked after [[rule]] so that a rule the user wrote by hand still
     * wins over what merely happened to be true last time. */
    int restored = session_claim_desktop(view->server, view);
    if (restored >= 0 && !(have_rule && rule.desktop >= 0)) current_desktop = restored;
    int cx = current_desktop * view->server->screen_width + (view->server->screen_width - initial_w) / 2;
    int cy = (view->server->screen_height - initial_h) / 2;
    
    view->x = cx;
    view->y = cy;
    
    view_set_size(view, view->width, view->height);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x - view->server->camera_x, view->y);
    view_update_border_geometry(view);

    PhysicsBody *body = physics_sync_body(&view->server->physics, view->id, view->x, view->y, view->width, view->height, view->server->screen_width);

    /* No body means the window is past MAX_WINDOWS: it will map and be usable,
     * but physics, collisions and tiling will all skip it. That used to happen
     * in complete silence, leaving one inexplicably inert window; say it once,
     * through the same tray pill that reports config problems. */
    if (!body && !view->server->warned_window_limit) {
        view->server->warned_window_limit = 1;
        config_report_error(&view->server->config,
                            "window limit reached (%d) — new windows open without physics",
                            MAX_WINDOWS);
        wlr_log(WLR_ERROR, "MAX_WINDOWS (%d) reached; window %u has no physics body",
                MAX_WINDOWS, view->id);
        server_request_tray_redraw(view->server);
    }
    
    if (body) {
        body->shaped = 0;
        body->corner_mode = (view->server->desktop_mode[body->desktop_id] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
        /* Rule properties live on the physics body, not the view. */
        if (have_rule) {
            if (rule.nocollide >= 0) body->no_collide = rule.nocollide;
            if (rule.pin       >= 0) body->pinned     = rule.pin;
            /* Material: copied across as-is, NANs included, because NAN is
             * exactly what the body wants for "defer to the desktop". */
            body->rule_mass     = rule.mass;
            body->rule_gravity  = rule.gravity;
            body->rule_bounce   = rule.bounce;
            body->rule_friction = rule.friction;
        }
    }
    
    /* Publish to external panels BEFORE focusing, so the activation state that
     * server_focus_view pushes lands on an existing handle. */
    foreign_view_map(view);
    server_focus_view(view->server, view);

    int desktop = body ? body->desktop_id : current_desktop;
    if (view->server->desktop_mode[desktop] == DESKTOP_MODE_TILING) {
        bsp_insert(&view->server->bsp_roots[desktop], view->server->focused_view ? view->server->focused_view->id : 0, view->id);
        server_apply_tiling(view->server, desktop);
    } else if (view->server->desktop_mode[desktop] == DESKTOP_MODE_FLOATING) {
        /* Overlapping is the whole point of floating — shoving the new window
         * clear of the others would be the physics behaviour this mode exists
         * to switch off. */
        if (body) body->floating = 1;
    } else {
        physics_push_overlapping(&view->server->physics, view->id, 300.0);
    }

    /* Focus, tiling and sizing above may have re-enabled or repositioned
     * things; the window must stay hidden until its content is ready. */
    if (view->open_anim && view->open_hold > 0) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    server_request_tray_redraw(view->server);

    /* Last, so a subscriber that reacts by dispatching an action finds the
     * window already placed, tiled and focused rather than half-mapped. */
    ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_OPEN, view);
}

void view_unmap(FwmView *view) {
    /* First, while the window is still whole: below this line its body, its
     * tile and its title are being taken apart. */
    ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_CLOSE, view);

    foreign_view_unmap(view);
    /* Before anything else: the snapshot lives in scene_tree, which is about to
     * go, and it holds a buffer lock the close ghost may want back. */
    view_stop_squash(view);
    view_jelly_stop(view);
    view_stop_spin(view);

    /* Which desktop to re-home the keyboard on, read before the body goes. */
    PhysicsBody *ub = physics_find_body(&view->server->physics, view->id);
    int home_desktop = ub ? ub->desktop_id
                          : view->server->target_camera_x / view->server->screen_width;

    group_remove(view->server, view); /* no-op when not grouped */
    physics_remove_body(&view->server->physics, view->id);
    
    for (int i = 0; i < 10; i++) {
        if (bsp_find(view->server->bsp_roots[i], view->id)) {
            bsp_remove(&view->server->bsp_roots[i], view->id);
            if (view->server->desktop_mode[i] == DESKTOP_MODE_TILING) {
                server_apply_tiling(view->server, i);
            }
        }
    }
    
    int was_focused = view->server->focused_view == view;
    if (was_focused) {
        view->server->focused_view = NULL;
    }
    if (view->server->last_touched_view == view) {
        view->server->last_touched_view = NULL;
    }
    if (view->server->interactive.view == view) {
        view->server->interactive.view = NULL;
    }
    
    // Close animation: leave a snapshot of the last frame fading out (the
    // mirror of the map fade-in). The ghost takes over the buffer lock; the
    // physics tick fades it and frees it.
    if (view->last_buffer && view->server->config.decor.fade_in_ms > 0.0) {
        FwmGhost *ghost = calloc(1, sizeof(*ghost));
        if (ghost) {
            ghost->scene_buffer = wlr_scene_buffer_create(view->server->layer_windows, view->last_buffer);
        }
        if (ghost && ghost->scene_buffer) {
            // The raw buffer's top-left sits above/left of the xdg geometry
            // (CSD shadows) — compensate like the xdg scene helper does.
            // X11 surfaces have no geometry box: the buffer IS the window.
            struct wlr_box geo = {0};
            if (view->type == FWM_VIEW_XDG) {
                geo = view->xdg_toplevel->base->current.geometry;
            }
            ghost->buffer = view->last_buffer;
            view->last_buffer = NULL;
            ghost->x = view->x - geo.x;
            ghost->y = view->y - geo.y;
            wlr_scene_node_set_position(&ghost->scene_buffer->node,
                                        (int)ghost->x - view->server->camera_x, (int)ghost->y);
            wlr_scene_node_raise_to_top(&ghost->scene_buffer->node);
            wl_list_insert(&view->server->ghosts, &ghost->link);
        } else {
            free(ghost);
        }
    }
    if (view->last_buffer) {
        wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = NULL;
    }

    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
        if (view->type == FWM_VIEW_XDG) view->xdg_toplevel->base->data = NULL;
    }
    // Border rects and the open-animation cover were children of scene_tree —
    // destroyed with it.
    for (int i = 0; i < 4; i++) view->border[i] = NULL;
    view->open_cover = NULL;
    view->open_anim = 0;
    view->open_hold = 0;

    /* Only now, with this window's scene nodes gone, is it safe to ask what is
     * under the cursor. Without this the keyboard sits nowhere until the
     * pointer happens to move — closing the top window of a stack left the one
     * underneath unfocused even though the cursor was already over it. */
    if (was_focused) {
        server_refocus(view->server, home_desktop, view);
    }

    server_request_tray_redraw(view->server);
}
