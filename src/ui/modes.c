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

#include "modes.h"
#include "cairo_overlay.h"
#include "tray.h"
#include "../theme.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── icons ───────────────────────────────────────────────────────────── */

/* All four are drawn inside a unit box and scaled, so the same shapes serve the
 * 14px pill and the 16px menu rows without a second set of numbers. */

static void icon_tiling(cairo_t *cr, double x, double y, double s) {
    /* Two columns, the right one split — the BSP split every tiling screenshot
     * opens with, which reads as "tiled" faster than a plain grid does. */
    double g = s * 0.14;          /* gap between panes */
    double w = (s - g) / 2.0;
    cairo_rectangle(cr, x, y, w, s);
    cairo_rectangle(cr, x + w + g, y, w, (s - g) / 2.0);
    cairo_rectangle(cr, x + w + g, y + (s + g) / 2.0, w, (s - g) / 2.0);
    cairo_fill(cr);
}

static void icon_floating(cairo_t *cr, double x, double y, double s) {
    /* Two overlapping outlines: windows that sit wherever they were put. */
    double w = s * 0.62;
    double lw = fmax(1.0, s * 0.09);
    cairo_set_line_width(cr, lw);
    cairo_rectangle(cr, x + lw / 2.0, y + lw / 2.0, w, w);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + s - w - lw / 2.0, y + s - w - lw / 2.0, w, w);
    cairo_stroke(cr);
}

static void icon_gravity(cairo_t *cr, double x, double y, double s) {
    /* A weight falling onto a floor: arrow plus ground line. */
    double cx = x + s / 2.0;
    double lw = fmax(1.0, s * 0.11);
    cairo_set_line_width(cr, lw);
    cairo_move_to(cr, cx, y);
    cairo_line_to(cr, cx, y + s * 0.52);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - s * 0.26, y + s * 0.40);
    cairo_line_to(cr, cx + s * 0.26, y + s * 0.40);
    cairo_line_to(cr, cx, y + s * 0.74);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_rectangle(cr, x, y + s - lw, s, lw);
    cairo_fill(cr);
}

static void icon_cava(cairo_t *cr, double x, double y, double s) {
    /* Four bars of different heights — the feature drawn as itself. */
    static const double h[4] = { 0.45, 1.0, 0.65, 0.85 };
    double g = s * 0.10;
    double w = (s - g * 3.0) / 4.0;
    for (int i = 0; i < 4; i++) {
        double bh = s * h[i];
        cairo_rectangle(cr, x + i * (w + g), y + s - bh, w, bh);
    }
    cairo_fill(cr);
}

/* A ring with a break in it: the desktops laid round, and the seam that closing
 * them shuts. Drawn open because the icon has to mean the THING, and the switch
 * beside it says whether it is on.
 *
 * Weighted like the others — they are filled shapes or thick strokes, and a
 * hairline circle among them reads as a smudge rather than as an icon. The
 * break sits on the right, where the eye lands last, and the caps are round so
 * the two ends look cut rather than frayed. */
static void icon_ring(cairo_t *cr, double x, double y, double s) {
    double lw = fmax(2.0, s * 0.15);
    double r = s / 2.0 - lw / 2.0;
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    /* All the way round bar sixty degrees: the break has to read as a gap in a
     * ring, and anything much wider reads as the letter C. */
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + s / 2.0, y + s / 2.0, r, M_PI * 0.17, M_PI * 1.83);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

void modes_icon(cairo_t *cr, int icon, double x, double y, double size) {
    cairo_save(cr);
    switch (icon) {
    case MODE_ICON_TILING:   icon_tiling(cr, x, y, size);   break;
    case MODE_ICON_FLOATING: icon_floating(cr, x, y, size); break;
    case MODE_ICON_GRAVITY:  icon_gravity(cr, x, y, size);  break;
    case MODE_ICON_CAVA:     icon_cava(cr, x, y, size);     break;
    case MODE_ICON_RING:     icon_ring(cr, x, y, size);     break;
    default: break;
    }
    cairo_restore(cr);
}

/* ── menu geometry ───────────────────────────────────────────────────── */

#define MENU_W        300
#define MENU_PAD      12.0
#define MENU_ROW_H    36.0
#define MENU_CHAMFER  10.0   /* 45-degree corner cut; the tray is pointed, not round */
#define SWITCH_W      36.0
#define SWITCH_H      18.0
#define SEG_W         174.0
#define SEG_H         20.0
#define MENU_H        ((int)(MENU_PAD * 2 + MENU_ROW_H * MODES_ROW_COUNT))

