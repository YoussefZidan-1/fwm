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

/* The knock a window makes when it hits something. See sound.h for the
 * threading rules — they are the whole design, not a detail.
 *
 * Playback goes through libpulse-simple only, deliberately. PipeWire boxes serve
 * the same API through pipewire-pulse, so this reaches both stacks with one
 * blocking write on one thread; a native pw_stream would add a second event loop
 * and its own callback thread to play a 90ms click. The capture side has to be
 * native PipeWire (it wants the default sink's monitor); this side does not. */
#include "sound.h"
#include "audio.h"
#include "wav.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>

#ifdef HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#define OUT_RATE      44100
#define BLOCK           256   /* frames per write: 5.8ms, so a hit starts promptly */
#define MAX_VOICES        8   /* simultaneous hits; a stack collapsing needs a few */
#define IDLE_CLOSE_S    3.0   /* quiet for this long and the device is handed back */
#define REOPEN_MIN_S    1.0   /* floor on retries after a failed open */
#define MAX_FRAMES  (OUT_RATE * 4)   /* 4s: a collision sound, not a jingle */

/* One playing copy of the sample. `step` is how far along the sample one output
 * frame advances, so it carries both the resample ratio and the pitch. */
struct Voice {
    double pos;
    double step;
    float  gain;
    int    active;
};

struct FwmSound {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    atomic_bool     quit;
    atomic_int      refs;      /* this handle + the thread */

    /* The sample. Written before the thread starts and never again, so the
     * mixer reads it without the lock. */
    float *sample;
    int    frames;
    int    rate;

    /* Guarded by `lock`. */
    struct Voice voices[MAX_VOICES];
    double volume;
};

static void sound_release(FwmSound *s) {
    if (atomic_fetch_sub(&s->refs, 1) != 1) return;
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
    free(s->sample);
    free(s);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── the built-in click ──────────────────────────────────────────────── */

/* Synthesised rather than shipped as a file: a compositor that needs an asset
 * on disk to make a noise is a compositor that makes no noise the moment
 * somebody packages it wrong.
 *
 * Two parts, which is what makes it read as two objects meeting rather than as
 * a beep — a very short noise burst for the contact itself, and a low decaying
 * sine under it for the weight behind it. */
static float *make_click(int *frames_out, int *rate_out) {
    int n = OUT_RATE / 11;              /* ~90ms, most of it the tail */
    float *buf = calloc((size_t)n, sizeof(float));
    if (!buf) return NULL;

    /* Own generator, so the click is bit-identical on every machine and no
     * caller has to have seeded rand(). */
    uint32_t rng = 0x9E3779B9u;
    double peak = 0.0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / OUT_RATE;
        rng = rng * 1664525u + 1013904223u;
        double noise = (double)((rng >> 8) & 0xFFFF) / 32768.0 - 1.0;

        double v = 0.75 * noise * exp(-t * 150.0)          /* the contact */
                 + 0.45 * sin(2.0 * M_PI * 190.0 * t) * exp(-t * 45.0);  /* the body */
        buf[i] = (float)v;
        if (fabs(v) > peak) peak = fabs(v);
    }

    /* Normalised, so the configured volume means the same thing whether the
     * click is this or somebody's own file. */
    if (peak > 0.0) {
        float k = (float)(0.9 / peak);
        for (int i = 0; i < n; i++) buf[i] *= k;
    }

    *frames_out = n;
    *rate_out   = OUT_RATE;
    return buf;
}

/* ── the sample ─────────────────────────────────────────────────────── */

/* The configured file, or NULL with the reason logged. Parsing lives in
 * src/wav.c, away from the threads and the audio backend, because it is the
 * only part of this that reads bytes fwm did not write. */
static float *load_sample(const char *path, int *frames, int *rate) {
    const char *err = NULL;
    float *s = wav_load_mono(path, MAX_FRAMES, frames, rate, &err);
    if (!s) {
        wlr_log(WLR_ERROR, "sound: %s: %s — using the built-in click",
                path, err ? err : "unreadable");
        return NULL;
    }
    wlr_log(WLR_INFO, "sound: loaded %s (%d frames @ %d Hz)", path, *frames, *rate);
    return s;
}

