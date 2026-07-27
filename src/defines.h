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

#ifndef FWM_DEFINES_H
#define FWM_DEFINES_H

/* Concurrent windows with a physics body. Slots are recycled when a window
 * closes, so this is a ceiling on what is on screen at once, not on what has
 * been opened over the session. Costs ~256 * (sizeof(PhysicsBody) +
 * sizeof(BodySlot)) of static memory, which is tens of kilobytes. */
#define MAX_WINDOWS             256
/* Virtual desktops on the strip. The world is this many screens wide. */
#define FWM_DESKTOPS            10
#define DRAG_MARGIN             5
#define PHYSICS_MARGIN          3
#define MASS_DENSITY            0.0005
#define FRICTION                0.985
#define THROW_SPEED_MULTIPLIER  0.65
#define MAX_THROW_SPEED         1800.0
#define STOP_SPEED_THRESHOLD    1.0
#define RESTITUTION             0.3
#define PHYSICS_TICK_RATE       60.0
/* Approach speed (px/s) a collision must reach before it counts as an impact
 * worth reacting to. Above resting jitter, below a short drop. */
#define PHYSICS_HIT_MIN_SPEED   120.0
/* Ceiling (px/s) on the momentum a DRAGGED window hands to what it runs into.
 * The mouse can move a window far faster than any throw, and a dragged body is
 * kinematic — infinitely heavy — so an uncapped shove launched the other window
 * onto the next desktop. Kept well above PHYSICS_HIT_MIN_SPEED so a shove still
 * squashes both windows, and below a full throw (MAX_THROW_SPEED *
 * THROW_SPEED_MULTIPLIER) so brushing past never outruns a deliberate fling.
 * Only the momentum is limited: the dragged window itself still tracks the
 * cursor exactly, its position comes from the mirror, not from this. */
#define DRAG_PUSH_MAX_SPEED     600.0
/* Ceiling (px/s) on how fast a visualiser bar may RISE, before [cava] push
 * scales it. A bar is kinematic — infinitely heavy — so whatever speed it has
 * on contact goes straight into the window, and a kick drum moves a bar most of
 * its travel inside one tick: 137px in 1/60s is 8200 px/s, which pinned a
 * window against the ceiling and held it there.
 *
 * A single kick at this speed only lifts v²/2g ≈ 30px, which looks far too
 * timid on paper — but a window spans dozens of bands that peak at different
 * moments, so it gets struck again on the way down and pumped like a ball on a
 * paddle. Real music through this reaches ~90px; the 400 that one kick's worth
 * of arithmetic suggested measured 232px and put the window off the top of the
 * screen. Tune against music, not against the formula.
 *
 * Only the upward direction is capped. A bar dropping away from a window never
 * touches it, so limiting the fall would just make the physical row lag the
 * drawn one for nothing. */
#define BAR_MAX_RISE_SPEED      250.0
#define GRAVITY  981.0   // px/s² (earth ~9.8 m/s² at 100 px/m)

#endif /* FWM_DEFINES_H */