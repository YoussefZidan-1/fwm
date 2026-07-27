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

#include "cava.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 2048 @ 44.1 kHz is ~46 ms of sound and a 21 Hz bin. Shorter loses the bass
 * bands entirely (they collapse into bin 0); longer smears a kick drum across
 * two frames. Must stay a power of two — the transform below is radix-2. */
#define FFT_SIZE 2048
#define FFT_BINS (FFT_SIZE / 2)

/* Anything quieter than this reads as silence rather than as a bar that never
 * quite reaches the floor. */
#define DB_FLOOR -62.0

struct FwmCava {
    FwmAudio *audio;

    int mode;
    int bars;
    int screen_w, screen_h;

    float level[CONFIG_MAX_BARS];   /* smoothed, 0..1 — what is drawn AND pushed */
    float target[CONFIG_MAX_BARS];  /* this frame's raw spectrum */

    /* Log-spaced band edges as FFT bin indices, recomputed only when the rate
     * or the configured range changes. */
    int   bin_lo[CONFIG_MAX_BARS];
    int   bin_hi[CONFIG_MAX_BARS];
    int   binned_rate;
    double binned_min_hz, binned_max_hz;

    float window[FFT_SIZE];         /* Hann, precomputed */
    float re[FFT_SIZE], im[FFT_SIZE];
    float samples[FFT_SIZE];

    /* FWM_TEST_CAVA=1: drive the row from a travelling wave instead of from the
     * sound card. A nested test run has no audio to play and grim cannot hear
     * one anyway, so without this neither half of the feature can be checked
     * off a screenshot. */
    bool   synthetic;
    double phase;

    /* Drawing. NULL for a physical-only row. */
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *rect[CONFIG_MAX_BARS];
    double drawn_height;            /* height the tree was last positioned for */
    double drawn_opacity;
    unsigned drawn_theme_gen;
};

/* ── fft ─────────────────────────────────────────────────────────────── */

/* In-place iterative radix-2 Cooley-Tukey. Real input, so only the first half
 * of the spectrum is meaningful and the caller never looks past FFT_BINS. */
static void fft(float *re, float *im, int n) {
    /* Bit-reversal permutation. */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k],           ui = im[i + k];
                float vr = re[i + k + len / 2], vi = im[i + k + len / 2];
                float tr = vr * cr - vi * ci;
                float ti = vr * ci + vi * cr;
                re[i + k] = ur + tr;  im[i + k] = ui + ti;
                re[i + k + len / 2] = ur - tr;
                im[i + k + len / 2] = ui - ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* ── bands ───────────────────────────────────────────────────────────── */

/* Log-spaced band edges: octaves, not hertz. Linear bins would give the first
 * bar everything below 250 Hz and the last thirty bars nothing but hiss. */
