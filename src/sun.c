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

#include "sun.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG (M_PI / 180.0)

/* Degrees above the horizon over which a shadow fades out entirely.
 *
 * Without it the last minute before sunset would switch every shadow on the
 * desktop off between one tick and the next. Dusk is the one moment of the day
 * this is visibly ABOUT, so it is given room: the shadows lengthen to their
 * limit, thin out, and are gone. */
#define SUN_DUSK_DEG 8.0

double sun_hour_local(time_t t) {
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 12.0;
    return tm.tm_hour + tm.tm_min / 60.0 + tm.tm_sec / 3600.0;
}

/* Hermite ramp, 0 below the horizon and 1 once the sun is properly up. */
static double dusk_ramp(double elevation) {
    if (elevation <= 0.0)          return 0.0;
    if (elevation >= SUN_DUSK_DEG) return 1.0;
    double t = elevation / SUN_DUSK_DEG;
    return t * t * (3.0 - 2.0 * t);
}

void sun_light(const SunConfig *cfg, double hour, FwmSunLight *out) {
    memset(out, 0, sizeof(*out));
    if (!cfg || !cfg->enabled) return;

    double az = cfg->azimuth;
    double el = cfg->elevation;

    if (cfg->mode == SUN_MODE_CLOCK) {
        double day = cfg->sunset - cfg->sunrise;
        if (day <= 0.0) return; /* a day with no daylight in it: permanent night */
        double p = (hour - cfg->sunrise) / day;
        if (p <= 0.0 || p >= 1.0) return;  /* before dawn or after dusk */
        az = cfg->dawn_azimuth + p * (cfg->dusk_azimuth - cfg->dawn_azimuth);
        el = cfg->noon_elevation * sin(M_PI * p);
    }

    out->azimuth   = az;
    out->elevation = el;
    if (el <= 0.0) return; /* pointed at the ground by hand: also night */

    /* How far the shadow is thrown. `length` is the length at 45 degrees,
     * where tan is 1, so the reference reads as a plain number of pixels and
     * the low sun stretches it from there. */
    double len = cfg->length * tan((90.0 - el) * DEG);
    if (len < 0.0)              len = 0.0;
    if (len > cfg->length_max)  len = cfg->length_max;

    /* Away from the light: azimuth 0 is a sun over the top of the screen, and
     * a window under it drops its shadow straight down. */
    out->dx = -sin(az * DEG) * len;
    out->dy =  cos(az * DEG) * len;

    out->alpha = cfg->opacity * dusk_ramp(el);
    if (out->alpha < 0.0) out->alpha = 0.0;
    if (out->alpha > 1.0) out->alpha = 1.0;
}

bool sun_light_differs(const FwmSunLight *a, const FwmSunLight *b) {
    return fabs(a->dx - b->dx) > 0.25 ||
           fabs(a->dy - b->dy) > 0.25 ||
           fabs(a->alpha - b->alpha) > 0.004;
}
