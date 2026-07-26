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

#ifndef FWM_VIEW_H
#define FWM_VIEW_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>
#include <stdbool.h>

struct FwmServer;
struct FwmGroup;

typedef enum {
    FWM_VIEW_XDG,      /* native Wayland xdg-shell toplevel */
    FWM_VIEW_XWAYLAND, /* X11 window under Xwayland */
} FwmViewType;

typedef struct FwmView {
    uint32_t id; /* Unique ID matching the ID in physics */
    FwmViewType type;
    struct wlr_xdg_toplevel *xdg_toplevel;       /* NULL for X11 views */
    struct wlr_xwayland_surface *xwl_surface;    /* NULL for xdg views */
    struct wlr_scene_tree *scene_tree;
    
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener set_title;
    /* X11-only listeners */
    struct wl_listener xwl_associate;
    struct wl_listener xwl_dissociate;
    struct wl_listener xwl_request_configure;
    
    /* Saved geometry (local coordinates in desktop) */
    int x, y;
    int width, height;

    /* Tile-glide animation: when tile_anim is set, the physics tick eases the
     * window toward (tile_tx, tile_ty) instead of snapping it there. */
    int tile_anim;
    double tile_tx, tile_ty;

    /* Size this view was last aligned against. The layout re-runs when a
     * client commits a different one; without this it would re-run on every
     * commit, cursor blink included. */
    int aligned_w, aligned_h;

    /* Focus border: 4 rects (top, bottom, left, right) parented to scene_tree,
     * so they move with the window for free. NULL when borders are disabled. */
    struct wlr_scene_rect *border[4];

    /* Open animation.
     *
     * The client's surface is NEVER blended at partial opacity: ramping it was
     * tried and kept producing a visible flash at the start, because a client's
     * first frames (blank, white, half-drawn) get composited mid-ramp. Instead:
     *
     *   1. the whole scene tree is DISABLED at map, so nothing shows at all;
     *   2. `open_hold` counts commits until the client has painted real
     *      content (commit #1 is the mapping commit itself);
     *   3. the tree is then enabled fully opaque, and a solid cover rect that
     *      we draw ourselves fades out over it while the window rises into
     *      place. Everything that blends is ours, so a client frame can never
     *      appear half-transparent. */
    int open_anim;
    double open_t;
    int open_hold;
    double open_hold_ms;
    struct wlr_scene_rect *open_cover;

    /* Last committed buffer, kept locked so view_unmap can leave a fading
     * close-animation snapshot (FwmGhost) after the client buffer is gone. */
    struct wlr_buffer *last_buffer;

    /* Impact squash & stretch. Deforms a SNAPSHOT of the last committed frame,
     * never the live surface: wlroots' scene resets a surface buffer's
     * dest_size on every client commit, so a live deformation would be wiped
     * out the moment the client redraws. The real content is hidden for the
     * ~250ms this runs; the window is effectively a still frame, which is
     * imperceptible at impact speed and is the same trade the close ghost
     * already makes. */
    struct wlr_scene_buffer *squash_buf;
    struct wlr_buffer *squash_lock;   /* our own lock on the snapshot */
    double squash_t;                  /* seconds since the impact */
    double squash_amount;             /* peak deformation, 0..1 */
    double squash_nx, squash_ny;      /* impact normal, points at the contact */

    /* Wobble ("jelly") while a window is dragged. Shares the snapshot slot
     * above — squash_buf is the deformed node either way, so the wobble, the
     * impact squash and the spin can never be on screen at the same time and
     * whichever starts last takes the slot over.
     *
     * The model is one lagging mass on a spring: `jelly_mx/my` chases the
     * window's real position, and the distance it is behind by IS the
     * deformation. Nothing here is driven by the drag's velocity directly —
     * a spring lags on its own, which is what makes shaking the window build
     * up a wobble that keeps going for a moment after the hand stops. */
    int jelly;                        /* the wobble owns squash_buf */
    int jelly_settling;               /* let go: relax to rest, then give the
                                       * live window back */
    double jelly_mx, jelly_my;        /* the mass, world px */
    double jelly_vx, jelly_vy;        /* its velocity, px/s */
    double jelly_px, jelly_py;        /* window position at the last tick */
    /* How stretched each axis currently is, in lag px — an envelope that
     * follows the lag's MAGNITUDE up quickly and down slowly, never the
     * magnitude itself. A shaken jelly stays stretched along the shake; taking
     * the magnitude straight collapses the window back to its resting shape at
     * every zero crossing of the wobble, twice per shake. */
    double jelly_ampx, jelly_ampy;
    /* Where the deformed box is drawn, smoothed. Shaken faster than the spring
     * can follow, the lag saturates into what is very nearly a square wave, and
     * the picture would jump between its two extremes in single frames. */
    double jelly_ox, jelly_oy;
    /* The spare snapshot. A drag lasts seconds, not the quarter second an
     * impact does, so the frozen picture is refreshed as the spin's is — into
     * this one while the scene is still showing the other, then the two swap.
     * Overwriting the buffer on screen would tear. */
    struct wlr_buffer *jelly_alt;
    double jelly_snap_t;              /* seconds since the last refresh */

    /* Free rotation (experimental; see PhysicsBody.spin).
     *
     * Same snapshot trick as the squash, for the same reason and one step
     * further: wlr_scene is axis-aligned to its bones — node positions are
     * ints and a scene buffer's only transform is one of the eight
     * wl_output ones — so a genuinely tilted window cannot be expressed in
     * the scene graph at all. What CAN be is an upright buffer whose CONTENT
     * is a rotated picture, which is what this is: the window's subtree is
     * flattened into `spin_src`, that gets drawn rotated into one of the two
     * `spin_dst` buffers, and the scene shows the result.
     *
     * The cost is that the window is a still frame while it spins, so the
     * snapshot is retaken a few times a second; the benefit is that damage,
     * compositing and scanout keep working exactly as they always did. */
    struct wlr_scene_buffer *spin_buf;
    struct wlr_buffer *spin_src;      /* flattened window content, w x h */
    struct wlr_texture *spin_tex;     /* ... imported once, reused every frame */
    struct wlr_buffer *spin_dst[2];   /* rotated output, square, diagonal-sized */
    int spin_flip;                    /* which of the two to draw into next */
    int spin_border;                  /* were the borders shown before the spin */
    int spin_w, spin_h;               /* window size the snapshot was taken at */
    int spin_size;                    /* side of the spin_dst squares */
    double spin_snap_t;               /* seconds since the last re-snapshot */
    double spin_angle;                /* angle currently on screen, radians */

    /* wlr-foreign-toplevel handle: this window as external panels see it
     * (see foreign.h). NULL while unmapped. */
    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct wl_listener ftl_request_activate;
    struct wl_listener ftl_request_close;
    struct wl_listener ftl_request_fullscreen;

    /* Tab-stack membership; NULL when not grouped (see group.h). */
    struct FwmGroup *group;

    /* Real (whole-output) fullscreen: while such a view is on the active
     * desktop the tray hides — overlays outrank windows in the scene, so
     * this is the only way a fullscreen surface can cover everything. */
    int fs_real;
    
    struct FwmServer *server;
    struct wl_list link;
} FwmView;

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server);
FwmView *view_xwl_create(struct wlr_xwayland_surface *xsurface, struct FwmServer *server);
void view_destroy(FwmView *view);
void view_map(FwmView *view);
void view_unmap(FwmView *view);

