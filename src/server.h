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

#ifndef FWM_SERVER_H
#define FWM_SERVER_H

#include <time.h>
#include <wlr/util/box.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <xkbcommon/xkbcommon.h>

#include "physics.h"
#include "bsp.h"
#include "config.h"
#include "gestures.h"

#define DESKTOP_MODE_PHYSICS 0
#define DESKTOP_MODE_TILING  1
/* "Normal desktop environment": windows keep the position you drop them at and
 * overlap freely — no gravity, no shoving, no layout. Mechanically it is the
 * per-window pinned + no_collide pair raised to the whole desktop. */
#define DESKTOP_MODE_FLOATING 2

typedef enum {
    FWM_ACTION_NONE,
    FWM_ACTION_MOVE,
    FWM_ACTION_RESIZE,
    FWM_ACTION_SWAP,
    FWM_ACTION_BSP_RESIZE,
    /* Turning a window with the mouse: the cursor's angle around the window's
     * centre is the window's angle, and letting go hands whatever rate the hand
     * was turning at to the simulation. The window does not move while this
     * runs — it is an anchor, like a resize. */
    FWM_ACTION_TWIST
} FwmInteractiveAction;

struct FwmView;
struct Launcher;

typedef struct {
    FwmInteractiveAction action;
    struct FwmView *view;
    double start_x, start_y;
    int view_start_x, view_start_y;
    int view_start_width, view_start_height;
    
    /* Throw speed history */
    double last_x, last_y;
    struct timespec last_time;
    double vx, vy;
    double hist_x[4];
    double hist_y[4];
    struct timespec hist_time[4];
    int hist_count;
    int collision_disabled;

    /* Swirl: how the cursor's direction of travel is turning, which is what
     * winds a spinning window up mid-drag (see server_pointer.c). `dir` is the
     * angle of the velocity vector at the last sample; `acc`/`abs`/`span` are
     * a leaky integral of how far it has turned, how much it turned either
     * way, and over how long — so the rate is read from a fifth of a second of
     * hand movement, and a wobble that nets out to nothing is told apart from
     * a circle that does not. */
    double swirl_dir;
    struct timespec swirl_time;
    double swirl_acc, swirl_abs, swirl_span;
    int swirl_have;   /* a previous sample exists to compare against */

    /* Where the drag took hold of the window, in the window's OWN frame
     * (unrotated, relative to its center). A spinning window hangs from this
     * point: grab it by a corner and it swings like a real object held there
     * (server.c, server_drag_swing). Fixed for the length of the drag — the
     * hand does not slide along the window. */
    double grab_lx, grab_ly;
    /* The grab point in world coordinates, and how fast it is moving, as of
     * the last physics tick. The swing is driven by this point's
     * ACCELERATION, so both are needed. */
    double pivot_x, pivot_y;
    double pivot_vx, pivot_vy;
    /* ... and the smoothed acceleration itself, which is state rather than a
     * local because it is filtered across ticks (SWING_ACC_TAU). */
    double pivot_ax, pivot_ay;
    int pivot_have;
    
    /* Twist (FWM_ACTION_TWIST). `twist_base` is the angle the window is being
     * held at — seeded from its own angle at the grab, then moved by however
     * far the cursor turns around its centre, so the window never jumps to meet
     * the hand. `twist_last` is the previous cursor angle, for unwrapping.
     * `twist_vel` is how fast the hand is turning, smoothed, because the
     * release hands that rate to the simulation and one stuttering frame must
     * not be what decides it. */
    double twist_base, twist_last;
    double twist_vel;
    struct timespec twist_time;

    /* BSP resize */
    BspNode *bsp_node;
    float bsp_start_ratio;
    
    /* Swap drag */
    double cur_x, cur_y;
} FwmInteractiveState;

/* Snapshot of a closed window's last frame, fading out (close animation).
 * Owns one lock on `buffer`; released when the fade ends. */
typedef struct FwmGhost {
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_buffer *buffer;
    double x, y; /* world coordinates (camera-independent) */
    double t;    /* fade progress 0 -> 1 */
    struct wl_list link;
} FwmGhost;

