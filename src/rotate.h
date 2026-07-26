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

#ifndef FWM_ROTATE_H
#define FWM_ROTATE_H

#include <stdbool.h>

struct wlr_renderer;
struct wlr_buffer;
struct wlr_texture;

/* The raw-GLES2 escape hatch, for the two deformations wlroots cannot express:
 * an arbitrary rotation and a mesh warp.
 *
 * wlr_render_pass_add_texture places an AXIS-ALIGNED box and its `transform`
 * only covers the eight wl_output transforms — there is no 37-degree option
 * anywhere in the public renderer API, and no way at all to bend a window.
 * So these two drop to raw GLES2 for a handful of textured triangles, on the
 * renderer's own EGL context and through the two wlroots hooks meant for it
 * (wlr_gles2_renderer_get_buffer_fbo, wlr_gles2_texture_get_attribs).
 *
 * Everything else in fwm stays on the public API; when this is unavailable —
 * the Vulkan or pixman renderer, a shader that will not compile — the caller
 * is expected to fall back rather than lose the window (see view_spin_tick,
 * which snaps to the nearest quarter turn instead). */

/* True when rotate_blit and warp_blit can work at all with this renderer.
 * Cheap; safe to call before the first blit. */
bool rotate_supported(struct wlr_renderer *renderer);

/* Draw `src` into `dst`, rotated `angle` radians about the center of `dst` and
 * scaled to src_w x src_h px. `dst` is cleared to transparent first, so it
 * wants to be big enough for the rotated box (a square of the diagonal always
 * is). Angles are y-down, matching PhysicsBody.angle and the rest of fwm's
 * coordinates. Returns false if the rotation could not be drawn. */
bool rotate_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                 struct wlr_texture *src, int src_w, int src_h, double angle);

/* Largest mesh warp_blit will draw, control points per side. */
#define WARP_MAX_GRID 16

/* Draw `src` into `dst` as a `grid` x `grid` lattice of control points, so the
 * picture can bend rather than merely move: this is what the drag wobble is
 * made of. `pts` holds grid*grid (x, y) pairs in DESTINATION pixels, row-major
 * from the top-left, and says where each point of the source's own regular grid
 * ends up — the identity mesh reproduces the source exactly.
 *
 * `dst` is cleared to transparent first and wants margin around the window on
 * every side, since a wobbling lattice overshoots its resting shape. Returns
 * false if the mesh could not be drawn. */
bool warp_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
               struct wlr_texture *src, int grid, const float *pts);

/* Release the shader programs. Call once at teardown, before the renderer is
 * destroyed. */
void rotate_shutdown(struct wlr_renderer *renderer);

#endif /* FWM_ROTATE_H */
