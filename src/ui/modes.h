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

#ifndef FWM_MODES_H
#define FWM_MODES_H

#include <stdbool.h>
#include <cairo.h>
#include <wlr/types/wlr_scene.h>

/*
 * The modes pill in the tray, and the menu it opens.
 *
 * The pill is drawn by tray.c (it is part of the tray's one buffer); the menu
 * is its own overlay, anchored under the pill the way the error panel is
 * anchored under the warning pill. What lives here is everything both need to
 * agree on: the icons, the state struct, and the hit-testing.
 *
 * Icons are drawn as cairo paths rather than set as text. A glyph font is not
 * something a compositor can assume — the tray already gambles on ⚠ and ⌨ —
 * and four shapes are less code than the fallback logic would be.
 */

enum {
    MODE_ICON_TILING = 0,
    MODE_ICON_FLOATING,
    MODE_ICON_GRAVITY,
    MODE_ICON_CAVA,
    MODE_ICON_COUNT,
};

/* Draw `icon` into a size x size box at (x, y) using the CURRENT cairo source,
 * so the caller owns the colour and therefore the on/off/dimmed distinction. */
void modes_icon(cairo_t *cr, int icon, double x, double y, double size);

/* Everything the pill and the menu render. Filled by the server each frame from
 * the live compositor state, never cached here — a mode changed by a keybind
 * must show up in the pill without anyone telling it. */
typedef struct ModesState {
    int tiling;    /* active desktop is DESKTOP_MODE_TILING */
    int floating;  /* active desktop is DESKTOP_MODE_FLOATING */
    int gravity;   /* physics.gravity_scale > 0 */
    int cava;      /* CAVA_MODE_* */
    double opacity;
} ModesState;

/* Fixed pill width. Fixed on purpose: the pill sits between the desktop island
 * and the clock, both of which move with the screen width and the date string,
 * and a pill that grew with the number of active modes would start overlapping
 * one of them at exactly the moment it had the most to say. */
#define MODES_PILL_W 126

/* Open and close are the same animation in opposite directions, so they read
 * the same two numbers. Split them and they drift. */
#define MODES_MENU_ANIM_MS 170.0
#define MODES_MENU_RISE_PX  14.0

/* Rows of the menu, in draw order. */
enum {
    MODES_ROW_NONE = -1,
    MODES_ROW_TILING = 0,
    MODES_ROW_FLOATING,
    MODES_ROW_GRAVITY,
    MODES_ROW_CAVA,
    MODES_ROW_COUNT,
};

/* Cava's three positions. The config has a fourth (`"physical"`, bars that push
 * without being drawn), deliberately left out of the menu: it is a thing to
 * discover in the file, not to land on by clicking past it. */
enum {
    MODES_CAVA_OFF = 0,
    MODES_CAVA_VISUAL,
    MODES_CAVA_PHYSICAL,
    MODES_CAVA_SEGS,
};

/* Open the menu under the pill. `pill_x` is where the pill starts in SCREEN
 * coordinates, so the menu can line its right edge up with the pill's. */
struct wlr_scene_buffer *modes_menu_show(struct wlr_scene_tree *parent,
                                         int screen_w, int screen_h,
                                         double pill_x, const ModesState *st);

/* Redraw an open menu after a toggle, without the flicker of a destroy+show.
 * The switches do not jump to the new state — they ease toward it on the tick
 * below, and this only records where they are heading. */
void modes_menu_redraw(struct wlr_scene_buffer *buf, const ModesState *st);

/* Advance the menu's own animations (switch knobs sliding, the cava highlight
 * travelling, the rows staggering in) and redraw if anything moved. Call once
 * per frame while the menu is open. Separate from cairo_overlay_tick, which
 * animates the NODE — position and opacity — and cannot reach inside the
 * buffer to move a knob.
 *
 * Returns true while anything is still in motion, so the compositor keeps
 * driving frames instead of dropping to the idle heartbeat mid-slide. */
bool modes_menu_tick(struct wlr_scene_buffer *buf, const ModesState *st, double dt);

/* True while the menu has animation left to run, for server_is_busy. */
bool modes_menu_animating(void);

/* Hit-test the menu, in MENU-BUFFER-LOCAL coordinates. Returns a MODES_ROW_*,
 * or MODES_ROW_NONE outside any row. For MODES_ROW_CAVA, *seg receives the
 * MODES_CAVA_* segment under the point (-1 if the point is on the row but not
 * on the segmented control). *seg is untouched for every other row. */
int modes_menu_hit(double x, double y, int *seg);

/* Size of the menu, so the caller can tell whether a click landed inside it at
 * all before converting coordinates. */
void modes_menu_size(int *w, int *h);

#endif /* FWM_MODES_H */