typedef struct FwmServer {
    struct wl_display *wl_display;
    struct wlr_backend *wlr_backend;
    struct wlr_session *session; /* NULL on nested backends; used for VT switching */
    struct wlr_renderer *wlr_renderer;
    struct wlr_allocator *wlr_allocator;
    struct wlr_scene *scene;
    struct wlr_scene_tree *layer_background;
    struct wlr_scene_tree *layer_windows;
    struct wlr_scene_tree *layer_overlay;
    /* wlr-layer-shell trees, interleaved with ours (bottom to top):
     * wallpaper < ls_background < ls_bottom < windows < ls_top < our overlays
     * < ls_overlay. See src/layer.h. */
    struct wlr_scene_tree *ls_background;
    struct wlr_scene_tree *ls_bottom;
    struct wlr_scene_tree *ls_top;
    struct wlr_scene_tree *ls_overlay;
    struct FwmWallpaper *wallpaper;
    struct FwmWallpaper *wallpaper_prev; /* outgoing set, alive during a cross-fade */
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces;             /* FwmLayerSurface.link */
    struct FwmLayerSurface *focused_layer;     /* owns the keyboard, if any */
    struct wlr_box usable_area;                /* screen minus exclusive zones */
    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;
    
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    
    struct wl_list views;
    struct wl_list groups; /* FwmGroup tab-stacks */
    struct wl_list ghosts; /* FwmGhost close-animation snapshots */ /* FwmView list */
    struct FwmView *focused_view;
    struct FwmView *last_touched_view;
    
    struct wl_list outputs;
    
    /* Inputs */
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor;
    struct wl_listener seat_request_set_selection;
    struct wl_listener seat_request_set_primary_selection;

    /* Touchpad gestures. The state machine is gestures.c; the wiring, and what
     * each resolved gesture does, is server_gestures.c. A gesture fwm has
     * nothing bound to is forwarded to the client through pointer_gestures
     * instead, so in-app pinch-zoom keeps working. */
    struct wlr_pointer_gestures_v1 *pointer_gestures;
    GestureState gesture;
    int gesture_base_camera; /* camera the live desktop pan started from */
    struct wl_listener cursor_swipe_begin;
    struct wl_listener cursor_swipe_update;
    struct wl_listener cursor_swipe_end;
    struct wl_listener cursor_pinch_begin;
    struct wl_listener cursor_pinch_update;
    struct wl_listener cursor_pinch_end;
    struct wl_listener cursor_hold_begin;
    struct wl_listener cursor_hold_end;

    /* Drag and drop. The data transfer itself is handled entirely by
     * wlr_data_device; all we own is the icon drawn under the cursor. */
    struct wl_listener seat_request_start_drag;
    struct wl_listener seat_start_drag;
    struct wlr_scene_tree *drag_icon;      /* NULL when no drag is running */
    struct wl_listener drag_icon_destroy;

    /* xdg-activation: apps asking to be raised/focused (a link opening in an
     * already-running browser, a chat client jumping to a message). */
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener xdg_activation_request_activate;

    /* Idle: ext-idle-notify tells idle daemons (swayidle) when the user goes
     * quiet; idle-inhibit lets a client (a video player) suppress that. */
    /* Window list for external panels (waybar taskbar); see foreign.h. */
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel;

    /* Pointer capture: games and 3D viewports lock the cursor and steer from
     * raw deltas instead of its absolute position. */
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wl_listener new_pointer_constraint;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_pointer_constraint_v1 *active_constraint; /* NULL when free */
    struct wl_listener constraint_destroy;

    /* Display power (swayidle turning the screen off), gamma (wlsunset night
     * light) and client-requested cursor shapes. */
    struct wlr_output_power_manager_v1 *output_power;
    struct wl_listener output_power_set_mode;
    struct wlr_gamma_control_manager_v1 *gamma_control;
    struct wlr_cursor_shape_manager_v1 *cursor_shape;
    struct wl_listener cursor_shape_request;

    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
    int idle_inhibited;                    /* last state pushed to the notifier */

    /* ext-session-lock-v1. `locked` stays set if the lock client dies, which
     * is what keeps a crashed locker from becoming an unlock — see src/lock.h. */
    /* Control socket (src/ipc.h). NULL if it could not be created. */
    struct FwmIpc *ipc;

    /* Session save/restore bookkeeping (src/session.c); opaque here. Not to be
     * confused with `session` above, which is the libseat/VT session. */
    void *session_state;

    /* MAX_WINDOWS overflow is reported once per session, not once per window:
     * the tray pill stores a capped 24 messages and would otherwise fill with
     * copies of the same one. */
    int warned_window_limit;

    struct wlr_session_lock_manager_v1 *lock_manager;
    struct wlr_session_lock_v1 *lock;      /* NULL once the client is gone */
    int locked;
    struct wlr_scene_tree *layer_lock;     /* above everything, incl. ls_overlay */
    struct wl_list lock_surfaces;          /* FwmLockSurface.link */
    struct wl_listener new_lock;
    struct wl_listener new_lock_surface;
    struct wl_listener lock_unlock;
    struct wl_listener lock_destroy;
    
    /* Keyboard input */
    struct wl_list keyboards;
    struct wl_list pointers;   /* struct FwmPointer; see server_internal.h */
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_toplevel_decoration;
    struct wl_listener xwl_ready;
    struct wl_listener xwl_new_surface;

    /* Held-key auto-repeat for repeatable binds (e.g. move_camera) */
    struct wl_event_source *key_repeat_timer;
    const char *repeat_action;
    /* Launcher key auto-repeat (arrows/backspace/typing while it is open). */
    int repeat_l_active;
    xkb_keysym_t repeat_l_sym;
    char repeat_l_utf8[16];   /* points into config.keys[].action; NULL when idle */
    uint32_t repeat_keycode;     /* raw event keycode currently repeating */
    unsigned char key_consumed[768]; /* per-keycode: press was eaten by a bind,
                                        swallow the matching release too */
    int group_click; /* tab-bar click consumed; swallow its release */

    /* When the last physics step finished. The tick timer and the display's
     * vsync are different clocks — the timer is armed in whole milliseconds, so
     * 60Hz becomes 16ms and runs at 62.5Hz — and anything the simulation
     * integrates therefore advances in steps that do not line up with the
     * frames it is drawn on. Rendering interpolates from this; see
     * server_render_angle. */
    struct timespec last_tick;
    /* When the tick callback last ran, and the real time it has taken in but
     * not yet spent on a whole step. Together these say how far behind the
     * clock the simulation is at any instant. */
    struct timespec tick_real_prev;
    double sim_accum;

    /* Effect frame diagnostics, on only when FWM_DEBUG_EFFECTS is set in the
     * environment. Judder is a timing complaint, and timing cannot be argued
     * about from the code — this counts what actually reached the screen while
     * a window was spinning or wobbling, and says so once a second.
     * `fx_snaps` counts composited re-photographs, `fx_moved` the frames where
     * the picture actually changed. */
    int fx_debug;
    struct timespec fx_since;
    int fx_frames, fx_snaps, fx_moved;
    double fx_dt_min, fx_dt_max;
    /* How evenly a rotation actually reaches the screen.
     *
     * Not the angle step itself: over a second that conflates judder with a
     * spin honestly slowing down, and it reads a wrap past +-pi as a 355-degree
     * jump. What matters is the drawn angular SPEED — step over the real time
     * the frame took — and how much it changes from one frame to the next.
     * Smooth motion holds it steady whatever the frame times are; judder is
     * precisely this number jumping. Reported as the worst frame-to-frame
     * change, in percent. */
    double fx_omega_prev;
    double fx_omega_jump;   /* worst |domega|/omega seen this second, 0..1 */
    int    fx_omega_have;
    double fx_snap_us;   /* time spent flattening subtrees this second */

    /* The [mode.<name>] submap the keyboard is currently in, as an index into
     * config.modes, or -1 for the root map. While a mode is active it owns
     * every key: its binds fire, Escape leaves, and nothing else reaches the
     * focused client. Reset on config reload, since the modes are rebuilt from
     * scratch and the index would otherwise point at a different one. */
    int key_mode;
    
    /* Physics and desktop coordinates */
    PhysicsWorld physics;
    int camera_x;
    int target_camera_x;
    /* Desktop-switch slide: timed ease-in-out from cam_anim_from to
     * cam_anim_to; retargets smoothly if target_camera_x changes mid-flight. */
    int cam_anim;
    int cam_anim_from, cam_anim_to;
    double cam_anim_t;
    /* Continuous free pan (a held move_camera: bind). Must NOT use the slide
     * above: that animator restarts its fixed-duration ease every time the
     * target moves, and a held bind moves it every 40ms, so the camera only
     * ever completed the slowest ~1% of an ease-in-out and then caught up in
     * one jump on release. Free pan chases the target exponentially instead. */
    int cam_free;
    /* Impact shake. Deliberately a RENDER-ONLY offset applied to the world
     * layer trees: camera_x must not move, because edge auto-scroll and the
     * active-desktop test compare it against target_camera_x exactly. */
    /* FWM_TEST_ACTION debug hook: fires one action shortly after startup. */
    char *test_action;
    struct wl_event_source *test_action_timer;
    int focus_desktop;  /* desktop server_refocus last homed the keyboard on */
    int tick_idle;      /* physics timer is on the slow heartbeat */
    double shake_mag;   /* px; decays to 0 */
    double shake_t;     /* seconds since the last impact, drives the oscillation */
    int screen_width;
    int screen_height;
    
    BspNode *bsp_roots[10];
    int desktop_mode[10];
    
    FwmConfig config;
    FwmInteractiveState interactive;
    
    struct wl_event_source *physics_timer;
    /* Drives frames at a playing video wallpaper's own fps, so video does not
     * pin the whole compositor to 60 Hz. Armed only while a video plays and is
     * not covered; see server_video_sync. */
    struct wl_event_source *video_timer;
    int video_timer_on;
    struct timespec last_anim; /* frame-time clock for visual animations */
    
    /* UI scene nodes */
    struct wlr_scene_buffer *tray_buffer;
    /* Tray hidden by the user (toggle_tray). Unlike the automatic hide under a
     * real-fullscreen window, this also gives the strip back: tile_area and
     * fake fullscreen stop reserving TRAY_BOTTOM, so windows grow into it. */
    int tray_hidden;
    struct wlr_scene_buffer *hints_buffer;
    struct wlr_scene_buffer *welcome_buffer;
    struct wlr_scene_buffer *errors_buffer; /* config-error detail panel */
    struct Launcher *launcher;
    
    int running;
} FwmServer;

