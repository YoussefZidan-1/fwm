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

/* A minimal WAV reader. See wav.h for why it is its own module. */
#include "wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A collision sound, not an album: anything past this is somebody pointing the
 * option at the wrong file, and reading it would be the only allocation in fwm
 * whose size a config value could pick. */
#define MAX_WAV_BYTES (16u * 1024u * 1024u)

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

float *wav_load_mono(const char *path, int max_frames,
                     int *frames_out, int *rate_out, const char **err) {
    const char *dummy = NULL;
    if (!err) err = &dummy;
    *err = NULL;
    if (!path || !frames_out || !rate_out || max_frames < 2) {
        *err = "bad arguments";
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { *err = "cannot open"; return NULL; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *err = "not seekable"; return NULL; }
    long size = ftell(f);
    rewind(f);
    /* 44 is the shortest possible header-plus-nothing; a file smaller than that
     * cannot contain a format chunk at all. */
    if (size < 44) { fclose(f); *err = "too short to be a wav"; return NULL; }
    if ((unsigned long)size > MAX_WAV_BYTES) { fclose(f); *err = "too large"; return NULL; }

    unsigned char *raw = malloc((size_t)size);
    if (!raw) { fclose(f); *err = "out of memory"; return NULL; }
    size_t got = fread(raw, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(raw); *err = "short read"; return NULL; }

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        free(raw);
        *err = "not a RIFF/WAVE file";
        return NULL;
    }

    int chans = 0, bits = 0, fmt = 0, rate = 0;
    const unsigned char *data = NULL;
    size_t data_len = 0;

    /* Chunks are WALKED rather than assumed at fixed offsets: real files carry
     * LIST, fact and cue chunks before the samples, and reading `fmt ` at byte
     * 12 works only on files written by the same tool as your test file. */
    size_t off = 12;
    while (off + 8 <= (size_t)size) {
        const unsigned char *id = raw + off;
        uint32_t len = rd32(raw + off + 4);
        size_t body = off + 8;
        /* A length that runs past the end is a truncated (or lying) file — clamp
         * rather than refuse, since the data chunk of a cut-off recording is
         * still perfectly playable, and clamping is what keeps every read below
         * inside the buffer. */
        if (len > (uint32_t)((size_t)size - body)) len = (uint32_t)((size_t)size - body);

        if (memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            fmt   = rd16(raw + body);
            chans = rd16(raw + body + 2);
            rate  = (int)rd32(raw + body + 4);
            bits  = rd16(raw + body + 14);
            /* WAVE_FORMAT_EXTENSIBLE says nothing by itself; the sample width is
             * what tells PCM from float in every file that uses it. */
            if (fmt == 0xFFFE) fmt = (bits == 32) ? 3 : 1;
        } else if (memcmp(id, "data", 4) == 0) {
            data = raw + body;
            data_len = len;
        }
        /* Chunks are word-aligned: an odd length is followed by a pad byte, and
         * missing it lands the next read in the middle of a header. */
        off = body + len + (len & 1u);
    }

    if (!data || data_len == 0)          { free(raw); *err = "no data chunk";      return NULL; }
    if (chans < 1 || chans > 2)          { free(raw); *err = "not mono or stereo"; return NULL; }
    if (rate < 4000 || rate > 384000)    { free(raw); *err = "implausible rate";   return NULL; }
    if (!((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
        free(raw);
        *err = "not 16-bit PCM or 32-bit float";
        return NULL;
    }

    int stride = chans * (bits / 8);
    long avail = (long)(data_len / (size_t)stride);
    if (avail > max_frames) avail = max_frames;
    if (avail < 2) { free(raw); *err = "no usable frames"; return NULL; }
    int frames = (int)avail;

    float *out = calloc((size_t)frames, sizeof(float));
    if (!out) { free(raw); *err = "out of memory"; return NULL; }

    for (int i = 0; i < frames; i++) {
        const unsigned char *p = data + (size_t)i * (size_t)stride;
        double sum = 0.0;
        for (int c = 0; c < chans; c++) {
            if (bits == 16) {
                sum += (double)(int16_t)rd16(p + c * 2) / 32768.0;
            } else {
                uint32_t u = rd32(p + c * 4);
                float v;
                memcpy(&v, &u, sizeof(v));   /* the file is little-endian IEEE */
                sum += (double)v;
            }
        }
        /* Downmixed to mono: the sound belongs to a window somewhere on a strip
         * ten screens wide, and panning it by where that window happens to be
         * would be a different feature — and a worse one, since the hit you
         * cannot see would come out of the wrong speaker. */
        out[i] = (float)(sum / chans);
    }

    free(raw);
    *frames_out = frames;
    *rate_out   = rate;
    return out;
}