void modes_menu_size(int *w, int *h) {
    if (w) *w = MENU_W;
    if (h) *h = MENU_H;
}

/* Row rect in menu-local coords. */
static double row_y(int row) { return MENU_PAD + MENU_ROW_H * row; }

int modes_menu_hit(double x, double y, int *seg) {
    if (x < 0 || x > MENU_W || y < 0 || y > MENU_H) return MODES_ROW_NONE;
    for (int r = 0; r < MODES_ROW_COUNT; r++) {
        double ry = row_y(r);
        if (y < ry || y >= ry + MENU_ROW_H) continue;
        if (r == MODES_ROW_CAVA) {
            /* The segmented control is the whole point of this row: a click
             * anywhere else on it would have to guess which way to cycle, so it
             * does nothing instead. */
            double sx = MENU_W - MENU_PAD - SEG_W;
            double sy = ry + (MENU_ROW_H - SEG_H) / 2.0;
            if (seg) {
                *seg = -1;
                if (x >= sx && x < sx + SEG_W && y >= sy && y < sy + SEG_H) {
                    int i = (int)((x - sx) / (SEG_W / MODES_CAVA_SEGS));
                    if (i < 0) i = 0;
                    if (i >= MODES_CAVA_SEGS) i = MODES_CAVA_SEGS - 1;
                    *seg = i;
                }
            }
        }
        return r;
    }
    return MODES_ROW_NONE;
}

/* ── animation ───────────────────────────────────────────────────────── */

/*
 * The menu animates its own contents, which cairo_overlay's animation cannot:
 * that one moves and fades the NODE, and a knob sliding inside the buffer is
 * not something a node transform can express.
 *
 * State is file-static rather than per-buffer because there is exactly one
 * modes menu and it is destroyed on close — a struct hung off the buffer would
 * be the same single instance with more indirection.
 */
#define ANIM_SPEED   16.0   /* exponential approach; ~150ms to settle */
#define ANIM_EPS      0.002 /* below this, snap and stop redrawing */

static struct {
    double sw[MODES_ROW_COUNT]; /* 0 = off position, 1 = on */
    double seg;                 /* cava highlight, in segment units */
    double open;                /* 0..1, drives the row stagger */
    int    moving;              /* something is still easing; drives server_is_busy */
    int    live;
} g_anim;

static int cava_seg(int mode);

/* Snap every control to `st` with the menu closed, so opening animates the
 * rows in but NOT the switches — a switch that slid on open would suggest the
 * user had just flipped it. */
static void anim_reset(const ModesState *st) {
    for (int r = 0; r < MODES_ROW_COUNT; r++) g_anim.sw[r] = 0.0;
    g_anim.sw[MODES_ROW_TILING]   = st->tiling   ? 1.0 : 0.0;
    g_anim.sw[MODES_ROW_FLOATING] = st->floating ? 1.0 : 0.0;
    g_anim.sw[MODES_ROW_GRAVITY]  = st->gravity  ? 1.0 : 0.0;
    g_anim.seg  = cava_seg(st->cava);
    g_anim.open   = 0.0;
    g_anim.moving = 1;
    g_anim.live   = 1;
}

bool modes_menu_animating(void) {
    /* Reports the LAST tick's verdict, plus anything a click has just queued.
     * Deriving it here instead would mean re-reading the compositor state from
     * a function that has none — and getting it wrong costs more than a stale
     * frame: server_is_busy drops the loop to a 4Hz heartbeat, and a knob that
     * slides across four frames is not an animation, it is a glitch. */
    return g_anim.live && (g_anim.moving || g_anim.open < 1.0 - ANIM_EPS);
}

/* Reveal progress for one row: rows come in from the top, each a little after
 * the one above it. */
static double row_reveal(int row) {
    double p = (g_anim.open - row * 0.10) / 0.55;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    /* Ease-out cubic: fast in, settling — the shape a menu should have. */
    double inv = 1.0 - p;
    return 1.0 - inv * inv * inv;
}

/* ── drawing ─────────────────────────────────────────────────────────── */

/* Chamfered panel: the corners are cut at 45 degrees rather than rounded,
 * which is the same geometry the chevron-ended islands use — a rounded panel
 * next to a pointed pill reads as two different programs. */
