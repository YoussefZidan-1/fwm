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

/* Where the sun is and which way that throws a shadow.
 *
 * Every one of these is a thing you would otherwise have to sit in front of
 * the compositor at the right hour of the day to see — half of them only
 * happen at dusk. sun.c reaches no further than config.h's types, so the whole
 * day can be walked through in a loop instead. */

#include "test.h"
#include "sun.h"

static SunConfig base(void) {
    SunConfig c = {0};
    c.enabled        = 1;
    c.mode           = SUN_MODE_MANUAL;
    c.azimuth        = 0.0;
    c.elevation      = 45.0;
    c.sunrise        = 7.0;
    c.sunset         = 21.0;
    c.dawn_azimuth   = -70.0;
    c.dusk_azimuth   = 70.0;
    c.noon_elevation = 65.0;
    c.length         = 14.0;
    c.length_max     = 64.0;
    c.opacity        = 0.45;
    c.blur           = 12.0;
    return c;
}

static void test_off(void) {
    CASE("switched off");
    SunConfig c = base();
    c.enabled = 0;
    FwmSunLight l;
    sun_light(&c, 12.0, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);
    CHECK_DBL(l.dx, 0.0, 1e-12);
    CHECK_DBL(l.dy, 0.0, 1e-12);
}

static void test_straight_down(void) {
    CASE("sun over the top of the screen");
    SunConfig c = base();
    FwmSunLight l;
    sun_light(&c, 12.0, &l);

    /* Azimuth 0 is a light above the desktop, and at 45 degrees the shadow is
     * exactly `length` long — that is what makes the number in the config
     * readable as pixels. */
    CHECK_DBL(l.dx, 0.0, 1e-9);
    CHECK_DBL(l.dy, 14.0, 1e-9);
    CHECK_DBL(l.alpha, 0.45, 1e-9);
}

static void test_azimuth_turns_the_shadow(void) {
    CASE("the shadow falls away from the light");
    SunConfig c = base();
    FwmSunLight l;

    /* Lit from the right: the shadow goes left, and nowhere else. */
    c.azimuth = 90.0;
    sun_light(&c, 0.0, &l);
    CHECK_DBL(l.dx, -14.0, 1e-9);
    CHECK_DBL(l.dy, 0.0, 1e-9);

    /* Lit from the left: the other way, same length. */
    c.azimuth = 270.0;
    sun_light(&c, 0.0, &l);
    CHECK_DBL(l.dx, 14.0, 1e-9);
    CHECK_DBL(l.dy, 0.0, 1e-9);

    /* From below the screen, which is where the light is late in the day. */
    c.azimuth = 180.0;
    sun_light(&c, 0.0, &l);
    CHECK_DBL(l.dx, 0.0, 1e-9);
    CHECK_DBL(l.dy, -14.0, 1e-9);

    /* Anywhere in between, the shadow keeps its length. */
    c.azimuth = 37.0;
    sun_light(&c, 0.0, &l);
    CHECK_DBL(hypot(l.dx, l.dy), 14.0, 1e-9);
}

static void test_low_sun_is_a_long_shadow(void) {
    CASE("elevation sets the length");
    SunConfig c = base();
    FwmSunLight l;

    c.elevation = 70.0;
    sun_light(&c, 0.0, &l);
    double high = hypot(l.dx, l.dy);

    c.elevation = 20.0;
    sun_light(&c, 0.0, &l);
    double low = hypot(l.dx, l.dy);
    CHECK(low > high);

    /* However low it gets, the cap holds — otherwise a sun a whisker above
     * the horizon would throw a shadow the length of the desktop. */
    c.elevation = 0.5;
    sun_light(&c, 0.0, &l);
    CHECK_DBL(hypot(l.dx, l.dy), c.length_max, 1e-9);
}