/* ── mixing ──────────────────────────────────────────────────────────── */

static int voices_active(FwmSound *s) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (s->voices[i].active) return 1;
    return 0;
}

/* Fill one block. Returns 1 if anything was playing, so the caller can tell
 * quiet from busy without asking a second time under the lock. */
static int mix_block(FwmSound *s, float *out, int n) {
    memset(out, 0, (size_t)n * sizeof(float));

    pthread_mutex_lock(&s->lock);
    double vol = s->volume;
    int any = 0;
    for (int v = 0; v < MAX_VOICES; v++) {
        struct Voice *vo = &s->voices[v];
        if (!vo->active) continue;
        any = 1;
        for (int i = 0; i < n; i++) {
            if (vo->pos >= (double)(s->frames - 1)) { vo->active = 0; break; }
            int    i0 = (int)vo->pos;
            double fr = vo->pos - (double)i0;
            /* Linear interpolation: at these pitches the error is far below
             * anything audible in a 90ms knock, and it is what keeps a 48kHz
             * file from playing sharp. */
            double smp = (double)s->sample[i0] * (1.0 - fr)
                       + (double)s->sample[i0 + 1] * fr;
            out[i] += (float)(smp * vo->gain * vol);
            vo->pos += vo->step;
        }
    }
    pthread_mutex_unlock(&s->lock);

    /* Eight windows landing in the same millisecond can sum past full scale.
     * Clipped rather than scaled: ducking the whole mix because of one loud
     * frame is the more audible artefact of the two. */
    for (int i = 0; i < n; i++) {
        if (out[i] >  1.0f) out[i] =  1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }
    return any;
}

static void drop_voices(FwmSound *s) {
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MAX_VOICES; i++) s->voices[i].active = 0;
    pthread_mutex_unlock(&s->lock);
}

/* ── playback ────────────────────────────────────────────────────────── */

#ifdef HAVE_PULSE

/* Open the sink, mix until things go quiet, hand it back. Returns false when
 * the device could not be opened, so the caller can back off instead of
 * hammering it once per collision. */
static bool play_session(FwmSound *s) {
    /* Never let libpulse autospawn a daemon: fwm is a window manager, and on a
     * PipeWire box that second server would fight it for the card. Same probe
     * the capture side uses. */
    if (!audio_server_running()) {
        drop_voices(s);
        return false;
    }

    pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32NE, .rate = OUT_RATE, .channels = 1 };
    /* Short target buffer: this is a reaction to something the user just did on
     * screen, so latency is the whole quality of the feature. Four blocks is
     * ~23ms, well inside what reads as immediate and still enough that an
     * ordinary scheduling hiccup does not underrun. */
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength   = BLOCK * 4 * sizeof(float),
        .prebuf    = BLOCK * sizeof(float),
        .minreq    = (uint32_t)-1,
        .fragsize  = (uint32_t)-1,
    };

    int err = 0;
    pa_simple *p = pa_simple_new(NULL, "fwm", PA_STREAM_PLAYBACK, NULL,
                                 "window collisions", &ss, NULL, &attr, &err);
    if (!p) {
        wlr_log(WLR_INFO, "sound: pulse refused the playback stream (%s)", pa_strerror(err));
        drop_voices(s);
        return false;
    }

    float buf[BLOCK];
    double idle = 0.0;
    while (!atomic_load(&s->quit)) {
        int any = mix_block(s, buf, BLOCK);
        /* This write is why nothing joins the mixer thread: it parks until the
         * server has room, which is milliseconds normally and much longer on a
         * device that has just been woken. */
        if (pa_simple_write(p, buf, sizeof(buf), &err) < 0) {
            wlr_log(WLR_INFO, "sound: pulse write failed (%s)", pa_strerror(err));
            break;
        }
        if (any) {
            idle = 0.0;
        } else {
            /* Silence is written for a few seconds after the last hit rather
             * than closing at once: collisions come in bursts, and reopening a
             * stream per bounce is both slow and audible as a click of its
             * own. */
            idle += (double)BLOCK / OUT_RATE;
            if (idle > IDLE_CLOSE_S) break;
        }
    }

    pa_simple_free(p);
    return true;
}

