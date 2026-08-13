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

#include "droplet.h"

#include <math.h>

/* How much of the fill a point waits out before it starts moving at all, as a
 * fraction of the whole, scaled by how far that point is from where the drop
 * landed. At 0 every corner would set off together and the fill would be a box
 * being scaled up — which is the animation this exists in order not to be. Too
 * near 1 and the last corner has almost no time left to travel in, so it snaps.
 */
#define FILL_LAG 0.5

/* Surface tension: each point overruns its place and settles back. Small,
 * because this is water finding an edge, not a spring. */
#define FILL_BACK 1.4

/* ... and never more than this many pixels past, whatever the slot's size. The
 * mesh is drawn into a buffer only so much bigger than the window (the caller's
 * margin, at least 48px), and a proportional overshoot in a full-screen slot
 * would leave the far corners outside their own buffer to be hard-clipped. */
#define FILL_OVERSHOOT_MAX 16.0

/* The elliptical grid mapping, on [-1,1]^2: the unit square onto the unit
 * DISC, corners and all. Exact at the corners, which is what does the rounding,
 * and the identity along both axes. */
static void squircle(double u, double v, double *sx, double *sy) {
    *sx = u * sqrt(1.0 - v * v / 2.0);
    *sy = v * sqrt(1.0 - u * u / 2.0);
}

/* The drop's radius for a `w` x `h` window: the circle inscribed in it.
 *
 * One radius for both axes, and that is the whole difference between a drop and
 * a lens — scaling the disc by w/2 across and h/2 down would map it back onto
 * an ellipse of the window's own proportions, so a wide window would round off
 * into a wide blob and never look like a bead of anything. Taking the smaller
 * side keeps the drop inside the window it came from, which is what keeps it
 * inside the buffer that window is drawn into. */
static double drop_radius(double w, double h) {
    return (w < h ? w : h) / 2.0;
}

void droplet_round(float *pts, int grid, double w, double h, double amount) {
    if (!pts || grid < 2 || amount <= 0.0) return;

    double r = drop_radius(w, h);

    for (int j = 0; j < grid; j++) {
        for (int i = 0; i < grid; i++) {
            double u = 2.0 * i / (grid - 1) - 1.0;
            double v = 2.0 * j / (grid - 1) - 1.0;
            double sx, sy;
            squircle(u, v, &sx, &sy);

            /* Where the point rests is u * w/2 from the centre; where the
             * circle wants it is r * sx. The window's own proportions are in
             * the first term only, which is why the difference squeezes the
             * picture along its longer side — a drop holding a wide window
             * holds it compressed, exactly as a real one would. */
            int k = j * grid + i;
            pts[k * 2 + 0] += (float)(amount * (r * sx - u * w / 2.0));
            pts[k * 2 + 1] += (float)(amount * (r * sy - v * h / 2.0));
        }
    }
}

/* The furthest corner sets the scale for every other point's delay, so the
 * whole fill finishes exactly when t does however off-centre the drop landed.
 * Measured, not assumed to be the diagonal: a drop let go in a corner is nearly
 * a slot's diagonal from the opposite one, one let go in the middle is half of
 * it, and both should take the time they were given. */
static double fill_reach(double w, double h, double ox, double oy) {
    double far = 0.0;
    for (int c = 0; c < 4; c++) {
        double cx = (c & 1) ? w : 0.0;
        double cy = (c & 2) ? h : 0.0;
        double d = hypot(cx - ox, cy - oy);
        if (d > far) far = d;
    }
    return far > 1.0 ? far : 1.0;
}

void droplet_fill_begin(DropletFill *f, double slot_w, double slot_h,
                        double drop_w, double drop_h,
                        double ox, double oy, double seconds) {
    if (!f) return;
    if (seconds <= 0.0) seconds = 0.001;

    /* A drop bigger than the slot it is being poured into is just the slot. */
    if (drop_w > slot_w) drop_w = slot_w;
    if (drop_h > slot_h) drop_h = slot_h;

    /* And it lands INSIDE the slot, wherever the cursor was: dropped on the gap
     * between two tiles, or past the edge of one, the hand is outside the slot
     * that wins, and a drop starting outside the window is both a picture drawn
     * outside its own buffer and a spread that begins by moving inward. Pulled
     * to the nearest place the whole drop fits, which for a cursor just off an
     * edge is that edge — where it looks like it was let go. */
    double hw = drop_w / 2.0, hh = drop_h / 2.0;
    if (ox < hw) ox = hw;
    if (ox > slot_w - hw) ox = slot_w - hw;
    if (oy < hh) oy = hh;
    if (oy > slot_h - hh) oy = slot_h - hh;

    f->t = 0.0;
    f->rate = 1.0 / seconds;
    f->ox = ox;
    f->oy = oy;
    f->sw = drop_w;
    f->sh = drop_h;
    f->w = slot_w;
    f->h = slot_h;

    f->reach = fill_reach(slot_w, slot_h, ox, oy);
}

