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

#ifndef FWM_STATS_MENU_H
#define FWM_STATS_MENU_H

#include <stdbool.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_scene.h>

#include "../stats.h"

/*
 * The menu the stats pill opens: one row per sensor, a switch on each, and the
 * sensor's current value beside it so the row says what it is offering to
 * show.
 *
 * Same chrome as the modes menu (ui/modes.h exports the panel and the switch),
 * different rows: these are DATA, not an enum. The user's config decides how
 * many there are and what they are called, so nothing here can be a fixed list
 * — which is the one structural difference between the two menus and the reason
 * this is a second file rather than a fifth mode row.
 */

/* Open and close are the same animation in opposite directions, exactly as the
 * modes menu's are, and deliberately the same numbers: two menus a pill apart
 * that opened at different speeds would read as two different programs. */
#define STATS_MENU_ANIM_MS 170.0
#define STATS_MENU_RISE_PX  14.0

/* Open under the pill. `screen` is the monitor in layout coordinates and
 * `pill_x` where the pill starts in those same coordinates, so the menu can be
 * clamped to the screen its own strip is drawn on. */
struct wlr_scene_buffer *stats_menu_show(struct wlr_scene_tree *parent,
                                         const struct wlr_box *screen,
                                         double pill_x, double pill_w,
                                         const FwmStats *stats, double opacity);

/* Redraw after a toggle. The switches ease toward the new state on the tick
 * below rather than jumping to it. */
void stats_menu_redraw(struct wlr_scene_buffer *buf, const FwmStats *stats,
                       double opacity);

/* Advance the switches and the row stagger; redraws if anything moved. Returns
 * true while something is still in motion, so the compositor keeps producing
 * frames instead of dropping to its idle heartbeat mid-slide. */
bool stats_menu_tick(struct wlr_scene_buffer *buf, const FwmStats *stats,
                     double opacity, double dt);

/* True while the menu has animation left to run, for server_is_busy. */
bool stats_menu_animating(void);

/* Size of the open menu, so a caller can tell whether a click landed in it
 * before converting coordinates. Depends on the number of sensors, hence the
 * stats handle. */
void stats_menu_size(const FwmStats *stats, int *w, int *h);

/* Hit-test in MENU-BUFFER-LOCAL coordinates. Returns the sensor index, or -1
 * outside any row. */
int stats_menu_hit(const FwmStats *stats, double x, double y);

#endif /* FWM_STATS_MENU_H */
