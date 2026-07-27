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

#ifndef FWM_AUDIO_H
#define FWM_AUDIO_H

#include <stdbool.h>

/*
 * Loopback capture of whatever the machine is playing, for the visualiser.
 *
 * EVERY PipeWire call happens on a thread of our own — including the setup.
 * That is not tidiness, it is the whole design constraint: on a machine with no
 * sound server running, `pw_context_new()` DEADLOCKS. Not "fails", not "times
 * out" — it never returns (measured: still stuck after 90s). Called from the
 * compositor thread it takes the event loop with it, and since fwm *is* the
 * display server that means a session that never finishes starting: no input,
 * no IPC, nothing to kill it with but a TTY.
 *
 * So audio_create() only starts a thread and returns. The compositor never
 * waits on PipeWire for anything, and the worst a broken sound stack can do is
 * leave the bars flat.
 *
 * The capture thread downmixes to mono and drops samples into a ring; the
 * compositor thread reads the newest window out of it and does the FFT itself
 * (src/cava.c), which is cheap enough that moving it here would only buy an
 * extra lock and a second copy.
 *
 * PipeWire is also an OPTIONAL build dependency: without it every function here
 * still links and audio_state() is permanently AUDIO_FAILED.
 */
typedef struct FwmAudio FwmAudio;

enum {
    AUDIO_STARTING = 0, /* thread is connecting — may never finish, see above */
    AUDIO_RUNNING,      /* capturing */
    AUDIO_FAILED,       /* no PipeWire, no daemon, or the stream was refused */
};

/* True when fwm was built against PipeWire at all, worth reporting differently
 * ("not built in") from a capture that merely failed to connect. */
bool audio_supported(void);

/* Start the capture thread. Returns immediately — it does NOT wait to find out
 * whether a sound server exists, and the returned handle is valid regardless.
 * NULL only if the thread could not be created at all. Poll audio_state(). */
FwmAudio *audio_create(void);

/* AUDIO_*. Callers should treat AUDIO_STARTING as "no samples yet" and
 * AUDIO_FAILED as "there will never be any". */
int audio_state(FwmAudio *a);

/* Copy the newest `n` mono samples (roughly -1..1) into `out`, oldest first.
 * Returns false and leaves `out` untouched when the ring has not filled yet or
 * nothing has played since the last call — the caller then decays what it
 * already has rather than snapping the bars to zero. */
bool audio_samples(FwmAudio *a, float *out, int n);

/* Sample rate the stream negotiated, for turning FFT bins into Hz. 0 until the
 * format is settled. */
int audio_rate(FwmAudio *a);

/* Stop capturing and free. A thread still stuck in PipeWire setup is DETACHED
 * rather than joined — see the implementation: waiting on it is exactly the
 * deadlock this module exists to avoid. */
void audio_destroy(FwmAudio *a);

#endif /* FWM_AUDIO_H */
