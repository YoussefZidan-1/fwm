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

#include "rotate.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/* The quad is drawn with the corner positions computed on the CPU, so the
 * shaders carry no matrix at all: the rotation IS the four vertices. */
static const char vert_src[] =
    "attribute vec2 pos;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "  v_texcoord = texcoord;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char frag_2d_src[] =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord); }\n";

/* A dmabuf imported by the GLES2 renderer usually lands on the external target
 * instead of GL_TEXTURE_2D, and sampling it needs a different sampler type —
 * hence two programs rather than one. */
static const char frag_ext_src[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform samplerExternalOES tex;\n"
    "void main() { gl_FragColor = texture2D(tex, v_texcoord); }\n";

struct program {
    GLuint id;
    GLint attr_pos, attr_texcoord, uni_tex;
    bool tried;       /* compiled once already, successfully or not */
};

/* fwm has exactly one renderer and one thread, so the programs live here
 * rather than being threaded through every caller. `owner` guards the only way
 * that could go wrong: a config reload that replaces the renderer would leave
 * these belonging to a destroyed context. */
static struct wlr_renderer *owner;
static struct program prog_2d, prog_ext;

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        wlr_log(WLR_ERROR, "rotate: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool program_build(struct program *p, const char *frag_src) {
    if (p->tried) return p->id != 0;
    p->tried = true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    /* The program keeps its own reference until it is linked; the shaders are
     * of no further use to us either way. */
    glDetachShader(id, vs);
    glDetachShader(id, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(id, sizeof(log) - 1, NULL, log);
        wlr_log(WLR_ERROR, "rotate: shader link failed: %s", log);
        glDeleteProgram(id);
        return false;
    }

    p->id = id;
    p->attr_pos = glGetAttribLocation(id, "pos");
    p->attr_texcoord = glGetAttribLocation(id, "texcoord");
    p->uni_tex = glGetUniformLocation(id, "tex");
    return true;
}

bool rotate_supported(struct wlr_renderer *renderer) {
    return renderer && wlr_renderer_is_gles2(renderer);
}

/* Making the renderer's context current is the whole reason this file can
 * exist: wlroots hands out the EGL display and context precisely so a
 * compositor can render something the render pass API cannot express. The
 * previous context is restored afterwards — wlroots' own passes do the same,
 * and the current EGL context is global state shared with them. */
struct egl_save {
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
};

static bool egl_enter(struct wlr_renderer *renderer, struct egl_save *save) {
    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
    if (!egl) return false;
    EGLDisplay dpy = wlr_egl_get_display(egl);
    EGLContext ctx = wlr_egl_get_context(egl);
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;

    save->display = eglGetCurrentDisplay();
    save->context = eglGetCurrentContext();
    save->draw = eglGetCurrentSurface(EGL_DRAW);
    save->read = eglGetCurrentSurface(EGL_READ);

    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
}

static void egl_leave(struct wlr_renderer *renderer, const struct egl_save *save) {
    if (save->display == EGL_NO_DISPLAY) {
        struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
        eglMakeCurrent(wlr_egl_get_display(egl), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return;
    }
    eglMakeCurrent(save->display, save->draw, save->read, save->context);
}

bool rotate_blit(struct wlr_renderer *renderer, struct wlr_buffer *dst,
                 struct wlr_texture *src, int src_w, int src_h, double angle) {
    if (!rotate_supported(renderer) || !dst || !src) return false;
    if (src_w <= 0 || src_h <= 0) return false;
    if (owner != renderer) {
        /* A different renderer than the one the programs were built on: forget
         * them without touching GL. The old context is the only thing that
         * could free them and it is already gone — calling into it would be a
         * use-after-free, and the objects died with it anyway. */
        memset(&prog_2d, 0, sizeof(prog_2d));
        memset(&prog_ext, 0, sizeof(prog_ext));
        owner = renderer;
    }

    struct wlr_gles2_texture_attribs attribs = {0};
    if (!wlr_texture_is_gles2(src)) return false;
    wlr_gles2_texture_get_attribs(src, &attribs);

    struct egl_save save;
    if (!egl_enter(renderer, &save)) return false;

    bool ok = false;

    struct program *p = attribs.target == GL_TEXTURE_EXTERNAL_OES ? &prog_ext : &prog_2d;
    if (!program_build(p, p == &prog_ext ? frag_ext_src : frag_2d_src)) goto out;

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, dst);
    if (!fbo) {
        wlr_log(WLR_ERROR, "rotate: no FBO for the destination buffer");
        goto out;
    }

    int dw = dst->width, dh = dst->height;

    /* The four corners of the source rectangle, rotated about the center of
     * the destination. Same y-down matrix Box2D's polygon uses in this world,
     * so what is drawn is exactly the box that collides. */
    double c = cos(angle), s = sin(angle);
    double hw = src_w / 2.0, hh = src_h / 2.0;
    const double local[4][2] = {
        { -hw, -hh }, {  hw, -hh }, {  hw,  hh }, { -hw,  hh },
    };
    /* Texture coordinates follow the same corner order. v=0 is the top row of
     * the source, and NDC y=-1 is the top row of the destination FBO (wlroots
     * renders into buffers y-down), so top maps to top with no flip. */
    static const GLfloat texcoords[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    GLfloat verts[8];
    for (int i = 0; i < 4; i++) {
        double x = dw / 2.0 + (c * local[i][0] - s * local[i][1]);
        double y = dh / 2.0 + (s * local[i][0] + c * local[i][1]);
        verts[i * 2 + 0] = (GLfloat)(2.0 * x / dw - 1.0);
        verts[i * 2 + 1] = (GLfloat)(2.0 * y / dh - 1.0);
    }
    /* Triangle fan over the corners in order: 0-1-2, 0-2-3. */

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dw, dh);
    glDisable(GL_SCISSOR_TEST);
    /* The destination is bigger than the window (it has to hold the rotated
     * box), so everything outside the quad must end up fully transparent. */
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* No blending: the snapshot is already premultiplied and the target was
     * just cleared, so a straight copy preserves its alpha exactly. Blending
     * it over transparent black would multiply the colour by alpha twice and
     * leave a translucent window with dark fringes. */
    glDisable(GL_BLEND);

    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(attribs.target, attribs.tex);
    /* Linear sampling is what makes a rotation look rotated rather than
     * shredded; clamping keeps the edge pixels from wrapping around. */
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(p->uni_tex, 0);

    /* Client-side arrays, so a buffer left bound by whoever ran last would be
     * read instead of ours. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(p->attr_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(p->attr_texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(p->attr_pos);
    glEnableVertexAttribArray(p->attr_texcoord);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(p->attr_pos);
    glDisableVertexAttribArray(p->attr_texcoord);
    glBindTexture(attribs.target, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = true;

out:
    egl_leave(renderer, &save);
    return ok;
}

void rotate_shutdown(struct wlr_renderer *renderer) {
    if (!renderer || !rotate_supported(renderer)) return;
    if (!prog_2d.id && !prog_ext.id) {
        owner = NULL;
        return;
    }

    struct egl_save save;
    if (egl_enter(renderer, &save)) {
        if (prog_2d.id) glDeleteProgram(prog_2d.id);
        if (prog_ext.id) glDeleteProgram(prog_ext.id);
        egl_leave(renderer, &save);
    }
    memset(&prog_2d, 0, sizeof(prog_2d));
    memset(&prog_ext, 0, sizeof(prog_ext));
    owner = NULL;
}