static void panel_path(cairo_t *cr, double x, double y, double w, double h, double c) {
    cairo_new_path(cr);
    cairo_move_to(cr, x + c,     y);
    cairo_line_to(cr, x + w - c, y);
    cairo_line_to(cr, x + w,     y + c);
    cairo_line_to(cr, x + w,     y + h - c);
    cairo_line_to(cr, x + w - c, y + h);
    cairo_line_to(cr, x + c,     y + h);
    cairo_line_to(cr, x,         y + h - c);
    cairo_line_to(cr, x,         y + c);
    cairo_close_path(cr);
}

/* `pos` is the animated 0..1 knob position, not a boolean: the caller has
 * already eased it, and the colours crossfade along the same number so the
 * track and the knob can never disagree about which way the switch is going. */
static void draw_switch(cairo_t *cr, double x, double y, double pos, double alpha) {
    const FwmTheme *thm = theme_get();
    double cut = SWITCH_H / 2.0;
    cairo_new_path(cr);
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + SWITCH_W - cut, y);
    cairo_line_to(cr, x + SWITCH_W, y + SWITCH_H / 2.0);
    cairo_line_to(cr, x + SWITCH_W - cut, y + SWITCH_H);
    cairo_line_to(cr, x + cut, y + SWITCH_H);
    cairo_line_to(cr, x, y + SWITCH_H / 2.0);
    cairo_close_path(cr);

    double off[4] = { thm->dim[0], thm->dim[1], thm->dim[2], alpha * 0.5 };
    double on[4]  = { thm->accent[0], thm->accent[1], thm->accent[2], alpha };
    cairo_set_source_rgba(cr,
        off[0] + (on[0] - off[0]) * pos,
        off[1] + (on[1] - off[1]) * pos,
        off[2] + (on[2] - off[2]) * pos,
        off[3] + (on[3] - off[3]) * pos);
    cairo_fill(cr);

    /* Knob: a diamond, and it turns as it travels. A square would sit in a
     * chevron-ended track looking like it had been dropped in; the same square
     * on its point belongs to the same family as the tray's islands. The
     * quarter turn across the travel is what makes the motion legible at 18px
     * — a shape that only slides that far reads as a flicker. */
    double kw = SWITCH_H * 0.58;
    double x0 = x + cut;
    double x1 = x + SWITCH_W - cut;
    double kx = x0 + (x1 - x0) * pos;
    double ky = y + SWITCH_H / 2.0;

    cairo_save(cr);
    cairo_translate(cr, kx, ky);
    cairo_rotate(cr, M_PI / 4.0 + pos * M_PI / 2.0);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], 1.0);
    cairo_rectangle(cr, -kw / 2.0, -kw / 2.0, kw, kw);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* CAVA_MODE_* -> segment. BOTH and PHYSICAL share the "physical" segment: the
 * menu offers three positions, the config four, and the one it does not offer
 * (push without drawing) still has to light SOMETHING up or the row reads as
 * off while the bars are visibly throwing windows around. */
static int cava_seg(int mode) {
    switch (mode) {
    case 0:  return MODES_CAVA_OFF;
    case 1:  return MODES_CAVA_VISUAL;
    default: return MODES_CAVA_PHYSICAL;
    }
}