void droplet_fill_retarget(DropletFill *f, double slot_w, double slot_h) {
    if (!f) return;
    f->w = slot_w;
    f->h = slot_h;
    f->reach = fill_reach(slot_w, slot_h, f->ox, f->oy);
}

bool droplet_fill_step(DropletFill *f, double dt) {
    if (!f) return false;
    if (dt > 0.0) f->t += dt * f->rate;
    if (f->t >= 1.0) {
        f->t = 1.0;
        return false;
    }
    return true;
}

/* Overrun and settle. Standard ease-out-back, but with the overshoot measured
 * in pixels by the caller rather than left proportional — see
 * FILL_OVERSHOOT_MAX. */
static double ease_back(double p, double span) {
    double q = p - 1.0;
    double over = FILL_BACK;
    /* How far past its place FILL_BACK would carry a point travelling `span`
     * px. Past the cap, weaken the constant instead of the whole ease, so the
     * arrival keeps its shape and only stops swinging so wide. */
    double peak = 0.0898 * span;   /* max of q^3*(x+1)+q^2*x per unit of x */
    if (peak * over > FILL_OVERSHOOT_MAX && peak > 0.0) {
        over = FILL_OVERSHOOT_MAX / peak;
    }
    return 1.0 + q * q * ((over + 1.0) * q + over);
}

void droplet_fill_points(const DropletFill *f, float *out, int grid,
                         double mx, double my) {
    if (!f || !out || grid < 2) return;

    double r = drop_radius(f->sw, f->sh);

    for (int j = 0; j < grid; j++) {
        for (int i = 0; i < grid; i++) {
            double u = (double)i / (grid - 1);       /* 0..1 across the slot */
            double v = (double)j / (grid - 1);

            /* Where this point ends up: its place in the slot. */
            double rx = u * f->w;
            double ry = v * f->h;

            /* Where it starts: the drop's own box, centred on the cursor. */
            double bx = f->ox + (u - 0.5) * f->sw;
            double by = f->oy + (v - 0.5) * f->sh;

            /* ... rounded off, because that is the shape the hand let go of.
             * Applied here rather than through droplet_round so it can fade
             * out per point as that point arrives: the drop squares itself off
             * as it fills, edge by edge, instead of all at once at the end. */
            double su, sv;
            squircle(2.0 * u - 1.0, 2.0 * v - 1.0, &su, &sv);
            double round_x = r * su - (2.0 * u - 1.0) * f->sw / 2.0;
            double round_y = r * sv - (2.0 * v - 1.0) * f->sh / 2.0;

            /* The wavefront: near points are already moving while far ones have
             * not been reached yet. */
            double d = hypot(rx - f->ox, ry - f->oy) / f->reach;
            double p = (f->t - d * FILL_LAG) / (1.0 - FILL_LAG);
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;

            double e = ease_back(p, hypot(rx - bx, ry - by));

            int k = j * grid + i;
            out[k * 2 + 0] = (float)(mx + bx + (rx - bx) * e + round_x * (1.0 - p));
            out[k * 2 + 1] = (float)(my + by + (ry - by) * e + round_y * (1.0 - p));
        }
    }

    /* The sheet does not tear.
     *
     * Every point is on its own clock — that is what the wavefront IS — and a
     * point near where the drop landed can therefore be most of the way home
     * while its neighbour has not set off. Far enough apart and the near one
     * overtakes: the column order inverts, the quad between them turns inside
     * out, and warp_blit draws the window's own picture mirrored through the
     * fold for a frame or two. Nothing crashes and nothing is logged; it simply
     * looks wrong, which is the hardest kind of wrong to find later.
     *
     * So a point that has been overtaken is carried by whoever overtook it,
     * which is also what the eye expects of a liquid: the leading edge pulls
     * the rest of the drop along behind it rather than sliding through it. */
    for (int j = 0; j < grid; j++) {
        for (int i = 1; i < grid; i++) {
            int k = j * grid + i;
            if (out[k * 2 + 0] < out[(k - 1) * 2 + 0]) out[k * 2 + 0] = out[(k - 1) * 2 + 0];
        }
    }
    for (int i = 0; i < grid; i++) {
        for (int j = 1; j < grid; j++) {
            int k = j * grid + i;
            if (out[k * 2 + 1] < out[(k - grid) * 2 + 1]) out[k * 2 + 1] = out[(k - grid) * 2 + 1];
        }
    }
}
