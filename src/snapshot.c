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

#include "snapshot.h"
#include "server.h"
#include <math.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>

struct snapshot_ctx {
    struct wlr_render_pass *pass;
    struct wlr_renderer *renderer;
    int origin_x, origin_y;      /* subtree point that lands on the buffer's top-left */
    double scale;
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

    /* Scale the two edges rather than the origin and the size: rounding each
     * corner to the same grid is what keeps a row of scaled buffers from
     * showing a one-pixel seam between them. */
    int x0 = (int)lround((sx - ctx->origin_x) * ctx->scale);
    int y0 = (int)lround((sy - ctx->origin_y) * ctx->scale);
    int x1 = (int)lround((sx + w - ctx->origin_x) * ctx->scale);
    int y1 = (int)lround((sy + h - ctx->origin_y) * ctx->scale);

    wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
        .texture = tex,
        .dst_box = { .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0 },
        .alpha = &scene_buffer->opacity,
        .transform = scene_buffer->transform,
        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    });
    if (!cached) wlr_texture_destroy(tex);   /* only ever destroy our own import */
}

struct wlr_buffer *snapshot_alloc(FwmServer *server, int w, int h) {
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

bool snapshot_subtree(FwmServer *server, struct wlr_buffer *dst,
                      struct wlr_scene_node *node,
                      int origin_x, int origin_y, double scale) {
    if (!server->wlr_renderer || !node || !dst) return false;

    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->wlr_renderer, dst, NULL);
    if (!pass) return false;

    /* Start from transparent: content that does not cover the whole box must
     * not pick up whatever the allocator handed us. */
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .x = 0, .y = 0, .width = dst->width, .height = dst->height },
        .color = { .r = 0, .g = 0, .b = 0, .a = 0 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    struct snapshot_ctx ctx = {
        .pass = pass, .renderer = server->wlr_renderer,
        .origin_x = origin_x, .origin_y = origin_y, .scale = scale,
    };
    wlr_scene_node_for_each_buffer(node, snapshot_add_buffer, &ctx);

    return wlr_render_pass_submit(pass);
}
