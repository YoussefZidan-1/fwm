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

#ifndef FWM_EXPO_H
#define FWM_EXPO_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;
struct FwmView;

typedef struct FwmExpo FwmExpo;

/* The desktop strip: the camera pulls back until several desktops are on
 * screen at once, and windows can be found, moved between desktops and
 * switched to from there.
 *
 * It is cheap for one reason: fwm's ten desktops ALREADY are a continuous
 * horizontal strip in world coordinates (server->camera_x, and a window's x
 * runs across all ten). Pulling back is a scale on an axis that exists, not a
 * cube that has to be built. What is on screen while it runs is a set of
 * snapshots — the live scene is hidden underneath and the simulation is
 * frozen, so nothing walks away while you are looking at it. */

bool expo_active(struct FwmServer *server);

/* Enter, or start leaving. Leaving lands on whatever desktop the strip is
 * looking at. Bound to the `expo` action. */
void expo_toggle(struct FwmServer *server);

/* Second zoom step of the SAME mode (the `z` key while it is open): the whole
 * strip at once, and back. */
void expo_zoom_step(struct FwmServer *server);

/* Frame-time animation. Called from server_animate; a no-op when closed. */
void expo_tick(struct FwmServer *server, double dt);

/* Where the strip is looking, as a fractional desktop index (false when it is
 * closed), and that rounded to a desktop (-1 when closed). The tray's marker
 * reads these so it keeps reporting where you are while the strip pans, rather
 * than freezing on the desktop it was entered from. */
bool expo_view_position(struct FwmServer *server, double *pos);
int expo_view_desktop(struct FwmServer *server);

/* Send the strip to a desktop, eased, without touching the live camera. False
 * when the strip is not up, so a caller can fall through to its own way of
 * getting there — this is what makes the `view:` binds, the tray's desktop
 * island and its scroll wheel keep meaning "go to desktop N" while the strip
 * is open, instead of moving a world nobody is looking at. */
bool expo_goto_desktop(struct FwmServer *server, int d);

/* True while the strip is still moving, so the tick keeps the frame loop at
 * full rate instead of dropping to the idle heartbeat. */
bool expo_animating(struct FwmServer *server);

/* Input, while the strip owns it. Each returns true when the event was
 * consumed and must not reach a client. Coordinates are output-local pixels. */
bool expo_handle_key(struct FwmServer *server, xkb_keysym_t sym);
bool expo_handle_motion(struct FwmServer *server, double lx, double ly);
bool expo_handle_button(struct FwmServer *server, uint32_t button, bool pressed,
                        double lx, double ly);
bool expo_handle_axis(struct FwmServer *server, double delta);

/* A window that is going away must not be left with a card standing in for it.
 * Called from the unmap and destroy paths. */
void expo_forget_view(struct FwmServer *server, struct FwmView *view);

/* Tear the strip down without animating, at shutdown or when the outputs
 * change under it. */
void expo_destroy(struct FwmServer *server);

#endif /* FWM_EXPO_H */
