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

#ifndef FWM_SHADOW_H
#define FWM_SHADOW_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

#include "config.h"
#include "sun.h"

/* The shadow one window casts, as scene nodes.
 *
 * Drawn as a nine-patch out of ONE small image shared by every window on the
 * desktop: a blurred rectangle is separable and identical whatever it is the
 * shadow of, so the alternative — a screen-sized blurred bitmap per window,
 * redrawn on every resize — buys nothing but megabytes. The image is (4r+1)
 * square for a penumbra of r px; the corners are sampled 1:1 and only the
 * middle row and column are stretched, so nothing about the blur is distorted
 * by the size of the window it belongs to.
 *
 * The nodes live in the window's own scene tree, under everything else in it,
 * which is what makes them move, raise and change desktop with the window for
 * free. The price is that anything flattening that tree into a picture (the
 * spin, the wobble, expo's cards) has to put the shadow out first — the same
 * bargain the focus borders already make, see view_snapshot_into. */
typedef struct FwmShadow FwmShadow;

/* Nine disabled nodes at the bottom of `parent`. NULL only on allocation
 * failure, and every caller must be able to carry on without one. */
FwmShadow *shadow_create(struct wlr_scene_tree *parent);
void shadow_destroy(FwmShadow *shadow);

/* Place the shadow of a `w` x `h` window at the window's own origin. A light
 * with no alpha in it (night, or [sun] off) hides the nodes instead, which is
 * the whole of what night costs. Safe with shadow == NULL. */
void shadow_update(FwmShadow *shadow, int w, int h,
                   const SunConfig *cfg, const FwmSunLight *light);

/* Hide the nodes without forgetting the geometry — for the length of a
 * snapshot, and for a window that must not cast at all (fullscreen). The next
 * shadow_update puts back whatever the light says. Safe with NULL. */
void shadow_set_enabled(FwmShadow *shadow, bool enabled);

/* Is this node — this buffer — one of the shadow's own?
 *
 * Two callers, both of which walk a window's tree and must leave the shadow
 * out of what they are doing: the dim, which would otherwise fade the shadow
 * along with the window and make the light follow the keyboard around, and
 * view_set_content_enabled, which would otherwise switch every cell back on
 * including the ones deliberately left dark. Safe with NULL. */
bool shadow_owns_node(const FwmShadow *shadow, struct wlr_scene_node *node);
bool shadow_owns_buffer(const FwmShadow *shadow, struct wlr_scene_buffer *buf);

/* Release the shared image. Compositor shutdown only; the next shadow_update
 * would simply build it again. */
void shadow_atlas_finish(void);

#endif /* FWM_SHADOW_H */