static void draw_segments(cairo_t *cr, PangoLayout *layout,
                          double x, double y, int active, double alpha) {
    static const char *label[MODES_CAVA_SEGS] = { "off", "visual", "physical" };
    const FwmTheme *thm = theme_get();
    double sw = SEG_W / MODES_CAVA_SEGS;

    cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], alpha * 0.4);
    cairo_rectangle(cr, x, y, SEG_W, SEG_H);
    cairo_fill(cr);

    /* The highlight slides between positions rather than jumping, so the eye
     * can follow which way the setting moved — the whole reason this is a
     * three-position control and not a label that changes text. */
    double hp = g_anim.live ? g_anim.seg : (double)cava_seg(active);
    if (hp < 0.0) hp = 0.0;
    if (hp > MODES_CAVA_SEGS - 1) hp = MODES_CAVA_SEGS - 1;
    cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], alpha);
    cairo_rectangle(cr, x + hp * sw, y, sw, SEG_H);
    cairo_fill(cr);
    active = cava_seg(active);

    const FwmTheme *t2 = thm;
    cairo_set_source_rgba(cr, t2->pill[0], t2->pill[1], t2->pill[2], alpha * 0.8);
    for (int i = 1; i < MODES_CAVA_SEGS; i++)
        cairo_rectangle(cr, x + i * sw - 0.5, y + 3, 1.0, SEG_H - 6);
    cairo_fill(cr);

    PangoFontDescription *desc = pango_font_description_from_string("sans 7");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    for (int i = 0; i < MODES_CAVA_SEGS; i++) {
        int tw, th;
        pango_layout_set_text(layout, label[i], -1);
        pango_layout_get_pixel_size(layout, &tw, &th);

        /* Colour by how much of the highlight is actually UNDER this label, not
         * by which segment is selected. Keyed to the target index instead, a
         * label went dark-on-dark the moment the highlight left it and stayed
         * that way for the whole slide — invisible for exactly the frames the
         * animation exists to show. */
        double over = 1.0 - fabs(hp - (double)i);
        if (over < 0.0) over = 0.0;
        if (over > 1.0) over = 1.0;
        cairo_set_source_rgb(cr,
            thm->muted[0] + (thm->pill[0] - thm->muted[0]) * over,
            thm->muted[1] + (thm->pill[1] - thm->muted[1]) * over,
            thm->muted[2] + (thm->pill[2] - thm->muted[2]) * over);
        cairo_move_to(cr, x + i * sw + (sw - tw) / 2.0, y + (SEG_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);
    }
    (void)active;

    desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

struct MenuCtx { ModesState st; };

static void draw_menu(cairo_t *cr, int w, int h, void *user) {
    struct MenuCtx *ctx = user;
    const ModesState *st = &ctx->st;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], st->opacity);
    panel_path(cr, 0, 0, w, h, MENU_CHAMFER);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    static const char *name[MODES_ROW_COUNT] = {
        "Tiling", "Floating", "Gravity", "Cava", "Ring",
    };
    static const int   icon[MODES_ROW_COUNT] = {
        MODE_ICON_TILING, MODE_ICON_FLOATING, MODE_ICON_GRAVITY, MODE_ICON_CAVA,
        MODE_ICON_RING,
    };
    const int on[MODES_ROW_COUNT] = {
        st->tiling, st->floating, st->gravity, st->cava != 0, st->ring,
    };

    for (int r = 0; r < MODES_ROW_COUNT; r++) {
        double ry = row_y(r);
        double icon_s = 16.0;
        double iy = ry + (MENU_ROW_H - icon_s) / 2.0;

        /* Each row slides the last few px into place and fades as it lands, a
         * little after the row above it. Drawn with a group so the whole row —
         * icon, label and control — shares one alpha rather than each element
         * fading on its own schedule. */
        double rev = g_anim.live ? row_reveal(r) : 1.0;
        if (rev <= 0.001) continue;
        cairo_push_group(cr);
        cairo_save(cr);
        cairo_translate(cr, (1.0 - rev) * 14.0, 0.0);

        double lit = on[r] ? rev : 0.0;
        cairo_set_source_rgb(cr,
            thm->dim[0] + (thm->accent[0] - thm->dim[0]) * lit,
            thm->dim[1] + (thm->accent[1] - thm->dim[1]) * lit,
            thm->dim[2] + (thm->accent[2] - thm->dim[2]) * lit);
        modes_icon(cr, icon[r], MENU_PAD, iy, icon_s);

        int tw, th;
        pango_layout_set_text(layout, name[r], -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        cairo_move_to(cr, MENU_PAD + icon_s + 12.0, ry + (MENU_ROW_H - th) / 2.0);
        pango_cairo_show_layout(cr, layout);

        if (r == MODES_ROW_CAVA) {
            draw_segments(cr, layout, MENU_W - MENU_PAD - SEG_W,
                          ry + (MENU_ROW_H - SEG_H) / 2.0, st->cava, st->opacity);
        } else {
            draw_switch(cr, MENU_W - MENU_PAD - SWITCH_W,
                        ry + (MENU_ROW_H - SWITCH_H) / 2.0,
                        g_anim.live ? g_anim.sw[r] : (on[r] ? 1.0 : 0.0), st->opacity);
        }

        /* CTM back to what push_group saw BEFORE popping: the group pattern is
         * matrixed against that, so popping under the row's own slide would
         * apply the offset a second time. */
        cairo_restore(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, rev);
    }

    g_object_unref(layout);
}