/* Shell-agnostic accessors: everything outside view.c must go through these
 * instead of poking xdg_toplevel directly (X11 views have no xdg_toplevel). */
struct wlr_surface *view_surface(FwmView *view);
const char *view_title(FwmView *view);
const char *view_app_id(FwmView *view);
void view_set_size(FwmView *view, int width, int height);
void view_send_close(FwmView *view);
void view_set_activated(FwmView *view, bool activated);
/* Tell the client it is (or is no longer) fullscreen. */
void view_set_fullscreen_hint(FwmView *view, bool fullscreen);
/* Push the current compositor-side position to the client. X11 clients place
 * their popups from it; no-op for xdg views (Wayland has no global coords). */
void view_sync_position(FwmView *view);

/* Border helpers (no-ops when borders are disabled or the view is unmapped). */
/* The size the client actually committed, which is not always the size it was
 * asked for — see server_align_tiles(). Falls back to the requested size
 * before the first commit. */
void view_committed_size(FwmView *view, int *w, int *h);

void view_update_border_geometry(FwmView *view);

/* Impact squash & stretch (see the squash_* fields above).
 * `nx`,`ny` is the contact normal pointing from the window toward whatever it
 * hit; `amount` is the peak deformation, 0..1. Starting one while another runs
 * restarts it. Safe to call when the view has no snapshot to deform — it is
 * simply ignored. */
void view_start_squash(FwmView *view, double nx, double ny, double amount);
void view_squash_tick(FwmView *view, double dt);
void view_stop_squash(FwmView *view);

/* Drag wobble (see the jelly_* fields above).
 *
 * view_jelly_begin arms it when a drag starts; view_jelly_tick, called every
 * frame with the window's current position, runs the spring and deforms the
 * snapshot; view_jelly_release says the drag is over, after which the wobble
 * damps out on its own and hands the live window back.
 *
 * `strength` scales the deformation (config effects.jelly); at 0 begin does
 * nothing. A spinning window is never given a wobble — the two want the same
 * snapshot, and a lag along the screen axes is meaningless on a tilted
 * picture. */
void view_jelly_begin(FwmView *view, double strength);
void view_jelly_tick(FwmView *view, double strength, double dt);
void view_jelly_release(FwmView *view);

/* Free rotation (see the spin_* fields above). view_spin_tick shows the window
 * at `angle` radians, creating the snapshot machinery on the first call and
 * refreshing it as it runs; view_stop_spin puts the live window back. Calling
 * the tick every frame while the body spins is the whole contract — there is
 * no separate "start". */
void view_spin_tick(FwmView *view, double angle, double dt);
void view_stop_spin(FwmView *view);
/* Is this window currently showing a rotated snapshot? */
bool view_is_spinning(FwmView *view);
void view_set_border_color(FwmView *view, const float color[4]);
void view_set_border_enabled(FwmView *view, int enabled);

/* Set opacity (0..1) on every surface buffer of the view (fade-in). */

#endif /* FWM_VIEW_H */
