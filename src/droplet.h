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

#ifndef FWM_DROPLET_H
#define FWM_DROPLET_H

#include <stdbool.h>

/* The shape a window takes when it is carried off a tiling layout, and the way
 * it becomes a window again when it is put back into one.
 *
 * Taking a tile out of the tree used to hand you the tile — full height, the
 * width of whatever column it sat in, an awkward thing to carry. It comes off
 * at one size now whatever it was ([tiling] pickup), and with this effect on it
 * comes off as a DROP: the same picture pulled round at the corners, wobbling
 * on the drag springs it already had. Put down, the drop lands where the cursor
 * let go and spreads out to the edges of its new slot, the near corners
 * arriving before the far ones.
 *
 * Two pieces of arithmetic and nothing else — no wlroots, no scene graph, no
 * state the compositor owns — so this can be, and is, tested on its own. Both
 * work on the lattice warp_blit and wobble.h already share: grid*grid (x, y)
 * pairs in DESTINATION pixels, row-major from the top-left, y-down. That is
 * what lets the wobble and the rounding compose by simply running one after the
 * other over a single array of points.
 */

/* Pull a lattice that currently holds a `w` x `h` rectangle toward a squircle,
 * in place.
 *
 * `amount` is 0 for the rectangle the window really is, and 1 for the roundest
 * this goes: a true circle, the one inscribed in the window, with the picture
 * squeezed along its longer side to fit. A circle and not an ellipse of the
 * window's own proportions — a wide window rounding off into a wide blob never
 * reads as a bead of anything. In between is the straight blend, so easing
 * `amount` up from 0 is a window rounding off into a drop.
 *
 * The displacement is worked out from where each point WOULD rest and added to
 * where it actually is. A lattice caught mid-wobble therefore keeps every bit
 * of its wobble and merely does it in a rounder shape — which is the whole
 * reason the drop is this and not a second spring system fighting the first. */
void droplet_round(float *pts, int grid, double w, double h, double amount);

/* The drop spreading out into the slot it was put down in.
 *
 * Every field is set by droplet_fill_begin; the caller only ever reads `t` (to
 * know how far along it is) and hands the struct back to the other two calls. */
typedef struct {
    double t;        /* 0 where it landed, 1 when the slot is full */
    double rate;     /* 1/s — the reciprocal of how long the fill takes */
    double ox, oy;   /* where the cursor let go, slot-local px */
    double sw, sh;   /* the drop's own size at the moment it landed */
    double w, h;     /* the slot being filled */
    double reach;    /* distance from (ox, oy) to the furthest corner, px */
} DropletFill;

/* Arm a fill: a `drop_w` x `drop_h` drop let go at (ox, oy) — slot-local
 * pixels, and outside the slot is fine — spreading into a `slot_w` x `slot_h`
 * slot over `seconds`. */
void droplet_fill_begin(DropletFill *f, double slot_w, double slot_h,
                        double drop_w, double drop_h,
                        double ox, double oy, double seconds);

/* Advance the fill by `dt` seconds. False once the slot is full, which is the
 * caller's cue to stop drawing the mesh and give the real window back. */
bool droplet_fill_step(DropletFill *f, double dt);

/* The slot turned out to be a different size — a client that committed
 * something other than what its tile asked for, which terminals do routinely.
 * Keeps the fill's progress and simply re-aims it, so the picture ends up flush
 * with the window instead of a character cell short of it. */
void droplet_fill_retarget(DropletFill *f, double slot_w, double slot_h);

/* Write the lattice as it stands — grid*grid (x, y) pairs, translated by
 * (mx, my) for the margin of the buffer being drawn into. This is exactly
 * warp_blit's `pts`, and exactly what wobble_points writes. */
void droplet_fill_points(const DropletFill *f, float *out, int grid,
                         double mx, double my);

#endif /* FWM_DROPLET_H */
