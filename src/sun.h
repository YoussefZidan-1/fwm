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

#ifndef FWM_SUN_H
#define FWM_SUN_H

#include <time.h>

#include "config.h"

/* Where the light is and what that does to a shadow.
 *
 * Everything here is arithmetic over [sun] and the clock — no scene, no
 * wlroots, no compositor — so what the sun DOES can be asserted directly
 * (tests/test_sun.c). Drawing it is shadow.c's job.
 *
 * The offset is the whole shape of the shadow: a window hangs over the
 * wallpaper parallel to it, and a rectangle lit by a distant source and cast
 * onto a parallel plane is the same rectangle, moved. So there is nothing to
 * skew and nothing to project — dx/dy is the answer in full. */
typedef struct FwmSunLight {
    double azimuth;   /* deg clockwise from the top of the screen */
    double elevation; /* deg above the horizon; <= 0 is night */
    double dx, dy;    /* where the shadow falls, px from the window */
    double alpha;     /* 0..1; exactly 0 at night, and nothing is drawn */
} FwmSunLight;

/* Local time as fractional hours (13:30 -> 13.5). */
double sun_hour_local(time_t t);

/* Work out the light for `hour`. In manual mode the hour is ignored and the
 * configured angles are used as they stand. Always writes `out`; a disabled
 * sun, or a sun under the horizon, comes back with alpha 0. */
void sun_light(const SunConfig *cfg, double hour, FwmSunLight *out);

/* Have two lights diverged enough to be worth moving 9 scene nodes per window
 * for? The clock crawls — a whole day of sun is ~0.004 deg per tick — and the
 * answer is almost always no. */
bool sun_light_differs(const FwmSunLight *a, const FwmSunLight *b);

#endif /* FWM_SUN_H */
