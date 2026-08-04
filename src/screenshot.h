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

#ifndef FWM_SCREENSHOT_H
#define FWM_SCREENSHOT_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;

/* Screenshots, encoded as PNG and put on the clipboard — no file, no
 * directory to configure, nothing to clean up later. fwm owns the seat's
 * selection, so it offers the bytes itself and any application can paste
 * them; see the clipboard section in screenshot.c for why the handover is
 * not one blocking write.
 *
 * The picture is the monitor's OWN last frame, read back from the buffer it
 * committed — not a re-composite of the scene graph the way src/snapshot.c
 * does it. That distinction is the whole reason this module exists: every
 * cairo overlay (tray, bars, launcher, hints) hands its pixels to the scene
 * and frees its copy, so a scene walk photographs a desktop with no furniture
 * on it at all. The committed frame has everything, exactly as it looked.
 *
 * Which means the capture cannot be synchronous: the request schedules a
 * frame and the save happens on the commit that follows. Nothing the caller
 * has to care about — except that anything meant to stay OUT of the picture
 * (the region selector's own overlay) must be gone before the request, which
 * is what screenshot_region_* below already does. */

/* The whole active monitor, onto the clipboard. Bound to Print by default. */
void screenshot_full(struct FwmServer *server);

/* Drag a rectangle out with the mouse, Escape to cancel. Bound to
 * Super+Shift+S. The selector owns pointer and keyboard while it is up. */
void screenshot_region(struct FwmServer *server);

/* True while the region selector is up. Every input path tests this before
 * doing anything else with the event. */
bool screenshot_selecting(struct FwmServer *server);

/* Input, while the selector is up. Each returns true if it consumed the
 * event, and all three are no-ops returning false when it is not. */
bool screenshot_handle_motion(struct FwmServer *server, double lx, double ly);
bool screenshot_handle_button(struct FwmServer *server, bool pressed);
bool screenshot_handle_key(struct FwmServer *server, xkb_keysym_t sym);

/* Drop the selector, any pending capture and any toast still on screen.
 * Called at teardown; also safe at any time. */
void screenshot_cleanup(struct FwmServer *server);

#endif /* FWM_SCREENSHOT_H */
