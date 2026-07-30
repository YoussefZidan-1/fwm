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

/* wav.c reads a file the USER points [sound] path at, by walking chunk headers
 * and indexing into the bytes it finds — so every one of these cases is a file
 * that used to be able to walk off the end of the buffer or divide by a zero
 * out of the header. A collision sound is not worth a crash in the display
 * server, and none of this is reachable from a nested run: the compositor logs
 * "unreadable" and plays the click, which looks identical to working. */

#include "test.h"
#include "wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmp_path[256];

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static void put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static const char *write_bytes(const unsigned char *buf, size_t n) {
    snprintf(tmp_path, sizeof tmp_path, "/tmp/fwm-test-wav-%d.wav", (int)getpid());
    FILE *f = fopen(tmp_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", tmp_path); exit(2); }
    if (n) fwrite(buf, 1, n, f);
    fclose(f);
    return tmp_path;
}

static void drop_file(void) { unlink(tmp_path); }

/* Build a WAV in memory. `extra_chunk` inserts a junk chunk of that many bytes
 * between `fmt ` and `data`, which is what real files do and what a parser that
 * assumes fixed offsets gets wrong. `pad` makes that chunk's length odd, so the
 * word-alignment pad byte has to be honoured. */
static size_t build_wav(unsigned char *out, size_t cap, int fmt, int bits, int chans,
                        int rate, int frames, int extra_chunk, int pad) {
    int stride = chans * (bits / 8);
    size_t data_len = (size_t)frames * (size_t)stride;
    size_t extra = extra_chunk ? (size_t)(8 + extra_chunk + (pad ? 1 : 0)) : 0;
    size_t total = 12 + 24 + extra + 8 + data_len;
    if (total > cap) { fprintf(stderr, "test buffer too small\n"); exit(2); }

    memset(out, 0, total);
    memcpy(out, "RIFF", 4);
    put32(out + 4, (uint32_t)(total - 8));
    memcpy(out + 8, "WAVE", 4);

    unsigned char *p = out + 12;
    memcpy(p, "fmt ", 4);
    put32(p + 4, 16);
    put16(p + 8,  (uint16_t)fmt);
    put16(p + 10, (uint16_t)chans);
    put32(p + 12, (uint32_t)rate);
    put32(p + 16, (uint32_t)(rate * stride));
    put16(p + 20, (uint16_t)stride);
    put16(p + 22, (uint16_t)bits);
    p += 24;

    if (extra_chunk) {
        /* `pad` makes the declared length odd; the file then carries one filler
         * byte after the body, which the reader has to skip. */
        memcpy(p, "LIST", 4);
        put32(p + 4, (uint32_t)extra_chunk);
        p += 8 + extra_chunk + (pad ? 1 : 0);
    }

    memcpy(p, "data", 4);
    put32(p + 4, (uint32_t)data_len);
    p += 8;

    /* A ramp, so a wrong stride or a wrong endianness shows up as a value and
     * not merely as silence. */
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < chans; c++) {
            if (bits == 16) {
                put16(p + (size_t)i * stride + (size_t)c * 2, (uint16_t)(int16_t)(i * 100));
            } else {
                float v = (float)i / (float)frames;
                uint32_t u;
                memcpy(&u, &v, 4);
                put32(p + (size_t)i * stride + (size_t)c * 4, u);
            }
        }
    }
    return total;
}

static void test_pcm16(void) {
    CASE("16-bit pcm");
    unsigned char buf[4096];
    size_t n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 64, 0, 0);
    const char *p = write_bytes(buf, n);

    int frames = 0, rate = 0;
    const char *err = "unset";
    float *s = wav_load_mono(p, 1000, &frames, &rate, &err);
    CHECK_NOT_NULL(s);
    CHECK(err == NULL);
    CHECK_INT(frames, 64);
    CHECK_INT(rate, 44100);
    if (s) {
        CHECK_DBL(s[0], 0.0, 1e-9);
        CHECK_DBL(s[10], 1000.0 / 32768.0, 1e-6);
    }
    free(s);
    drop_file();
}

