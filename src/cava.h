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

#ifndef FWM_CAVA_H
#define FWM_CAVA_H

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>
#include "config.h"

/*
 * The spectrum bars along the bottom of the screen.
 *
 * This module owns the analysis (FFT over the loopback samples from
 * src/audio.h, log-spaced bands, attack/decay smoothing) and the drawing. It
 * does NOT own the physics: it publishes the same levels it draws through
 * cava_levels(), and the tick hands them to physics_set_bars(). That split is
 * what makes `mode = "physical"` mean anything — the bars can push windows
 * around while nothing at all is drawn.
 *
 * The visual is a row of wlr_scene_rects rather than a cairo overlay. A
 * full-width strip redrawn at 60 Hz is ~1.4 MB of ARGB re-uploaded every frame;
 * a rect per bar is a GPU quad whose size is the only thing that changes, and
 * the flat look it forces happens to be the look the tray already has.
 */
typedef struct FwmCava FwmCava;

/* Build the visualiser and open the capture stream. Returns NULL when the mode
 * is off, when fwm was built without PipeWire, or when no audio server is
 * running — all three are ordinary states, and the caller simply has no bars.
 * `parent` should be the background layer: the bars sit above the wallpaper and
 * below the windows, so a window standing on a bar covers its top. */
FwmCava *cava_create(struct wlr_scene_tree *parent, const CavaConfig *cfg,
                     int screen_w, int screen_h);

/* Once per frame: pull the newest samples, redo the spectrum, and move the
 * rects. `cfg` is re-read every call so `fwmctl set cava.*` takes effect live. */
void cava_tick(FwmCava *c, const CavaConfig *cfg, double dt);

/* The current bar levels, 0..1, for physics_set_bars. NULL (and *count 0) when
 * the physical half of the mode is off, so the caller cannot accidentally put
 * bodies in the world for a purely decorative row. */
const float *cava_levels(const FwmCava *c, int *count);

/* True while anything is still moving, so the frame loop keeps running at the
 * tick rate instead of dropping to the idle heartbeat mid-song. */
bool cava_busy(const FwmCava *c);

/* The mode this instance was built for, so the server can notice a config
 * change and rebuild rather than silently keeping the old one. */
int cava_mode(const FwmCava *c);

/* The capture gave up — no sound server, or the stream was refused. The row is
 * built before that is known (audio_create cannot wait; see audio.h), so the
 * caller polls this and tears the visualiser down when it turns true. Otherwise
 * a machine with no audio keeps a row of kinematic bodies in the physics world
 * for a spectrum that will never arrive. */
bool cava_dead(const FwmCava *c);

void cava_destroy(FwmCava *c);

#endif /* FWM_CAVA_H */