bool server_init(FwmServer *server);
void server_run(FwmServer *server);
void server_destroy(FwmServer *server);

/* Ask every output for a frame. The scene schedules frames off its own damage,
 * so this is only for changes that damage nothing — a colour transform, or the
 * idle heartbeat. */
void server_schedule_frames(FwmServer *server);

/* Re-home the keyboard after the focused window disappears or the camera lands
 * on another desktop, instead of waiting for the next pointer motion.
 * `skip` excludes a view that is unmapping but still listed; NULL otherwise. */
void server_refocus(FwmServer *server, int desktop, struct FwmView *skip);
void server_focus_view(FwmServer *server, struct FwmView *view);
void server_apply_tiling(FwmServer *server, int desktop);
/* Re-run tile positioning against the sizes clients actually committed. Called
 * when a tiled window commits a size different from the one it was asked for. */
void server_align_tiles(FwmServer *server, int desktop);
/* Move one desktop to DESKTOP_MODE_*, running the leave/enter work for both
 * the old and the new mode. No-op when the desktop is already in that mode. */
void server_set_desktop_mode(FwmServer *server, int d, int mode);
void server_start_interactive_move(FwmServer *server, struct FwmView *view, uint32_t serial);
void server_start_interactive_resize(FwmServer *server, struct FwmView *view, uint32_t edges, uint32_t serial);
/* real=true: true fullscreen over the whole output (client is told it is
 * fullscreen). real=false: "fake" fullscreen filling the work area below the
 * tray, with the client left in its normal windowed state. Ignored when
 * fullscreen=false. */
