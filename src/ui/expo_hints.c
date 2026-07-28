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

#include "expo_hints.h"
#include "../theme.h"
#include "cairo_overlay.h"

#define HINT_PAD_X   16.0
#define HINT_PAD_Y   7.0
#define HINT_GAP     14.0   /* between one hint and the next */
#define HINT_KEY_GAP 6.0    /* between a key and what it does */
#define HINT_CUT     8.0    /* corner chevron, matching the tray */
#define HINT_BOTTOM  22     /* px above the bottom edge of the screen */

/* A key and what it does. Kept short on purpose: a bar along the bottom is
 * read sideways at a glance, and anything that wraps has already failed. */
struct hint { const char *key, *what; };

static const struct hint base[] = {
    { "Esc",         "leave" },
    { "z",           "wider" },
    { "← →",         "pan" },
    { "click",       "go there" },
    { "Super+drag",  "move window" },
    { "right-click", "menu" },
    { "x",           "ring" },
    { NULL, NULL },
};

/* At the far step the camera may leave its seat, and nothing on screen says
 * so — the whole reason this panel exists. */
static const struct hint flight_set[] = {
    { "Esc",              "leave" },
    { "z",                "closer" },
    { "middle / Alt+drag", "fly round" },
    { "wheel",            "in / out" },
    { "↑ ↓",              "tilt" },
    { "x",                "ring" },
    { NULL, NULL },
};

/* One shared state, because one strip means one panel. */
static struct {
    bool flight;
    bool valid;
} g_state;

static PangoLayout *hint_layout(cairo_t *cr, bool bold) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *d =
        pango_font_description_from_string(bold ? "sans bold 9" : "sans 9");
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    return l;
}

static double text_w(PangoLayout *l, const char *s) {
    pango_layout_set_text(l, s, -1);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    (void)h;
    return w;
}

/* Width of the whole bar, measured off-screen with the fonts the draw uses —
 * the panel is only as wide as what it says. */
static double measure(const struct hint *hints, double *height) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(s);
    PangoLayout *key = hint_layout(cr, true);
    PangoLayout *lab = hint_layout(cr, false);

    double w = HINT_PAD_X * 2;
    for (int i = 0; hints[i].key; i++) {
        if (i) w += HINT_GAP;
        w += text_w(key, hints[i].key) + HINT_KEY_GAP + text_w(lab, hints[i].what);
    }
    int th = 0, tw = 0;
    pango_layout_set_text(lab, "Ag", -1);
    pango_layout_get_pixel_size(lab, &tw, &th);
    if (height) *height = th + HINT_PAD_Y * 2;

    g_object_unref(key);
    g_object_unref(lab);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return w;
}

static void panel_path(cairo_t *cr, double w, double h, double cut) {
    cairo_move_to(cr, cut, 0);
    cairo_line_to(cr, w - cut, 0);
    cairo_line_to(cr, w, cut);
    cairo_line_to(cr, w, h - cut);
    cairo_line_to(cr, w - cut, h);
    cairo_line_to(cr, cut, h);
    cairo_line_to(cr, 0, h - cut);
    cairo_line_to(cr, 0, cut);
    cairo_close_path(cr);
}

static void draw_hints(cairo_t *cr, int w, int h, void *data) {
    const struct hint *hints = data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], 0.92);
    panel_path(cr, w, h, HINT_CUT);
    cairo_fill(cr);

    PangoLayout *key = hint_layout(cr, true);
    PangoLayout *lab = hint_layout(cr, false);

    /* Centred inside whatever width the panel was given: the two sets are
     * different lengths and share one buffer, so the row floats rather than
     * the panel resizing under the eye. */
    double row = 0.0;
    for (int i = 0; hints[i].key; i++) {
        if (i) row += HINT_GAP;
        row += text_w(key, hints[i].key) + HINT_KEY_GAP + text_w(lab, hints[i].what);
    }
    double x = (w - row) / 2.0;
    if (x < HINT_PAD_X) x = HINT_PAD_X;

    for (int i = 0; hints[i].key; i++) {
        if (i) x += HINT_GAP;

        /* The key in the accent colour, what it does in the muted one: the eye
         * scans the keys and only stops on the one it wanted. */
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
        pango_layout_set_text(key, hints[i].key, -1);
        cairo_move_to(cr, x, HINT_PAD_Y);
        pango_cairo_show_layout(cr, key);
        x += text_w(key, hints[i].key) + HINT_KEY_GAP;

        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        pango_layout_set_text(lab, hints[i].what, -1);
        cairo_move_to(cr, x, HINT_PAD_Y);
        pango_cairo_show_layout(cr, lab);
        x += text_w(lab, hints[i].what);
    }

    g_object_unref(key);
    g_object_unref(lab);
}

static void place(struct wlr_scene_buffer *buf, int screen_w, int screen_h,
                  double w, double h) {
    wlr_scene_node_set_position(&buf->node, (int)((screen_w - w) / 2.0),
                                (int)(screen_h - h - HINT_BOTTOM));
}

struct wlr_scene_buffer *expo_hints_show(struct wlr_scene_tree *parent,
                                         int screen_w, int screen_h, bool flight) {
    /* Sized for the WIDER of the two sets, once: the panel then never changes
     * shape when the zoom step does, and the row inside it is centred. */
    double h = 0.0, h2 = 0.0;
    double w = measure(base, &h);
    double wf = measure(flight_set, &h2);
    if (wf > w) w = wf;
    if (h2 > h) h = h2;
    const struct hint *hints = flight ? flight_set : base;

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, (int)w, (int)h);
    if (!buf) return NULL;
    cairo_overlay_update(buf, draw_hints, (void *)hints);
    place(buf, screen_w, screen_h, w, h);

    g_state.flight = flight;
    g_state.valid = true;
    return buf;
}

void expo_hints_set_flight(struct wlr_scene_buffer *buf, int screen_w,
                           int screen_h, bool flight) {
    if (!buf || (g_state.valid && g_state.flight == flight)) return;
    g_state.flight = flight;
    g_state.valid = true;

    const struct hint *hints = flight ? flight_set : base;
    cairo_overlay_update(buf, draw_hints, (void *)hints);
    (void)screen_w; (void)screen_h;
}
