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

#include "wobble.h"

#include <math.h>
#include <string.h>

/* Pull toward the point's own rest position. A uniform translation of the
 * whole sheet feels only this one — the neighbour springs cancel — so it alone
 * sets how far the window trails at a given drag speed: C*v/K, i.e. 96px at a
 * brisk 1200 px/s. */
#define WOBBLE_K_HOME  200.0   /* 1/s^2 */
/* Damping, as a rate. 0.57 of critical for the whole-sheet mode
 * (C / (2*sqrt(K_HOME))): a visible swing that does not pump itself up. */
#define WOBBLE_C       16.0    /* 1/s */

/* How far the bend reaches, as a fraction of the window. This is the knob that
 * decides whether the thing looks like jelly at all.
 *
 * Under a steady drag the lag grows away from the point you are holding over a
 * distance sqrt(K_EDGE/K_HOME) * (grid spacing) — everything past that trails
 * by the same amount. Set the neighbour springs by eye and that distance came
 * out at 170px on an 800px window: the near corner, the middle and the far
 * corner all lagged 88px (measured), which is a rigid board on one spring with
 * a pucker at the grab point. It has to be comparable to the window itself for
 * the lag to grow visibly across it, so it is derived from this rather than
 * guessed — and because it is a FRACTION, a small window bends like a big one
 * and the constant below comes out independent of both the window and the grid
 * resolution. */
#define WOBBLE_BEND    0.3
#define WOBBLE_K_EDGE  (WOBBLE_K_HOME * (WOBBLE_BEND * (WOBBLE_GRID - 1)) \
                                      * (WOBBLE_BEND * (WOBBLE_GRID - 1)))
/* Damping on the neighbour springs, applied to the difference of their
 * velocities so it touches only the sheet's internal modes and leaves the
 * whole-window swing to WOBBLE_C. Some is not optional: the stiffer the mesh
 * is made to get the bend above, the more lightly damped its short-wavelength
 * modes are, and an undamped one shimmers. This is 0.6 of critical for the
 * shortest wavelength the grid can hold. */
#define WOBBLE_C_EDGE  (0.6 * 33.94)   /* 0.6 * sqrt(K_EDGE), K_EDGE = 1152 */

/* Your hand holds a patch of the jelly, not one molecule of it. Points near
 * the grab get their home spring multiplied by up to this, falling off over
 * WOBBLE_GRIP_SPAN of the window's larger side, so the lag grows smoothly from
 * nothing under the cursor to the full C*v/K_HOME across the window.
 *
 * Pinning the single nearest control point and nothing else was the first
 * version, and it does almost nothing: one pinned point in a two-dimensional
 * elastic sheet is a nail in a rubber mat. Dragged at 1200 px/s, the middle of
 * the window trailed 74px and the far corner 80px (measured) — a rigid board
 * with a pucker at the grab. The gradient IS the effect, and it has to be put
 * there deliberately.
 *
 * These two and WOBBLE_BEND were then picked together off a sweep, against the
 * shape they produce rather than one at a time. Dragged at 1200 px/s by the
 * top-left corner, the lag along the diagonal comes out 0, 18, 54, 73, 78px:
 * the corner in your hand keeps up exactly, the far one trails by a tenth of
 * the window, and it ramps smoothly between. Grabbed dead centre the same
 * sweep gives 31, 13, 0, 13, 31 — symmetric, all four corners trailing. */
#define WOBBLE_GRIP      8.0
#define WOBBLE_GRIP_SPAN 0.25

/* Forward Euler is unstable past dt = 2/omega, and the stiffest mode here is
 * sqrt(8*K_EDGE + K_HOME) = 160 rad/s, i.e. 12ms. server_animate hands out dt
 * up to its 0.25s stall clamp, so the step count is what keeps a dropped frame
 * from detonating the sheet. */
#define WOBBLE_STEP_S  (1.0 / 480.0)
#define WOBBLE_MAX_STEPS 128

