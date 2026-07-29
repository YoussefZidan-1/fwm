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

/* S_ISSOCK is hidden by -std=c11 without this. The full build happens to get it
 * anyway through PipeWire's headers, so a pulse-only build was the one that
 * broke — same idiom as src/ipc.c. */
#define _GNU_SOURCE

#include "audio.h"

#include <stddef.h>  /* NULL — the stub branch below needs it on its own */

#if !defined(HAVE_PIPEWIRE) && !defined(HAVE_PULSE)

/* Built without any capture backend. Everything still links; the visualiser
 * sees an audio source that never produces anything, which is the same path a
 * machine with the libraries but no sound server takes. */

bool audio_supported(void) { return false; }
bool audio_server_running(void) { return false; }
FwmAudio *audio_create(void) { return NULL; }
int audio_state(FwmAudio *a) { (void)a; return AUDIO_FAILED; }
bool audio_samples(FwmAudio *a, float *out, int n) { (void)a; (void)out; (void)n; return false; }
int audio_rate(FwmAudio *a) { (void)a; return 0; }
void audio_destroy(FwmAudio *a) { (void)a; }

#else /* a backend is available */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wlr/util/log.h>

#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#ifdef HAVE_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

/* The visualiser only ever looks at the newest ~2048 samples. Sized at four
 * times that so a late tick (a stalled frame, a VT switch) still finds a whole
 * window intact instead of one the capture thread has half-overwritten. */
#define RING_SAMPLES 8192

struct FwmAudio {
    pthread_t       thread;
    atomic_int      state;      /* AUDIO_* */
    atomic_bool     quit;

    /* Two owners — the compositor and the capture thread — and whichever lets
     * go last frees. This is what lets audio_destroy return instantly instead
     * of waiting for a blocking read to come back. See audio_destroy. */
    atomic_int      refs;

    /* Guards main_loop only, so the compositor can signal a running PipeWire
     * loop without racing the thread that is tearing it down. */
    pthread_mutex_t ctl;
#ifdef HAVE_PIPEWIRE
    struct pw_main_loop *main_loop;
    struct pw_stream    *stream;
#endif

    /* Guards ring/write/filled/wrapped only. Held for one memcpy on either
     * side — the capture callback runs on the sound server's RT thread, and
     * anything longer than that here would be a dropout. */
    pthread_mutex_t lock;
    float    ring[RING_SAMPLES];
    unsigned write;
    unsigned filled;
    bool     wrapped;

    atomic_int channels;
    atomic_int rate;
};

/* Drop one reference; the second caller to arrive does the freeing. */
static void audio_release(struct FwmAudio *a) {
    if (atomic_fetch_sub(&a->refs, 1) != 1) return;
    pthread_mutex_destroy(&a->lock);
    pthread_mutex_destroy(&a->ctl);
    free(a);
}

/* Append mono samples to the ring. Shared by both backends. */
static void ring_push(struct FwmAudio *a, const float *src, unsigned frames, int ch) {
    if (ch < 1) return;
    pthread_mutex_lock(&a->lock);
    for (unsigned i = 0; i < frames; i++) {
        /* Downmix: one row of bars has nowhere to put stereo separation. */
        float sum = 0.0f;
        for (int c = 0; c < ch; c++) sum += src[i * (unsigned)ch + (unsigned)c];
        a->ring[a->write] = sum / (float)ch;
        a->write = (a->write + 1) % RING_SAMPLES;
        if (a->write == 0) a->wrapped = true;
        if (a->filled < RING_SAMPLES) a->filled++;
    }
    pthread_mutex_unlock(&a->lock);
}

/* ── pipewire backend ────────────────────────────────────────────────── */

#ifdef HAVE_PIPEWIRE

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    struct FwmAudio *a = userdata;
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t media_type, media_subtype;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) return;
    if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw) return;

    struct spa_audio_info_raw info = {0};
    if (spa_format_audio_raw_parse(param, &info) < 0) return;

    atomic_store(&a->channels, (int)info.channels);
    atomic_store(&a->rate, (int)info.rate);
    atomic_store(&a->state, AUDIO_RUNNING);
    wlr_log(WLR_INFO, "audio: capturing %d ch @ %d Hz (pipewire)",
            (int)info.channels, (int)info.rate);
}