void server_set_fullscreen(FwmServer *server, struct FwmView *view, bool fullscreen, bool real);
void server_request_tray_redraw(FwmServer *server);
/* Re-read the config file and re-apply everything that can change at runtime
 * (physics, decor, tiling, keymap, wallpaper, binds). Errors are reported
 * through the tray pill, never by failing. */
void server_reload_config(FwmServer *server);
/* Push the in-memory config onto the live compositor without touching the
 * file. Used by server_reload_config (rebuild_wallpaper = 1) and by
 * `fwmctl set`, which must not pay for an image decode per keystroke (0). */
void server_apply_config(FwmServer *server, int rebuild_wallpaper);
/* Copy the config's physics onto the live world: the scalars, and the
 * per-desktop profiles built out of them. Split out because startup and reload
 * both need exactly this and nothing else. */
void server_apply_physics_config(FwmServer *server);
/* Run a keybind action from outside the keyboard path (see src/ipc.c). */
void server_dispatch_action_external(FwmServer *server, const char *action);
/* Swap the wallpaper at runtime: rebuilds the layers, recomputes the palette
 * when [decor] color_source = "wallpaper", and remembers the choice in the
 * state file so it survives a restart. Replaces the FIRST [[wallpaper]] layer;
 * further parallax layers keep their images. */
void server_set_wallpaper(FwmServer *server, const char *path);

#endif /* FWM_SERVER_H */