static void rebuild_bins(FwmCava *c, int rate, double min_hz, double max_hz) {
    if (rate <= 0) return;
    if (c->binned_rate == rate && c->binned_min_hz == min_hz && c->binned_max_hz == max_hz)
        return;

    c->binned_rate   = rate;
    c->binned_min_hz = min_hz;
    c->binned_max_hz = max_hz;

    double bin_hz = (double)rate / FFT_SIZE;
    double lo_log = log(min_hz), hi_log = log(max_hz);

    for (int i = 0; i < c->bars; i++) {
        double f0 = exp(lo_log + (hi_log - lo_log) * (double)i       / c->bars);
        double f1 = exp(lo_log + (hi_log - lo_log) * (double)(i + 1) / c->bars);
        int b0 = (int)(f0 / bin_hz);
        int b1 = (int)(f1 / bin_hz);
        if (b0 < 1) b0 = 1;                 /* bin 0 is DC: pure offset, never sound */
        if (b1 <= b0) b1 = b0 + 1;          /* the low bands are narrower than one bin */
        if (b1 > FFT_BINS) b1 = FFT_BINS;
        if (b0 >= b1) b0 = b1 - 1;
        c->bin_lo[i] = b0;
        c->bin_hi[i] = b1;
    }
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void cava_build_rects(FwmCava *c, struct wlr_scene_tree *parent, double height) {
    c->tree = wlr_scene_tree_create(parent);
    if (!c->tree) return;
    wlr_scene_node_set_position(&c->tree->node, 0, c->screen_h - (int)lround(height));
    c->drawn_height = height;

    const FwmTheme *thm = theme_get();
    float col[4] = { (float)thm->accent[0], (float)thm->accent[1], (float)thm->accent[2], 1.0f };
    for (int i = 0; i < c->bars; i++) {
        c->rect[i] = wlr_scene_rect_create(c->tree, 1, 1, col);
        if (c->rect[i]) wlr_scene_node_set_enabled(&c->rect[i]->node, false);
    }
    c->drawn_theme_gen = theme_generation();
}

static void cava_draw(FwmCava *c, const CavaConfig *cfg) {
    if (!c->tree) return;

    double height = cfg->height;
    if (c->drawn_height != height) {
        wlr_scene_node_set_position(&c->tree->node, 0, c->screen_h - (int)lround(height));
        c->drawn_height = height;
    }

    /* The bars follow [decor] color_source like every other island does, so a
     * new wallpaper repaints them too. */
    unsigned gen = theme_generation();
    bool recolor = gen != c->drawn_theme_gen || cfg->opacity != c->drawn_opacity;
    float col[4];
    if (recolor) {
        const FwmTheme *thm = theme_get();
        /* Premultiplied: wlr_scene_rect takes the colour straight to the
         * renderer, and an unpremultiplied one washes out over the wallpaper. */
        float a = (float)cfg->opacity;
        col[0] = (float)thm->accent[0] * a;
        col[1] = (float)thm->accent[1] * a;
        col[2] = (float)thm->accent[2] * a;
        col[3] = a;
        c->drawn_theme_gen = gen;
        c->drawn_opacity   = cfg->opacity;
    }

    double slot = (double)c->screen_w / c->bars;
    int bw = (int)lround(slot - cfg->gap);
    if (bw < 1) bw = 1;

    for (int i = 0; i < c->bars; i++) {
        struct wlr_scene_rect *r = c->rect[i];
        if (!r) continue;
        if (recolor) wlr_scene_rect_set_color(r, col);

        int bh = (int)lround(c->level[i] * height);
        if (bh < 1) {
            wlr_scene_node_set_enabled(&r->node, false);
            continue;
        }
        wlr_scene_rect_set_size(r, bw, bh);
        wlr_scene_node_set_position(&r->node, (int)lround(i * slot),
                                    (int)lround(height) - bh);
        wlr_scene_node_set_enabled(&r->node, true);
    }
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

FwmCava *cava_create(struct wlr_scene_tree *parent, const CavaConfig *cfg,
                     int screen_w, int screen_h) {
    if (!cfg || cfg->mode == CAVA_MODE_OFF) return NULL;
    if (screen_w <= 0 || screen_h <= 0) return NULL;

    const char *tc = getenv("FWM_TEST_CAVA");
    bool synthetic = tc && atoi(tc) != 0;

    /* audio_create returns before it knows whether a sound server exists — it
     * must, because finding out can block forever (see audio.h). So the row is
     * built optimistically and torn down later if the capture gives up; that is
     * what cava_dead() below is for. */
    FwmAudio *audio = synthetic ? NULL : audio_create();
    if (!audio && !synthetic) return NULL;

    FwmCava *c = calloc(1, sizeof(*c));
    if (!c) { audio_destroy(audio); return NULL; }

    c->audio     = audio;
    c->synthetic = synthetic;
    c->mode      = cfg->mode;
    c->bars      = cfg->bars;
    if (c->bars < 2) c->bars = 2;
    if (c->bars > CONFIG_MAX_BARS) c->bars = CONFIG_MAX_BARS;
    c->screen_w  = screen_w;
    c->screen_h  = screen_h;

    for (int i = 0; i < FFT_SIZE; i++)
        c->window[i] = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / (FFT_SIZE - 1));

    if (cfg->mode & CAVA_MODE_VISUAL) cava_build_rects(c, parent, cfg->height);

    return c;
}

void cava_destroy(FwmCava *c) {
    if (!c) return;
    if (c->tree) wlr_scene_node_destroy(&c->tree->node);
    audio_destroy(c->audio);
    free(c);
}

int cava_mode(const FwmCava *c) { return c ? c->mode : CAVA_MODE_OFF; }

bool cava_dead(const FwmCava *c) {
    if (!c || c->synthetic) return false;
    return audio_state(c->audio) == AUDIO_FAILED;
}

const float *cava_levels(const FwmCava *c, int *count) {
    if (!c || !(c->mode & CAVA_MODE_PHYSICAL)) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = c->bars;
    return c->level;
}

bool cava_busy(const FwmCava *c) {
    if (!c) return false;
    for (int i = 0; i < c->bars; i++)
        if (c->level[i] > 0.002f) return true;
    return false;
}

/* ── tick ────────────────────────────────────────────────────────────── */

void cava_tick(FwmCava *c, const CavaConfig *cfg, double dt) {
    if (!c || !cfg) return;

    if (c->synthetic) {
        /* A wave that travels along the row plus a slow whole-row swell, so a
         * single screenshot shows bars at many different heights and a window
         * dropped on top gets tipped rather than lifted straight up. */
        c->phase += dt * 2.0;
        double swell = 0.55 + 0.45 * sin(c->phase * 0.7);
        for (int i = 0; i < c->bars; i++) {
            double t = (double)i / c->bars;
            double v = 0.5 + 0.5 * sin(c->phase + t * 6.0 * M_PI);
            c->target[i] = (float)(v * swell * cfg->sensitivity);
            if (c->target[i] > 1.0f) c->target[i] = 1.0f;
        }
    } else if (audio_samples(c->audio, c->samples, FFT_SIZE)) {
        rebuild_bins(c, audio_rate(c->audio), cfg->min_hz, cfg->max_hz);

        for (int i = 0; i < FFT_SIZE; i++) {
            c->re[i] = c->samples[i] * c->window[i];
            c->im[i] = 0.0f;
        }
        fft(c->re, c->im, FFT_SIZE);

        for (int i = 0; i < c->bars; i++) {
            /* Peak, not mean, across the band: a mean over a wide treble band
             * is dominated by the empty bins between harmonics, and the top
             * third of the row never moves. */
            float peak = 0.0f;
            for (int b = c->bin_lo[i]; b < c->bin_hi[i]; b++) {
                float m = c->re[b] * c->re[b] + c->im[b] * c->im[b];
                if (m > peak) peak = m;
            }
            /* Hann halves the coherent gain, hence the 4/FFT_SIZE rather than
             * 2/FFT_SIZE that a rectangular window would want. */
            double mag = sqrt((double)peak) * 4.0 / FFT_SIZE;
            double db  = 20.0 * log10(mag + 1e-9);
            double v   = (db - DB_FLOOR) / (0.0 - DB_FLOOR);
            v *= cfg->sensitivity;
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            c->target[i] = (float)v;
        }

        /* One pass of a 1-2-1 kernel across the row. Neighbouring bands of real
         * music differ by more than the eye reads as a spectrum, and without
         * this the row looks like noise rather than like sound. */
        float prev = c->target[0];
        for (int i = 1; i < c->bars - 1; i++) {
            float cur = c->target[i];
            c->target[i] = 0.25f * prev + 0.5f * cur + 0.25f * c->target[i + 1];
            prev = cur;
        }
    } else {
        /* Nothing playing: fall to zero rather than freezing the last frame. */
        memset(c->target, 0, sizeof(c->target));
    }

    /* Attack instantly, decay slowly — a bar that eased upward would round off
     * every transient, which is the only part of a beat worth showing. The
     * decay factor is raised to dt*60 so a slow frame decays by the same
     * amount of TIME rather than by the same amount per frame. */
    double fall = pow(cfg->smoothing, dt * 60.0);
    for (int i = 0; i < c->bars; i++) {
        if (c->target[i] >= c->level[i]) c->level[i] = c->target[i];
        else c->level[i] = (float)(c->level[i] * fall + c->target[i] * (1.0 - fall));
        if (c->level[i] < 0.001f) c->level[i] = 0.0f;
    }

    /* c->mode, not cfg->mode: the instance was built for one of them and owns
     * (or does not own) a scene subtree accordingly. A mode change in the config
     * rebuilds the whole thing; it must never quietly retarget this one. */
    if (c->mode & CAVA_MODE_VISUAL) cava_draw(c, cfg);
}