static void on_process(void *userdata) {
    struct FwmAudio *a = userdata;

    struct pw_buffer *b = pw_stream_dequeue_buffer(a->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    const float *src = buf->datas[0].data;
    if (!src) { pw_stream_queue_buffer(a->stream, b); return; }

    int ch = atomic_load(&a->channels);
    if (ch < 1) ch = 1;
    /* stride, not sizeof(float): the chunk covers every channel interleaved,
     * and dividing by the wrong one silently doubles the apparent tempo. */
    uint32_t stride = buf->datas[0].chunk->stride;
    if (stride == 0) stride = sizeof(float) * (uint32_t)ch;
    ring_push(a, src, buf->datas[0].chunk->size / stride, ch);

    pw_stream_queue_buffer(a->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
};

/* pw_context_new() — reached through pw_stream_new_simple() below — DEADLOCKS
 * when no sound server is running: measured stuck at 90s with no progress, not
 * an error return and not a timeout. That is why nothing here is ever called
 * from the compositor thread, and why socket_exists() checks first. */
static void run_pipewire(struct FwmAudio *a) {
    pw_init(NULL, NULL);

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop) return;
    pthread_mutex_lock(&a->ctl);
    a->main_loop = loop;
    pthread_mutex_unlock(&a->ctl);

    /* STREAM_CAPTURE_SINK is the whole trick: it makes this an input stream fed
     * by the default OUTPUT's monitor, i.e. what the speakers are playing,
     * rather than the microphone. Without it the bars follow the room. */
    a->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(loop),
        "fwm visualiser",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,          "Audio",
            PW_KEY_MEDIA_CATEGORY,      "Capture",
            PW_KEY_MEDIA_ROLE,          "Music",
            PW_KEY_STREAM_CAPTURE_SINK, "true",
            /* A short quantum keeps the bars in step with what is heard; the
             * ring above absorbs the jitter that comes with asking for one. */
            PW_KEY_NODE_LATENCY,        "1024/44100",
            NULL),
        &stream_events, a);
    if (!a->stream) goto out;

    {
        uint8_t pod_buf[1024];
        struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
        struct spa_audio_info_raw info = {
            .format = SPA_AUDIO_FORMAT_F32, .rate = 44100, .channels = 2,
        };
        const struct spa_pod *params[1] = {
            spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info),
        };
        int rc = pw_stream_connect(a->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                   PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS,
                                   params, 1);
        if (rc < 0) {
            wlr_log(WLR_INFO, "audio: pipewire refused the capture stream (%d)", rc);
            goto out;
        }
    }

    if (!atomic_load(&a->quit)) pw_main_loop_run(loop);

out:
    if (a->stream) { pw_stream_destroy(a->stream); a->stream = NULL; }
    pthread_mutex_lock(&a->ctl);
    a->main_loop = NULL;
    pthread_mutex_unlock(&a->ctl);
    pw_main_loop_destroy(loop);
}

#endif /* HAVE_PIPEWIRE */

/* ── pulseaudio backend ──────────────────────────────────────────────── */

#ifdef HAVE_PULSE

/* Simpler than PipeWire in every way that matters here: one blocking read on a
 * thread that exists for it. `@DEFAULT_MONITOR@` is libpulse's own name for
 * "the monitor of whatever the default sink is", so this follows the user
 * changing output devices with no introspection round-trip to write. */
static void run_pulse(struct FwmAudio *a) {
    pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32NE, .rate = 44100, .channels = 2 };
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .fragsize  = 1024 * sizeof(float) * 2,
        .tlength   = (uint32_t)-1,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
    };

    int err = 0;
    pa_simple *s = pa_simple_new(NULL, "fwm", PA_STREAM_RECORD,
                                 "@DEFAULT_MONITOR@", "visualiser",
                                 &ss, NULL, &attr, &err);
    if (!s) {
        wlr_log(WLR_INFO, "audio: pulse refused the capture stream (%s)", pa_strerror(err));
        return;
    }

    atomic_store(&a->channels, 2);
    atomic_store(&a->rate, 44100);
    atomic_store(&a->state, AUDIO_RUNNING);
    wlr_log(WLR_INFO, "audio: capturing 2 ch @ 44100 Hz (pulse)");

    float buf[1024 * 2];
    while (!atomic_load(&a->quit)) {
        /* This read is precisely why nothing joins this thread: it returns when
         * the monitor has a period ready, which on an idle or heavily buffered
         * sink is hundreds of milliseconds after being asked to stop. */
        if (pa_simple_read(s, buf, sizeof(buf), &err) < 0) {
            wlr_log(WLR_INFO, "audio: pulse read failed (%s)", pa_strerror(err));
            break;
        }
        ring_push(a, buf, sizeof(buf) / sizeof(buf[0]) / 2, 2);
    }

    pa_simple_free(s);
}

#endif /* HAVE_PULSE */

/* ── backend choice ──────────────────────────────────────────────────── */

/* Is there a socket for this server at all?
 *
 * Probing before connecting is not an optimisation, it is what makes the choice
 * safe. Two reasons, one per backend:
 *
 *  - PipeWire's pw_context_new() deadlocks with no daemon running (above), so
 *    "try PipeWire, fall back to Pulse" would never reach the fallback on
 *    exactly the machines that need it.
 *  - libpulse AUTOSPAWNS a pulseaudio daemon when it finds no server. A window
 *    manager must not start a sound server as a side effect of drawing bars —
 *    and on a box already running PipeWire that second daemon would fight it
 *    for the ALSA device, which costs the user their audio, not just the bars.
 */
