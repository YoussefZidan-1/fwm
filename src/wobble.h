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

#ifndef FWM_WOBBLE_H
#define FWM_WOBBLE_H

#include <stdbool.h>

/* The lattice behind the drag wobble — KDE's model, not a scaled rectangle.
 *
 * A window is a grid of control points joined by springs. Each point is also
 * pulled toward where it would sit if the window were at rest, so the sheet
 * always returns to shape. Moving the window drags the grid's rest positions
 * out from under the points, they trail, and the springs between them pass
 * that along: the corner you are holding keeps up with the cursor and the far
 * side arrives late, bending on the way. Nothing here knows about the drag's
 * velocity — the lag is a consequence, which is what makes it swing rather
 * than track.
 *
 * All coordinates are window-local pixels, y-down, origin at the window's
 * top-left. This file is pure arithmetic: no wlroots, no GL, and every input
 * is a number, so it can be (and is) tested on its own.
 */

/* Control points per side. Eight cells across bend smoothly enough that the
 * flat facets do not read; the whole step is a few thousand flops. */
#define WOBBLE_GRID 9
#define WOBBLE_POINTS (WOBBLE_GRID * WOBBLE_GRID)

typedef struct {
    int w, h;                      /* window size the lattice rests at */
    double px[WOBBLE_POINTS];      /* where each point actually is */
    double py[WOBBLE_POINTS];
    double vx[WOBBLE_POINTS];
    double vy[WOBBLE_POINTS];
    /* Per-point multiplier on the pull toward rest: how firmly your hand has
     * hold of that part of the sheet. 1 everywhere when nothing is grabbed. */
    double grip[WOBBLE_POINTS];
    int anchor;                    /* point held exactly on target, -1 for none */
    double limit;                  /* max px from rest; 0 = unbounded */
} Wobble;

/* Put the lattice at rest for a `w` x `h` window. */
void wobble_reset(Wobble *wb, int w, int h);

/* Retarget to a new window size, keeping whatever deformation is in flight
 * (the points are scaled with the box rather than snapped back to it). */
void wobble_resize(Wobble *wb, int w, int h);

/* Hold the control point nearest (lx, ly) exactly on its rest position: this
 * is the window's grab point, the part of the sheet your hand has. Passing a
 * point outside the window is fine, the nearest one is still used.
 * wobble_release lets it go, after which the whole lattice is free. */
void wobble_grab(Wobble *wb, double lx, double ly);
void wobble_release(Wobble *wb);

/* The window moved by (dx, dy) in world pixels. The points keep where they
 * were, so the rest positions have run ahead of them by exactly that much —
 * this is the only thing that ever drives the wobble. */
void wobble_translate(Wobble *wb, double dx, double dy);

/* How far a control point may be pulled from its rest position, in px. 0 lifts
 * the limit.
 *
 * The sheet has no speed limit of its own: the lag under a steady drag is
 * C*v/K_HOME, about 0.085 px per px/s, and nothing in the springs bounds it. But
 * the warped picture is drawn into a buffer only so much bigger than the window,
 * so past a certain hand speed the deformation is not a larger wobble — it is a
 * mesh outside its own buffer, hard-clipped by the edge and, once the offsets
 * pass the grid spacing, folded over itself. Shaken hard it reached seven times
 * the margin.
 *
 * Real jelly has a stretch limit too. Give the caller's margin to the sheet and
 * it saturates instead: the wobble grows with the hand exactly as it did up to
 * the point it would have torn, and simply stops growing after that. */
void wobble_set_limit(Wobble *wb, double px);

/* Advance the springs by `dt` seconds, sub-stepping internally so a long frame
 * slows the wobble down instead of blowing it up. */
void wobble_step(Wobble *wb, double dt);

/* True once the sheet is back in shape and still enough to stop drawing it. */
bool wobble_at_rest(const Wobble *wb);

/* The furthest any control point currently is from its rest position, px.
 * The caller sizes the margin around the window with this. */
double wobble_max_offset(const Wobble *wb);

/* Write the lattice as WOBBLE_POINTS (x, y) pairs, row-major from the
 * top-left, translated by (ox, oy) — the margin of the buffer being drawn
 * into. This is exactly warp_blit's `pts`. */
void wobble_points(const Wobble *wb, float *out, double ox, double oy);

#endif /* FWM_WOBBLE_H */