static void test_night(void) {
    CASE("night");
    SunConfig c = base();
    FwmSunLight l;

    /* By hand: the sun pointed at the ground. */
    c.elevation = -1.0;
    sun_light(&c, 12.0, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);

    /* By the clock: before dawn and after dusk, on both sides of the day. */
    c = base();
    c.mode = SUN_MODE_CLOCK;
    sun_light(&c, 3.0, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);
    sun_light(&c, 23.5, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);
    /* And exactly at the two ends, where the sun is level with the horizon. */
    sun_light(&c, c.sunrise, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);
    sun_light(&c, c.sunset, &l);
    CHECK_DBL(l.alpha, 0.0, 1e-12);

    /* A day that ends before it begins is night the whole way round rather
     * than a division by a negative one. */
    c.sunrise = 20.0;
    c.sunset  = 6.0;
    for (double hour = 0.0; hour < 24.0; hour += 0.5) {
        sun_light(&c, hour, &l);
        CHECK_DBL(l.alpha, 0.0, 1e-12);
    }
}

static void test_the_day_crosses_the_sky(void) {
    CASE("clock mode walks the day");
    SunConfig c = base();
    c.mode = SUN_MODE_CLOCK;
    FwmSunLight l;

    /* Morning: the light is in the east half, so shadows point the other way
     * from where they point in the afternoon. */
    sun_light(&c, 8.0, &l);
    double morning_dx = l.dx;
    CHECK(l.alpha > 0.0);
    sun_light(&c, 20.0, &l);
    CHECK(l.alpha > 0.0);
    CHECK(morning_dx * l.dx < 0.0);

    /* Midday is the top of the arc: the highest sun and the shortest shadow
     * of the day. */
    double noon = (c.sunrise + c.sunset) / 2.0;
    sun_light(&c, noon, &l);
    CHECK_DBL(l.elevation, c.noon_elevation, 1e-9);
    double shortest = hypot(l.dx, l.dy);
    for (double hour = c.sunrise + 0.1; hour < c.sunset; hour += 0.25) {
        FwmSunLight o;
        sun_light(&c, hour, &o);
        CHECK(hypot(o.dx, o.dy) >= shortest - 1e-9);
    }

    /* Nothing anywhere in the day is darker than [sun] opacity, and nothing
     * is below the horizon while the sun is up. */
    for (double hour = 0.0; hour < 24.0; hour += 0.05) {
        sun_light(&c, hour, &l);
        CHECK(l.alpha >= 0.0 && l.alpha <= c.opacity + 1e-12);
    }
}

static void test_dusk_fades_rather_than_switches(void) {
    CASE("dusk");
    SunConfig c = base();
    c.mode = SUN_MODE_CLOCK;

    /* Walking the last hour of the day, the shadows only ever get fainter,
     * and they are gone by the end of it — the one thing that must not happen
     * at sunset is every shadow on the desktop vanishing between two frames. */
    double prev = 1.0;
    int saw_partial = 0;
    for (double hour = c.sunset - 1.0; hour <= c.sunset; hour += 1.0 / 120.0) {
        FwmSunLight l;
        sun_light(&c, hour, &l);
        CHECK(l.alpha <= prev + 1e-9);
        if (l.alpha > 0.0 && l.alpha < c.opacity - 1e-6) saw_partial = 1;
        prev = l.alpha;
    }
    CHECK(saw_partial);
    CHECK_DBL(prev, 0.0, 1e-12);
}

static void test_differs(void) {
    CASE("worth redrawing for");
    SunConfig c = base();
    c.mode = SUN_MODE_CLOCK;

    /* One tick of the clock does not move the sun far enough to be worth
     * moving every shadow on the desktop for. */
    FwmSunLight a, b;
    sun_light(&c, 12.0, &a);
    sun_light(&c, 12.0 + 1.0 / 3600.0, &b);
    CHECK(!sun_light_differs(&a, &b));

    /* An hour of it certainly is. */
    sun_light(&c, 13.0, &b);
    CHECK(sun_light_differs(&a, &b));
}

int main(void) {
    test_off();
    test_straight_down();
    test_azimuth_turns_the_shadow();
    test_low_sun_is_a_long_shadow();
    test_night();
    test_the_day_crosses_the_sky();
    test_dusk_fades_rather_than_switches();
    test_differs();
    return t_report("sun");
}