struct wlr_scene_buffer *modes_menu_show(struct wlr_scene_tree *parent,
                                         const struct wlr_box *screen,
                                         double pill_x, const ModesState *st) {
    struct MenuCtx ctx = { .st = *st };

    /* Right-aligned with the pill, so the menu reads as hanging off it rather
     * than as a panel that happened to appear. Clamped to ITS OWN monitor for
     * the narrow case where the pill sits close to the right edge — the whole
     * arithmetic is in layout coordinates, so the clamp cannot pull the menu
     * onto a neighbouring screen. */
    int wx = (int)lround(pill_x + MODES_PILL_W - MENU_W);
    if (wx + MENU_W > screen->x + screen->width - 8)
        wx = screen->x + screen->width - 8 - MENU_W;
    if (wx < screen->x + 8) wx = screen->x + 8;
    int wy = screen->y + TRAY_BOTTOM + 6;

    anim_reset(st);

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, MENU_W, MENU_H);
    if (buf) {
        wlr_scene_node_set_position(&buf->node, wx, wy);
        cairo_overlay_update(buf, draw_menu, &ctx);
        /* Node-level fade and drop, on top of the per-row stagger the tick
         * draws inside the buffer: the panel arrives, then its contents do.
         * server_close_modes_menu runs exactly this in reverse. */
        cairo_overlay_animate_in(buf, MODES_MENU_ANIM_MS, -MODES_MENU_RISE_PX);
    }
    return buf;
}

void modes_menu_redraw(struct wlr_scene_buffer *buf, const ModesState *st) {
    if (!buf) return;
    g_anim.moving = 1;
    struct MenuCtx ctx = { .st = *st };
    cairo_overlay_update(buf, draw_menu, &ctx);
}

bool modes_menu_tick(struct wlr_scene_buffer *buf, const ModesState *st, double dt) {
    if (!buf || !st || !g_anim.live) return false;

    double target[MODES_ROW_COUNT] = {
        st->tiling ? 1.0 : 0.0,
        st->floating ? 1.0 : 0.0,
        st->gravity ? 1.0 : 0.0,
        0.0, /* the cava row has no switch; its highlight is animated below */
        st->ring ? 1.0 : 0.0,
    };
    double seg_target = cava_seg(st->cava);

    /* Clamp the step before easing with it.
     *
     * The frame loop idles at TICK_IDLE_MS (200ms) when nothing is moving,
     * which is exactly the state the menu is in right before someone clicks a
     * switch. The first tick after that click carries the whole 200ms gap, and
     * 1 - exp(-16 * 0.2) is 0.96: the knob covered 96% of its travel in one
     * frame and the "animation" was three barely-visible frames of the last 4%.
     * Waking the loop sooner does not help — the gap is measured from the
     * PREVIOUS tick, not from the click.
     *
     * Capped at ONE 60Hz frame, not merely at something small: any larger and
     * the first frame after idle is still visibly bigger than the rest, which
     * reads as the knob jumping and then easing. The cost is that a genuinely
     * slow frame advances the animation less than wall-clock says it should —
     * for a 150ms UI transition that is invisible, and it is the right way
     * round: better slightly slow than skipped. */
    if (dt > 1.0 / 60.0) dt = 1.0 / 60.0;

    /* Exponential approach, so the settle looks the same whether the frame took
     * 16ms or 33ms — the same form the camera slide and the tile glide use. */
    double k = 1.0 - exp(-ANIM_SPEED * dt);
    int moving = 0;

    for (int r = 0; r < MODES_ROW_COUNT; r++) {
        if (r == MODES_ROW_CAVA) continue;
        double d = target[r] - g_anim.sw[r];
        if (fabs(d) < ANIM_EPS) { g_anim.sw[r] = target[r]; continue; }
        g_anim.sw[r] += d * k;
        moving = 1;
    }

    double ds = seg_target - g_anim.seg;
    if (fabs(ds) < ANIM_EPS) g_anim.seg = seg_target;
    else { g_anim.seg += ds * k; moving = 1; }

    if (g_anim.open < 1.0 - ANIM_EPS) {
        /* Linear, not eased: row_reveal puts the easing on each row, and easing
         * the clock as well would stack two curves and stall the stagger. */
        g_anim.open += dt * 3.6;
        if (g_anim.open > 1.0) g_anim.open = 1.0;
        moving = 1;
    }

    g_anim.moving = moving;
    if (!moving) return false;
    modes_menu_redraw(buf, st);
    return true;
}
