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

#ifndef FWM_WAV_H
#define FWM_WAV_H

/*
 * Just enough WAV reading for [sound] path (see src/sound.c).
 *
 * Its own module because it is the one part of the sound feature that reads a
 * file the user points it at — pointer arithmetic over bytes fwm did not write —
 * and keeping it free of wlroots and of the audio backends is what lets
 * tests/test_wav.c hammer it with truncated and malformed headers.
 *
 * Reports failures through a message rather than logging them, for the same
 * reason: the caller knows whether a bad file is worth a log line, a tray
 * warning, or nothing at all.
 */

/* Load `path` as one channel of float samples, downmixed and untouched
 * otherwise — no resampling, so *rate is the file's own.
 *
 * Supports 16-bit PCM and 32-bit float, mono or stereo, which is what sound
 * editors export and what the format is worth supporting of. Returns a
 * malloc'd buffer the caller owns, or NULL with *err pointing at a static
 * description of what was wrong with the file.
 *
 * `max_frames` caps the result (a longer file is truncated, not refused).
 */
float *wav_load_mono(const char *path, int max_frames,
                     int *frames, int *rate, const char **err);

#endif /* FWM_WAV_H */
