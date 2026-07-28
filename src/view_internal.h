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


#ifndef FWM_VIEW_INTERNAL_H
#define FWM_VIEW_INTERNAL_H

#include "view.h"
#include "defines.h"

/* Shared between view.c and view_effects.c.
 *
 * The split is along the one seam a window really has: view.c is what a window
 * IS - mapping, geometry, focus, borders, the two shells it can come from -
 * and view_effects.c is what is DONE to it, which is the squash, the drag
 * wobble and the free rotation. They meet at exactly these few functions: an
 * effect has to be able to hide the live content it stands in front of, and to
 * move the borders around the shape it has deformed. */

/* Put the client's own surfaces (and only those) on or off screen. An effect
 * showing a picture of the window hides them; putting them back is what ends
 * the effect. */
void view_set_content_enabled(FwmView *view, bool enabled);

/* The window's box including its borders, and placing those borders around an
 * arbitrary deformed box. */
void view_border_box(FwmView *view, int *w, int *h);
void view_place_borders(FwmView *view, int x, int y, int w, int h);

/* Composite the window's whole subtree into a buffer of its own size, or into
 * one already allocated for it. See src/snapshot.h for why a client's own
 * buffer will not do. */
struct wlr_buffer *view_snapshot_content(FwmView *view);
bool view_snapshot_into(FwmView *view, struct wlr_buffer *buf);

#endif /* FWM_VIEW_INTERNAL_H */