/* Rest thresholds: offset in px, speed in px/s. */
#define WOBBLE_REST_PX  0.6
#define WOBBLE_REST_VEL 10.0

static inline int idx(int i, int j) { return j * WOBBLE_GRID + i; }

/* Where point (i, j) sits when the window is at rest. */
static inline double home_x(const Wobble *wb, int i) {
    return (double)wb->w * i / (WOBBLE_GRID - 1);
}
static inline double home_y(const Wobble *wb, int j) {
    return (double)wb->h * j / (WOBBLE_GRID - 1);
}

/* Rebuild the per-point grip multiplier from wherever the hand is. */
static void grip_update(Wobble *wb, double gx, double gy) {
    double span = WOBBLE_GRIP_SPAN * (wb->w > wb->h ? wb->w : wb->h);
    double denom = 2.0 * span * span;
    for (int j = 0; j < WOBBLE_GRID; j++) {
        for (int i = 0; i < WOBBLE_GRID; i++) {
            double dx = home_x(wb, i) - gx, dy = home_y(wb, j) - gy;
            wb->grip[idx(i, j)] = 1.0 + WOBBLE_GRIP * exp(-(dx * dx + dy * dy) / denom);
        }
    }
}

static void grip_clear(Wobble *wb) {
    for (int k = 0; k < WOBBLE_POINTS; k++) wb->grip[k] = 1.0;
}

void wobble_reset(Wobble *wb, int w, int h) {
    memset(wb, 0, sizeof(*wb));
    wb->w = w > 1 ? w : 1;
    wb->h = h > 1 ? h : 1;
    wb->anchor = -1;
    for (int j = 0; j < WOBBLE_GRID; j++) {
        for (int i = 0; i < WOBBLE_GRID; i++) {
            wb->px[idx(i, j)] = home_x(wb, i);
            wb->py[idx(i, j)] = home_y(wb, j);
        }
    }
    grip_clear(wb);
}

void wobble_resize(Wobble *wb, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == wb->w && h == wb->h) return;
    /* Carry the deformation across proportionally. Snapping the points to the
     * new rest grid instead would make a window that is resized mid-wobble
     * jump straight, which is the one moment the effect is meant to smooth. */
    double sx = (double)w / wb->w, sy = (double)h / wb->h;
    for (int k = 0; k < WOBBLE_POINTS; k++) {
        wb->px[k] *= sx;
        wb->py[k] *= sy;
        wb->vx[k] *= sx;
        wb->vy[k] *= sy;
    }
    wb->w = w;
    wb->h = h;
}

void wobble_grab(Wobble *wb, double lx, double ly) {
    /* Nearest control point, found by rounding rather than searched. */
    double fi = lx * (WOBBLE_GRID - 1) / (double)wb->w;
    double fj = ly * (WOBBLE_GRID - 1) / (double)wb->h;
    int i = (int)lround(fi), j = (int)lround(fj);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > WOBBLE_GRID - 1) i = WOBBLE_GRID - 1;
    if (j > WOBBLE_GRID - 1) j = WOBBLE_GRID - 1;
    wb->anchor = idx(i, j);
    grip_update(wb, home_x(wb, i), home_y(wb, j));
}

void wobble_release(Wobble *wb) {
    wb->anchor = -1;
    /* The whole sheet hangs free again, so it settles evenly rather than
     * snapping back around wherever the hand happened to be. */
    grip_clear(wb);
}

void wobble_translate(Wobble *wb, double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) return;
    for (int k = 0; k < WOBBLE_POINTS; k++) {
        wb->px[k] -= dx;
        wb->py[k] -= dy;
    }
}

/* One integration step. Split out so the sub-stepping above reads as the loop
 * it is. */
