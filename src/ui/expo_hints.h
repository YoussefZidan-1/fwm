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

#ifndef FWM_EXPO_HINTS_H
#define FWM_EXPO_HINTS_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

/* The strip's own keys, along the bottom of the screen while it is open.
 *
 * Its own panel rather than a line in the `show_hints` cheat-sheet, because
 * these are not binds: they belong to the mode and are not in anyone's config,
 * so nothing else could have found them to list. And on screen rather than in
 * the documentation, because a mode you enter once a day is a mode whose keys
 * you have forgotten by the next time.
 *
 * Two sets: what the strip always does, and — only at the far zoom step, where
 * the camera is allowed off its seat — what flying around it costs. */

/* Create the panel, centred along the bottom. `flight` picks the second set. */
struct wlr_scene_buffer *expo_hints_show(struct wlr_scene_tree *parent,
                                         int screen_w, int screen_h, bool flight);

/* Redraw for a change of zoom step. Cheap and idempotent: it only redraws when
 * the set actually changed. */
void expo_hints_set_flight(struct wlr_scene_buffer *buf, int screen_w,
                           int screen_h, bool flight);

#endif /* FWM_EXPO_HINTS_H */
