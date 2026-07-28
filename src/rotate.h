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

/* ── the desktop strip: many quads, one pass, with perspective ─────────
 *
 * The third thing wlroots cannot express, and the reason it needs its own
 * entry points rather than another warp: a desktop turned away from the viewer
 * is a PROJECTIVE transform, not a bend. warp_blit interpolates its lattice
 * linearly, which is the classic affine texture swim — straight lines inside a
 * turned card would bow. Here every vertex carries its view depth as a real w,
 * so the rasterizer does the divide and the sampling is perspective-correct.
 *
 * The caller projects; this file rasterises. That is the same division of
 * labour rotate_blit follows — the transform lives in the vertex array, never
 * in a matrix — and it is what lets the strip change shape (a flat band, a
 * drum, a drum seen from above) without a line changing here. */

/* One projected vertex: screen pixels in the destination, its view depth, and
 * the texture coordinate it samples. */
struct scene3d_vert {
    float x, y;   /* projected screen position */
    float w;      /* view depth (> 0; a vertex behind the eye cannot be drawn) */
    float u, v;   /* source texture coordinate, 0..1 */
};

/* Largest fan scene3d_fan will draw — the drum's end caps, one vertex per
 * segment plus the centre. */
#define SCENE3D_MAX_FAN 64

/* Open a pass into `dst`, which is cleared to transparent. Everything drawn
 * until scene3d_end blends over what came before it, in call order — there is
 * no depth buffer (the FBO belongs to wlroots and there is no public way to
 * attach one), so the caller owns the ordering. False when the renderer cannot
 * do this at all, in which case the caller is expected to fall back to a flat
 * strip. */
bool scene3d_begin(struct wlr_renderer *renderer, struct wlr_buffer *dst);

/* A textured quad, premultiplied, scaled by `alpha`. Vertices in the order
 * top-left, bottom-left, top-right, bottom-right — a triangle strip, which is
 * also the winding the caller's own back-face test should assume. */
bool scene3d_quad(struct wlr_texture *tex, const struct scene3d_vert v[4],
                  float alpha);

/* The same shape filled with one premultiplied colour: card edges, the frame
 * around a hovered window, a desktop with no wallpaper behind it. */
bool scene3d_quad_solid(const float rgba[4], const struct scene3d_vert v[4]);

/* A filled fan, `n` vertices from the centre outward — the drum's end caps,
 * which are what stop a ring looked at from above being a view into its own
 * far side. */
bool scene3d_fan_solid(const float rgba[4], const struct scene3d_vert *v, int n);

/* Close the pass and give the EGL context back. */
void scene3d_end(void);

/* Release the shader programs. Call once at teardown, before the renderer is
 * destroyed. */
void rotate_shutdown(struct wlr_renderer *renderer);

#endif /* FWM_ROTATE_H */