static void test_float32_stereo_downmix(void) {
    CASE("32-bit float, stereo");
    unsigned char buf[8192];
    size_t n = build_wav(buf, sizeof buf, 3, 32, 2, 48000, 32, 0, 0);
    const char *p = write_bytes(buf, n);

    int frames = 0, rate = 0;
    const char *err = "unset";
    float *s = wav_load_mono(p, 1000, &frames, &rate, &err);
    CHECK_NOT_NULL(s);
    CHECK_INT(frames, 32);
    CHECK_INT(rate, 48000);   /* the file's own rate, NOT resampled */
    /* Both channels carry the same ramp, so the downmix is that ramp. */
    if (s) CHECK_DBL(s[16], 16.0 / 32.0, 1e-6);
    free(s);
    drop_file();
}

static void test_chunks_are_walked(void) {
    CASE("junk chunk before data");
    unsigned char buf[4096];
    /* An odd-length LIST chunk between fmt and data: the pad byte after it is
     * exactly what a reader that adds `len` alone gets wrong, and then it reads
     * "ata\0" as the next chunk id and finds no samples at all. */
    size_t n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 40, 7, 1);
    const char *p = write_bytes(buf, n);

    int frames = 0, rate = 0;
    const char *err = "unset";
    float *s = wav_load_mono(p, 1000, &frames, &rate, &err);
    CHECK_NOT_NULL(s);
    CHECK_INT(frames, 40);
    free(s);
    drop_file();
}

static void test_frame_cap(void) {
    CASE("long files are truncated, not refused");
    unsigned char buf[8192];
    size_t n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 500, 0, 0);
    const char *p = write_bytes(buf, n);

    int frames = 0, rate = 0;
    float *s = wav_load_mono(p, 100, &frames, &rate, NULL);
    CHECK_NOT_NULL(s);
    CHECK_INT(frames, 100);
    free(s);
    drop_file();
}

static void test_rejections(void) {
    unsigned char buf[4096];
    int frames = 0, rate = 0;
    const char *err = NULL;

    CASE("missing file");
    CHECK(wav_load_mono("/nonexistent/fwm/no.wav", 1000, &frames, &rate, &err) == NULL);
    CHECK_NOT_NULL(err);

    CASE("not a wav");
    const char *p = write_bytes((const unsigned char *)
        "this is a text file that somebody renamed to .wav, as people do "
        "when a config option asks them for a sound and they have an mp3", 126);
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    CHECK_NOT_NULL(err);
    drop_file();

    CASE("truncated header");
    size_t n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 64, 0, 0);
    p = write_bytes(buf, 20);            /* RIFF/WAVE and half a fmt chunk */
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    drop_file();

    CASE("data chunk longer than the file");
    n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 64, 0, 0);
    /* Claim ten times the samples that are actually there. The clamp inside is
     * the only thing between this and a read past the buffer. */
    put32(buf + n - (size_t)64 * 2 - 4, 64u * 2u * 10u);
    p = write_bytes(buf, n);
    float *s = wav_load_mono(p, 1000, &frames, &rate, &err);
    if (s) {
        CHECK(frames <= 64);             /* clamped to what the file holds */
        free(s);
    }
    drop_file();

    CASE("24-bit is not supported, and says so");
    n = build_wav(buf, sizeof buf, 1, 24, 1, 44100, 30, 0, 0);
    p = write_bytes(buf, n);
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    CHECK_NOT_NULL(err);
    drop_file();

    CASE("five channels");
    n = build_wav(buf, sizeof buf, 1, 16, 2, 44100, 30, 0, 0);
    put16(buf + 12 + 10, 5);             /* channels, straight in the fmt chunk */
    p = write_bytes(buf, n);
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    drop_file();

    CASE("zero sample rate");
    n = build_wav(buf, sizeof buf, 1, 16, 1, 44100, 30, 0, 0);
    put32(buf + 12 + 12, 0);             /* a rate of 0 divides by nothing later */
    p = write_bytes(buf, n);
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    drop_file();

    CASE("empty file");
    p = write_bytes(NULL, 0);
    CHECK(wav_load_mono(p, 1000, &frames, &rate, &err) == NULL);
    drop_file();
}

int main(void) {
    test_pcm16();
    test_float32_stereo_downmix();
    test_chunks_are_walked();
    test_frame_cap();
    test_rejections();
    return t_report("wav");
}