static void wobble_substep(Wobble *wb, double dt) {
    double fx[WOBBLE_POINTS], fy[WOBBLE_POINTS];

    for (int j = 0; j < WOBBLE_GRID; j++) {
        for (int i = 0; i < WOBBLE_GRID; i++) {
            int k = idx(i, j);
            double hx = home_x(wb, i), hy = home_y(wb, j);

            double kh = WOBBLE_K_HOME * wb->grip[k];
            double ax = kh * (hx - wb->px[k]) - WOBBLE_C * wb->vx[k];
            double ay = kh * (hy - wb->py[k]) - WOBBLE_C * wb->vy[k];

            /* Each neighbour pulls toward where it would be if the two of them
             * were still the rest distance apart — a spring on the DIFFERENCE,
             * so a sheet that has moved as a whole feels nothing from these. */
            static const int di[4] = { -1, 1, 0, 0 };
            static const int dj[4] = { 0, 0, -1, 1 };
            for (int n = 0; n < 4; n++) {
                int ni = i + di[n], nj = j + dj[n];
                if (ni < 0 || nj < 0 || ni >= WOBBLE_GRID || nj >= WOBBLE_GRID) continue;
                int nk = idx(ni, nj);
                double rest_dx = home_x(wb, ni) - hx;
                double rest_dy = home_y(wb, nj) - hy;
                ax += WOBBLE_K_EDGE * ((wb->px[nk] - wb->px[k]) - rest_dx)
                    + WOBBLE_C_EDGE * (wb->vx[nk] - wb->vx[k]);
                ay += WOBBLE_K_EDGE * ((wb->py[nk] - wb->py[k]) - rest_dy)
                    + WOBBLE_C_EDGE * (wb->vy[nk] - wb->vy[k]);
            }

            fx[k] = ax;
            fy[k] = ay;
        }
    }

    for (int k = 0; k < WOBBLE_POINTS; k++) {
        wb->vx[k] += fx[k] * dt;
        wb->vy[k] += fy[k] * dt;
        wb->px[k] += wb->vx[k] * dt;
        wb->py[k] += wb->vy[k] * dt;
    }

    /* The hand outranks the springs: whatever they did to the point right
     * under the cursor, it is where the cursor put it. The grip field above is
     * what carries that out into the rest of the sheet. */
    if (wb->anchor >= 0) {
        int i = wb->anchor % WOBBLE_GRID, j = wb->anchor / WOBBLE_GRID;
        wb->px[wb->anchor] = home_x(wb, i);
        wb->py[wb->anchor] = home_y(wb, j);
        wb->vx[wb->anchor] = 0.0;
        wb->vy[wb->anchor] = 0.0;
    }
}

void wobble_step(Wobble *wb, double dt) {
    if (dt <= 0.0) return;
    int steps = (int)ceil(dt / WOBBLE_STEP_S);
    if (steps < 1) steps = 1;
    if (steps > WOBBLE_MAX_STEPS) steps = WOBBLE_MAX_STEPS;
    double sdt = dt / steps;
    for (int s = 0; s < steps; s++) wobble_substep(wb, sdt);
}

double wobble_max_offset(const Wobble *wb) {
    double worst = 0.0;
    for (int j = 0; j < WOBBLE_GRID; j++) {
        for (int i = 0; i < WOBBLE_GRID; i++) {
            int k = idx(i, j);
            double dx = fabs(wb->px[k] - home_x(wb, i));
            double dy = fabs(wb->py[k] - home_y(wb, j));
            if (dx > worst) worst = dx;
            if (dy > worst) worst = dy;
        }
    }
    return worst;
}

bool wobble_at_rest(const Wobble *wb) {
    if (wobble_max_offset(wb) >= WOBBLE_REST_PX) return false;
    for (int k = 0; k < WOBBLE_POINTS; k++) {
        if (fabs(wb->vx[k]) >= WOBBLE_REST_VEL) return false;
        if (fabs(wb->vy[k]) >= WOBBLE_REST_VEL) return false;
    }
    return true;
}

void wobble_points(const Wobble *wb, float *out, double ox, double oy) {
    for (int k = 0; k < WOBBLE_POINTS; k++) {
        out[k * 2 + 0] = (float)(wb->px[k] + ox);
        out[k * 2 + 1] = (float)(wb->py[k] + oy);
    }
}
