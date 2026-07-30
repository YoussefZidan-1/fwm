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

#ifndef FWM_SOUND_H
#define FWM_SOUND_H

#include <stdbool.h>
#include "config.h"

/*
 * The knock a window makes when it hits something ([sound] collisions).
 *
 * One short sample, played as many times over as there are collisions, mixed on
 * a thread of its own. The compositor thread only ever hands over "play this,
 * this loud, this fast" — it never opens, waits for, or writes to an audio
 * device, because every one of those blocks: pa_simple_write parks until the
 * server has room, and a display server that parks stops drawing.
 *
 * Same ownership trick as src/audio.h, and for the same reason: the mixer thread
 * may be inside a blocking write when the feature is switched off, so nothing
 * ever joins it. sound_destroy tells it to stop and drops one reference; the
 * last of the two to let go frees the handle.
 *
 * The device is opened lazily on the first hit and closed again after a few
 * seconds of quiet, so a session where nothing collides holds no stream open —
 * and the sink is free to go to sleep like it would with no clients at all.
 *
 * Playback needs libpulse-simple (pipewire boxes serve it through
 * pipewire-pulse). Without it every function here still links and
 * sound_supported() is false.
 */
typedef struct FwmSound FwmSound;

/* True when fwm was built with a playback backend at all — worth reporting
 * differently ("not built in") from a device that merely would not open. */
bool sound_supported(void);

/* Load the sample and start the mixer thread. Returns immediately and does NOT
 * touch an audio device. NULL if the thread could not be started; a sample that
 * cannot be read is not a failure — the built-in click stands in.
 *
 * `cfg` is copied, not borrowed: a reload frees the config while the thread is
 * still mixing. Live knobs (`volume`) are re-read through sound_set_config. */
FwmSound *sound_create(const SoundConfig *cfg);

/* Pick up new [sound] values without restarting the thread or reloading the
 * sample. A changed `path` is NOT picked up here — that is a rebuild, and
 * server_sound_sync does it by destroying and creating. */
void sound_set_config(FwmSound *s, const SoundConfig *cfg);

/* Play one hit. `gain` is 0..1 (already mapped from impact speed by the
 * caller), `pitch` multiplies playback rate — 1.0 is the sample as recorded,
 * lower is bigger and heavier. Both are clamped. Cheap and non-blocking: it
 * takes a mutex held only for a few writes, so it is safe to call several times
 * in one frame. Hits beyond the voice limit are dropped, quietest first. */
void sound_play(FwmSound *s, double gain, double pitch);

/* Stop and release. Returns at once; see the note above about never joining. */
void sound_destroy(FwmSound *s);

#endif /* FWM_SOUND_H */