#else  /* no playback backend */

static bool play_session(FwmSound *s) {
    drop_voices(s);
    return false;
}

#endif

static void *sound_thread(void *arg) {
    FwmSound *s = arg;
    double next_try = 0.0;

    while (!atomic_load(&s->quit)) {
        pthread_mutex_lock(&s->lock);
        while (!voices_active(s) && !atomic_load(&s->quit))
            pthread_cond_wait(&s->wake, &s->lock);
        pthread_mutex_unlock(&s->lock);
        if (atomic_load(&s->quit)) break;

        /* A machine with no sound server gets one probe per second at worst,
         * however many windows are bouncing. */
        double t = now_s();
        if (t < next_try) { drop_voices(s); continue; }
        if (!play_session(s)) next_try = t + REOPEN_MIN_S;
    }

    sound_release(s);   /* the compositor may be long gone; last one frees */
    return NULL;
}

/* ── api ─────────────────────────────────────────────────────────────── */

bool sound_supported(void) {
#ifdef HAVE_PULSE
    return true;
#else
    return false;
#endif
}

FwmSound *sound_create(const SoundConfig *cfg) {
    if (!cfg) return NULL;

    FwmSound *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (cfg->path[0]) s->sample = load_sample(cfg->path, &s->frames, &s->rate);
    /* A file that will not load is reported by load_sample and then forgiven:
     * the click keeps the feature working while the user fixes the path. */
    if (!s->sample) s->sample = make_click(&s->frames, &s->rate);
    if (!s->sample) { free(s); return NULL; }

    s->volume = cfg->volume;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    atomic_init(&s->quit, false);
    atomic_init(&s->refs, 2);   /* this handle + the thread */

    if (pthread_create(&s->thread, NULL, sound_thread, s) != 0) {
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->wake);
        free(s->sample);
        free(s);
        return NULL;
    }
    /* Detached from birth: nothing ever joins it, so there is no handle left to
     * leak by not joining. */
    pthread_detach(s->thread);
    return s;
}

void sound_set_config(FwmSound *s, const SoundConfig *cfg) {
    if (!s || !cfg) return;
    pthread_mutex_lock(&s->lock);
    s->volume = cfg->volume;
    pthread_mutex_unlock(&s->lock);
}

void sound_play(FwmSound *s, double gain, double pitch) {
    if (!s || s->frames < 2) return;
    if (!(gain > 0.0)) return;            /* also rejects NaN */
    if (gain > 1.0) gain = 1.0;
    if (!(pitch > 0.25)) pitch = 0.25;
    if (pitch > 4.0) pitch = 4.0;

    pthread_mutex_lock(&s->lock);

    int slot = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Full. Steal from the quietest voice, and only if this hit is louder —
         * a big landing must not be dropped because eight taps got there first,
         * and eight taps must not each cut the landing short. */
        int quietest = 0;
        for (int i = 1; i < MAX_VOICES; i++)
            if (s->voices[i].gain < s->voices[quietest].gain) quietest = i;
        if (s->voices[quietest].gain < (float)gain) slot = quietest;
    }

    if (slot >= 0) {
        s->voices[slot].pos    = 0.0;
        s->voices[slot].step   = (double)s->rate / OUT_RATE * pitch;
        s->voices[slot].gain   = (float)gain;
        s->voices[slot].active = 1;
    }

    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
}

void sound_destroy(FwmSound *s) {
    if (!s) return;

    /* Returns immediately, always — the mixer may be parked in a blocking
     * write, and waiting for it would freeze the whole session for the sake of
     * a feature being switched OFF (src/audio.c learned this the expensive
     * way). The thread is told to stop, woken if it is idle, and ownership is
     * shared: whichever of the two lets go last frees the handle. The caller
     * must drop its pointer here — it may already be freed. */
    atomic_store(&s->quit, true);
    pthread_mutex_lock(&s->lock);
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);

    sound_release(s);
}