static bool socket_exists(const char *name) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !dir[0]) return false;
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

/* The same two probes the capture thread runs, asked from outside so a caller
 * can wait for a sound server to turn up instead of concluding at startup that
 * there will never be one. */
bool audio_server_running(void) {
    bool have = false;
#ifdef HAVE_PIPEWIRE
    have = have || socket_exists("pipewire-0");
#endif
#ifdef HAVE_PULSE
    have = have || socket_exists("pulse/native");
#endif
    return have;
}

static void *audio_thread(void *arg) {
    struct FwmAudio *a = arg;
    int ran = 0;

#ifdef HAVE_PIPEWIRE
    /* PipeWire first when it is actually there: it is the newer stack, and on a
     * pipewire-pulse box both probes succeed while only this one is native. */
    if (!ran && socket_exists("pipewire-0")) { run_pipewire(a); ran = 1; }
#endif
#ifdef HAVE_PULSE
    if (!ran && socket_exists("pulse/native")) { run_pulse(a); ran = 1; }
#endif
    if (!ran)
        wlr_log(WLR_INFO, "audio: no sound server running — visualiser stays silent");

    atomic_store(&a->state, AUDIO_FAILED);
    audio_release(a);   /* the compositor may be long gone; last one frees */
    return NULL;
}

/* ── api ─────────────────────────────────────────────────────────────── */

bool audio_supported(void) { return true; }

FwmAudio *audio_create(void) {
    FwmAudio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    pthread_mutex_init(&a->lock, NULL);
    pthread_mutex_init(&a->ctl, NULL);
    atomic_init(&a->state, AUDIO_STARTING);
    atomic_init(&a->quit, false);
    atomic_init(&a->refs, 2);   /* this handle + the thread */
    atomic_init(&a->channels, 2);
    atomic_init(&a->rate, 44100);

    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        pthread_mutex_destroy(&a->lock);
        pthread_mutex_destroy(&a->ctl);
        free(a);
        return NULL;
    }
    /* Detached from birth: nothing ever joins it, so there is no handle left
     * to leak by not joining. */
    pthread_detach(a->thread);
    return a;
}

int audio_state(FwmAudio *a) { return a ? atomic_load(&a->state) : AUDIO_FAILED; }
int audio_rate(FwmAudio *a)  { return a ? atomic_load(&a->rate) : 0; }

bool audio_samples(FwmAudio *a, float *out, int n) {
    if (!a || !out || n <= 0 || n > RING_SAMPLES) return false;

    pthread_mutex_lock(&a->lock);
    /* Nothing new since last time means the sink went idle: the server stops
     * calling us rather than feeding silence. Say so, so the caller lets the
     * bars fall instead of holding the last frame forever. */
    if (a->filled == 0 || (!a->wrapped && a->write < (unsigned)n)) {
        pthread_mutex_unlock(&a->lock);
        return false;
    }
    unsigned start = (a->write + RING_SAMPLES - (unsigned)n) % RING_SAMPLES;
    unsigned first = RING_SAMPLES - start;
    if (first > (unsigned)n) first = (unsigned)n;
    memcpy(out, a->ring + start, first * sizeof(float));
    if (first < (unsigned)n)
        memcpy(out + first, a->ring, ((unsigned)n - first) * sizeof(float));
    a->filled = 0;
    pthread_mutex_unlock(&a->lock);
    return true;
}

void audio_destroy(FwmAudio *a) {
    if (!a) return;

    /* Returns immediately, always. This used to join the capture thread, and
     * that cost the compositor 218-786 ms every single time [cava] was switched
     * off: pa_simple_read blocks until the monitor hands over a period, and
     * PulseAudio's buffering makes that far longer than the fragment size
     * suggests. On a display server that is a visible freeze of the entire
     * session — for a feature being turned OFF.
     *
     * So the thread is never waited for. It is told to stop, and ownership is
     * shared: whichever of the two lets go last frees the handle. A thread
     * still parked in a blocking read finishes in its own time and finds itself
     * alone. The caller must drop its pointer here — it may already be freed. */
    atomic_store(&a->quit, true);

#ifdef HAVE_PIPEWIRE
    /* Wake a running PipeWire loop; the mutex is what keeps this from racing
     * the thread tearing it down. Pulse needs no equivalent — its loop is a
     * plain read that rechecks `quit` each time round. */
    pthread_mutex_lock(&a->ctl);
    if (a->main_loop) pw_main_loop_quit(a->main_loop);
    pthread_mutex_unlock(&a->ctl);
#endif

    audio_release(a);
}

#endif /* a backend is available */
