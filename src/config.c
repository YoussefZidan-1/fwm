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

#include "config.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>

/* ── physics defaults (mirrors old defines.h) ────────────────────────── */

static const PhysicsConfig physics_defaults = {
    .friction               = 0.97,
    .mass_density           = 0.0005,
    .throw_speed_multiplier = 0.65,
    .max_throw_speed        = 1800.0,
    .stop_speed_threshold   = 1.0,
    .restitution            = 0.75,
    .gravity                = 200.0,
    .tick_rate              = 60.0,
    /* No profiles, every desktop on the world's own values, and the gravity
     * ladder cycle_gravity has always climbed. Spelled out because this struct
     * is also assigned wholesale on the paths that never reach load_physics
     * (no config file, unreadable file, syntax error). */
    .profile_count      = 0,
    .desktop_profile    = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    .gravity_steps      = { 0.0, 0.15, 1.0 },
    .gravity_step_count = 3,
};

/* ── diagnostics ─────────────────────────────────────────────────────── */

/* Record a config problem. Never fatal: the caller carries on with defaults for
 * whatever it could not read, so a typo can never cost the user their session. */
void config_report_error(FwmConfig *cfg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[CONFIG_ERR_LEN];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "fwm config: %s\n", buf);
    cfg->error_total++;
    if (cfg->error_count < CONFIG_MAX_ERRORS) {
        snprintf(cfg->errors[cfg->error_count].msg, CONFIG_ERR_LEN, "%s", buf);
        cfg->error_count++;
    }
}

/* Actions understood by server_dispatch_action. Kept here so a typo in a bind
 * is reported at load time instead of silently doing nothing when pressed. */
static int action_is_known(const char *a) {
    static const char *exact[] = {
        "killclient", "toggle_tiling", "toggle_split", "EXIT", "show_hints",
        "show_errors", "reload_config", "wallpaper_picker", "group_toggle", "group_next",
        "group_prev", "group_add", "cycle_gravity", "pin_window",
        "toggle_nocollide", "toggle_nocollide_all", "toggle_tiling_all",
        "toggle_floating", "toggle_floating_all",
        "calm_all", "fake_fullscreen", "real_fullscreen",
        "launcher", "toggle_tray", "spin_window", "spin_all", NULL
    };
    static const char *prefixes[] = {
        "spawn:", "view:", "move_camera:", "tile_focus:", "tile_move:",
        "move_to:", "move_to_view:", FWM_MODE_ACTION, NULL
    };
    for (int i = 0; exact[i]; i++)
        if (strcmp(a, exact[i]) == 0) return 1;
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(a, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

/* ── mod key parsing ─────────────────────────────────────────────────── */

static unsigned int parse_mod_token(const char *tok) {
    if (strcmp(tok, "super") == 0)  return FWM_MOD_LOGO;
    if (strcmp(tok, "alt")   == 0)  return FWM_MOD_ALT;
    if (strcmp(tok, "ctrl")  == 0)  return FWM_MOD_CTRL;
    if (strcmp(tok, "shift") == 0)  return FWM_MOD_SHIFT;
    return 0;
}

// Split string helper because strsep modifies the pointer and we want a clean implementation.
static int parse_bind_key(const char *str, unsigned int *mod_out, xkb_keysym_t *key_out) {
    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    *mod_out = 0;
    *key_out = XKB_KEY_NoSymbol;

    char *tokens[8];
    int   n = 0;
    char *p = buf;
    char *tok;
    
    // Using standard strsep-like behavior or manual tokenization.
    // We can use strsep here since we make a local copy in buf.
    while ((tok = strsep(&p, "+")) != NULL && n < 8) {
        tokens[n++] = tok;
    }

    if (n == 0) return 0;

    for (int i = 0; i < n - 1; i++) {
        *mod_out |= parse_mod_token(tokens[i]);
    }

    // Convert keysym name to xkb_keysym_t.
    // If the key is e.g. "Return", xkb_keysym_from_name expects "Return".
    // Special key names in xkbcommon might be slightly different than X11 but mostly they align.
    // XStringToKeysym and xkb_keysym_from_name are highly compatible.
    // We need to map some common ones if they differ, but standard ones like "Return", "space", "q", "Escape" are identical.
    const char *keyname = tokens[n - 1];
    
    // In TOML, we had "Return", "space", "q", "t", "d", "f", "h", "l", "p", "n", "c", "g", "Escape", "question", etc.
    // X11 "Escape" -> xkbcommon "Escape".
    // X11 "Return" -> xkbcommon "Return".
    // X11 "space" -> xkbcommon "space".
    // X11 "question" -> xkbcommon "question".
    *key_out = xkb_keysym_from_name(keyname, XKB_KEYSYM_CASE_INSENSITIVE);
    if (*key_out == XKB_KEY_NoSymbol) {
        // Some fallback matches
        if (strcmp(keyname, "Return") == 0) *key_out = XKB_KEY_Return;
        else if (strcmp(keyname, "Escape") == 0) *key_out = XKB_KEY_Escape;
        else if (strcmp(keyname, "space") == 0) *key_out = XKB_KEY_space;
        else if (strcmp(keyname, "question") == 0) *key_out = XKB_KEY_question;
        else return 0; /* caller reports it with the full bind string */
    }
    return 1;
}

/* ── physics section ─────────────────────────────────────────────────── */

#define LOAD_DOUBLE(tbl, field, cfg_field) \
    do { \
        toml_datum_t _d = toml_double_in(tbl, field); \
        if (_d.ok) cfg_field = _d.u.d; \
    } while (0)

/* One [physics.<name>] profile: the world's values, with whatever the table
 * writes over the top, plus the desktops it claims. */
static void load_physics_profile(FwmConfig *cfg, toml_table_t *tbl, const char *name) {
    PhysicsConfig *p = &cfg->physics;
    if (p->profile_count >= CONFIG_MAX_PROFILES) {
        config_report_error(cfg, "[physics.%s]: too many profiles (max %d) — ignored",
                            name, CONFIG_MAX_PROFILES);
        return;
    }

    PhysicsProfileConfig *pr = &p->profiles[p->profile_count];
    snprintf(pr->name, sizeof(pr->name), "%s", name);
    pr->friction     = p->friction;
    pr->mass_density = p->mass_density;
    pr->restitution  = p->restitution;
    pr->gravity      = p->gravity;

    LOAD_DOUBLE(tbl, "friction",     pr->friction);
    LOAD_DOUBLE(tbl, "mass_density", pr->mass_density);
    LOAD_DOUBLE(tbl, "restitution",  pr->restitution);
    LOAD_DOUBLE(tbl, "gravity",      pr->gravity);

    /* A profile nothing points at is dead weight, and silence about it is the
     * kind of thing that costs an evening. */
    toml_array_t *desks = toml_array_in(tbl, "desktops");
    int claimed = 0;
    for (int i = 0; desks && i < toml_array_nelem(desks); i++) {
        toml_datum_t d = toml_int_at(desks, i);
        if (!d.ok) continue;
        if (d.u.i < 0 || d.u.i >= FWM_DESKTOPS) {
            config_report_error(cfg, "[physics.%s]: desktop %lld out of range 0..%d — ignored",
                                name, (long long)d.u.i, FWM_DESKTOPS - 1);
            continue;
        }
        p->desktop_profile[d.u.i] = p->profile_count;
        claimed++;
    }
    if (!claimed)
        config_report_error(cfg, "[physics.%s]: no desktops = [...] — profile unused", name);

    p->profile_count++;
}

static void load_physics(toml_table_t *root, FwmConfig *cfg) {
    PhysicsConfig *p = &cfg->physics;
    *p = physics_defaults;
    p->profile_count = 0;
    for (int i = 0; i < FWM_DESKTOPS; i++) p->desktop_profile[i] = -1;
    /* Zero-g, a lick of gravity, and earth — what cycle_gravity has always
     * walked, now merely the default list rather than the only one. */
    p->gravity_steps[0] = 0.0;
    p->gravity_steps[1] = 0.15;
    p->gravity_steps[2] = 1.0;
    p->gravity_step_count = 3;

    toml_table_t *tbl = toml_table_in(root, "physics");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "friction",               p->friction);
    LOAD_DOUBLE(tbl, "mass_density",           p->mass_density);
    LOAD_DOUBLE(tbl, "throw_speed_multiplier", p->throw_speed_multiplier);
    LOAD_DOUBLE(tbl, "max_throw_speed",        p->max_throw_speed);
    LOAD_DOUBLE(tbl, "stop_speed_threshold",   p->stop_speed_threshold);
    LOAD_DOUBLE(tbl, "restitution",            p->restitution);
    LOAD_DOUBLE(tbl, "gravity",                p->gravity);
    LOAD_DOUBLE(tbl, "tick_rate",              p->tick_rate);

    toml_array_t *steps = toml_array_in(tbl, "gravity_steps");
    if (steps) {
        int n = toml_array_nelem(steps);
        int count = 0;
        for (int i = 0; i < n && count < CONFIG_MAX_GRAVITY_STEPS; i++) {
            toml_datum_t d = toml_double_at(steps, i);
            if (!d.ok) {
                toml_datum_t di = toml_int_at(steps, i);   /* "1" is not "1.0" */
                if (!di.ok) continue;
                d.u.d = (double)di.u.i;
            }
            p->gravity_steps[count++] = d.u.d;
        }
        if (count > 0) p->gravity_step_count = count;
        else config_report_error(cfg, "[physics] gravity_steps: no usable numbers — "
                                      "keeping the built-in steps");
        if (n > CONFIG_MAX_GRAVITY_STEPS)
            config_report_error(cfg, "[physics] gravity_steps: only the first %d are used",
                                CONFIG_MAX_GRAVITY_STEPS);
    }

    /* Sub-tables of [physics] are profiles; the scalars above are not. Walked
     * after them so a profile inherits the world's final values. */
    for (int i = 0; ; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) break;
        toml_table_t *sub = toml_table_in(tbl, key);
        if (sub) load_physics_profile(cfg, sub, key);
    }
}

/* ── tiling section ──────────────────────────────────────────────────── */

static void load_tiling(toml_table_t *root, TilingConfig *t) {
    t->gaps_in    = 6;
    t->gaps_out   = 14;
    t->anim_speed = 12.0; /* ~250 ms glide */

    toml_table_t *tbl = toml_table_in(root, "tiling");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_int_in(tbl, "gaps_in");
    if (d.ok) t->gaps_in = (int)d.u.i;
    d = toml_int_in(tbl, "gaps_out");
    if (d.ok) t->gaps_out = (int)d.u.i;
    LOAD_DOUBLE(tbl, "anim_speed", t->anim_speed);

    if (t->gaps_in < 0) t->gaps_in = 0;
    if (t->gaps_out < 0) t->gaps_out = 0;
}

/* ── camera section ──────────────────────────────────────────────────── */

static void load_camera(toml_table_t *root, CameraConfig *c) {
    c->anim_ms = 350.0;
    c->free_speed = 14.0;

    toml_table_t *tbl = toml_table_in(root, "camera");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "anim_ms", c->anim_ms);
    LOAD_DOUBLE(tbl, "free_speed", c->free_speed);
}

/* ── decor section ───────────────────────────────────────────────────── */

/* Parse "#RRGGBB" or "#RRGGBBAA" into RGBA floats. Returns 0 on bad input. */
static int parse_hex_color(const char *s, float out[4]) {
    if (!s || s[0] != '#') return 0;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8) return 0;

    unsigned int v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < len / 2; i++) {
        char buf[3] = { s[1 + i*2], s[2 + i*2], 0 };
        char *end;
        v[i] = (unsigned int)strtoul(buf, &end, 16);
        if (*end) return 0;
    }
    for (int i = 0; i < 4; i++) out[i] = v[i] / 255.0f;
    // wlr_scene_rect expects premultiplied alpha
    for (int i = 0; i < 3; i++) out[i] *= out[3];
    return 1;
}

static void load_decor(toml_table_t *root, FwmConfig *cfg) {
    DecorConfig *dc = &cfg->decor;
    dc->border_width = 2;
    parse_hex_color("#7aa2f7", dc->col_active);   /* soft blue */
    parse_hex_color("#3b4261", dc->col_inactive); /* muted slate */
    dc->fade_in_ms = 260.0;
    dc->wallpaper_fade_ms = 420.0;
    dc->tray_opacity = 0.92;
    dc->launcher_opacity = 0.92;
    dc->icon_theme[0] = '\0';
    dc->color_source = COLOR_SOURCE_CONFIG;
    dc->tint_strength = 0.4;

    toml_table_t *tbl = toml_table_in(root, "decor");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_int_in(tbl, "border_width");
    if (d.ok) dc->border_width = (int)d.u.i;
    if (dc->border_width < 0) dc->border_width = 0;

    d = toml_string_in(tbl, "col_active");
    if (d.ok) {
        if (!parse_hex_color(d.u.s, dc->col_active))
            config_report_error(cfg, "[decor] col_active: \"%s\" is not #RRGGBB[AA]", d.u.s);
        free(d.u.s);
    }
    d = toml_string_in(tbl, "col_inactive");
    if (d.ok) {
        if (!parse_hex_color(d.u.s, dc->col_inactive))
            config_report_error(cfg, "[decor] col_inactive: \"%s\" is not #RRGGBB[AA]", d.u.s);
        free(d.u.s);
    }
    LOAD_DOUBLE(tbl, "fade_in_ms", dc->fade_in_ms);
    LOAD_DOUBLE(tbl, "wallpaper_fade_ms", dc->wallpaper_fade_ms);
    d = toml_string_in(tbl, "icon_theme");
    if (d.ok) { snprintf(dc->icon_theme, sizeof(dc->icon_theme), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "color_source");
    if (d.ok) {
        if (strcmp(d.u.s, "wallpaper") == 0)   dc->color_source = COLOR_SOURCE_WALLPAPER;
        else if (strcmp(d.u.s, "config") == 0) dc->color_source = COLOR_SOURCE_CONFIG;
        else config_report_error(cfg, "[decor] color_source: unknown value \"%s\" "
                                      "(use \"config\" or \"wallpaper\")", d.u.s);
        free(d.u.s);
    }
    LOAD_DOUBLE(tbl, "tint_strength", dc->tint_strength);
    if (dc->tint_strength < 0.0) dc->tint_strength = 0.0;
    if (dc->tint_strength > 1.0) dc->tint_strength = 1.0;
    LOAD_DOUBLE(tbl, "tray_opacity", dc->tray_opacity);
    LOAD_DOUBLE(tbl, "launcher_opacity", dc->launcher_opacity);
    if (dc->tray_opacity < 0.0) dc->tray_opacity = 0.0;
    if (dc->tray_opacity > 1.0) dc->tray_opacity = 1.0;
    if (dc->launcher_opacity < 0.0) dc->launcher_opacity = 0.0;
    if (dc->launcher_opacity > 1.0) dc->launcher_opacity = 1.0;
}

/* ── input section ───────────────────────────────────────────────────── */

/* A boolean the file may simply not mention. Leaves `*out` untouched then, so
 * the tri-state -1 (or tap's 1) survives. */
static void load_tristate(toml_table_t *tbl, const char *key, int *out) {
    toml_datum_t d = toml_bool_in(tbl, key);
    if (d.ok) *out = d.u.b ? 1 : 0;
}

/* One of a fixed set of words, copied verbatim so the applying code can map it
 * to libinput's enum — config.c has no business including libinput.h. */
static void load_enum(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                      const char *const *allowed, char *out, size_t cap) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (!d.ok) return;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(d.u.s, allowed[i]) == 0) {
            snprintf(out, cap, "%s", d.u.s);
            free(d.u.s);
            return;
        }
    }
    /* Build the list for the message rather than repeating it at each call. */
    char list[128] = "";
    for (int i = 0; allowed[i]; i++) {
        if (i) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, allowed[i], sizeof(list) - strlen(list) - 1);
    }
    config_report_error(cfg, "[input] %s: unknown value \"%s\" (want %s) — ignored",
                        key, d.u.s, list);
    free(d.u.s);
}

static void load_input(toml_table_t *root, FwmConfig *cfg) {
    InputConfig *in = &cfg->input;
    in->kbd_layout[0]  = '\0';
    in->kbd_variant[0] = '\0';
    in->kbd_options[0] = '\0';
    in->repeat_rate  = 25;
    in->repeat_delay = 600;

    /* Everything libinput owns is left alone unless the file says otherwise —
     * except tap, which fwm turns on (see InputConfig). */
    in->tap              = 1;
    in->tap_drag         = -1;
    in->drag_lock        = -1;
    in->natural_scroll   = -1;
    in->dwt              = -1;
    in->middle_emulation = -1;
    in->left_handed      = -1;
    in->accel_speed      = INPUT_ACCEL_UNSET;
    in->accel_profile[0] = '\0';
    in->scroll_method[0] = '\0';
    in->click_method[0]  = '\0';

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "input");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_string_in(tbl, "kbd_layout");
    if (d.ok) { snprintf(in->kbd_layout, sizeof(in->kbd_layout), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "kbd_variant");
    if (d.ok) { snprintf(in->kbd_variant, sizeof(in->kbd_variant), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "kbd_options");
    if (d.ok) { snprintf(in->kbd_options, sizeof(in->kbd_options), "%s", d.u.s); free(d.u.s); }
    d = toml_int_in(tbl, "repeat_rate");
    if (d.ok && d.u.i > 0) in->repeat_rate = (int)d.u.i;
    d = toml_int_in(tbl, "repeat_delay");
    if (d.ok && d.u.i > 0) in->repeat_delay = (int)d.u.i;

    /* Touchpad. A device that cannot do one of these ignores it; see
     * pointer_apply_input_config. */
    load_tristate(tbl, "tap",              &in->tap);
    load_tristate(tbl, "tap_drag",         &in->tap_drag);
    load_tristate(tbl, "drag_lock",        &in->drag_lock);
    load_tristate(tbl, "natural_scroll",   &in->natural_scroll);
    load_tristate(tbl, "dwt",              &in->dwt);
    load_tristate(tbl, "middle_emulation", &in->middle_emulation);
    load_tristate(tbl, "left_handed",      &in->left_handed);

    d = toml_double_in(tbl, "accel_speed");
    if (d.ok) {
        if (d.u.d >= -1.0 && d.u.d <= 1.0) in->accel_speed = d.u.d;
        else config_report_error(cfg, "[input] accel_speed %.3f out of range "
                                 "-1..1 — ignored", d.u.d);
    }

    static const char *const profiles[] = { "adaptive", "flat", NULL };
    static const char *const scrolls[]  = { "two_finger", "edge", "button", "none", NULL };
    static const char *const clicks[]   = { "button_areas", "clickfinger", "none", NULL };
    load_enum(cfg, tbl, "accel_profile", profiles,
              in->accel_profile, sizeof(in->accel_profile));
    load_enum(cfg, tbl, "scroll_method", scrolls,
              in->scroll_method, sizeof(in->scroll_method));
    load_enum(cfg, tbl, "click_method", clicks,
              in->click_method, sizeof(in->click_method));
}

static void load_focus(toml_table_t *root, FocusConfig *f, FwmConfig *cfg) {
    /* Default keeps activation useful without ever yanking the view away from
     * what the user is looking at. */
    f->on_activate = FOCUS_ACTIVATE_SAME_DESKTOP;

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "focus");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "on_activate");
    if (!d.ok) return;
    if      (strcmp(d.u.s, "never")        == 0) f->on_activate = FOCUS_ACTIVATE_NEVER;
    else if (strcmp(d.u.s, "same_desktop") == 0) f->on_activate = FOCUS_ACTIVATE_SAME_DESKTOP;
    else if (strcmp(d.u.s, "always")       == 0) f->on_activate = FOCUS_ACTIVATE_ALWAYS;
    else config_report_error(cfg, "[focus] on_activate: unknown value \"%s\" "
                                  "(never | same_desktop | always)", d.u.s);
    free(d.u.s);
}

static void load_effects(toml_table_t *root, EffectsConfig *e) {
    /* Shake is OFF by default: it moves the whole view on every hard landing,
     * which reads as intrusive during actual work. Opt in. */
    e->camera_shake = 0.0;
    e->squash = 1.0;
    e->jelly = 1.0;
    e->spin = 1.0;
    e->live = 1.0;
    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "effects");
    if (!tbl) return;
    LOAD_DOUBLE(tbl, "camera_shake", e->camera_shake);
    if (e->camera_shake < 0.0) e->camera_shake = 0.0;
    if (e->camera_shake > 4.0) e->camera_shake = 4.0;  /* past this it is nausea */
    LOAD_DOUBLE(tbl, "squash", e->squash);
    if (e->squash < 0.0) e->squash = 0.0;
    if (e->squash > 2.0) e->squash = 2.0;
    LOAD_DOUBLE(tbl, "jelly", e->jelly);
    if (e->jelly < 0.0) e->jelly = 0.0;
    if (e->jelly > 2.0) e->jelly = 2.0;
    LOAD_DOUBLE(tbl, "live", e->live);
    if (e->live < 0.0) e->live = 0.0;
    if (e->live > 1.0) e->live = 1.0;
    LOAD_DOUBLE(tbl, "spin", e->spin);
    if (e->spin < 0.0) e->spin = 0.0;
    if (e->spin > 4.0) e->spin = 4.0;
}

static void load_session(toml_table_t *root, SessionConfig *s, FwmConfig *cfg) {
    s->restore = SESSION_RESTORE_CRASH;
    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "session");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "restore");
    if (!d.ok) return;
    if      (strcmp(d.u.s, "crash")  == 0) s->restore = SESSION_RESTORE_CRASH;
    else if (strcmp(d.u.s, "always") == 0) s->restore = SESSION_RESTORE_ALWAYS;
    else if (strcmp(d.u.s, "never")  == 0) s->restore = SESSION_RESTORE_NEVER;
    else config_report_error(cfg, "[session] unknown restore \"%s\" — using \"crash\"", d.u.s);
    free(d.u.s);
}

/* ── binds section ───────────────────────────────────────────────────── */

/* Built-in binds, installed whenever the config file yielded no usable ones
 * (missing file, TOML syntax error, empty or entirely broken [binds]). Without
 * them a single typo — a forgotten quote — leaves a running compositor that
 * cannot spawn a terminal, switch desktops or exit. Mirrors the defaults in
 * config.toml.example; keep the two in sync. */
static const struct { const char *bind; const char *action; } default_binds[] = {
    { "super+Return",         "spawn:kitty"      },
    { "super+space",          "launcher"         },
    { "super+q",              "killclient"       },
    { "super+t",              "toggle_tiling"    },
    { "super+alt+space",      "toggle_floating"  },
    { "super+d",              "fake_fullscreen"  },
    { "super+f",              "real_fullscreen"  },
    { "super+h",              "move_camera:-50"  },
    { "super+l",              "move_camera:50"   },
    { "super+p",              "pin_window"       },
    { "super+n",              "toggle_nocollide" },
    { "super+g",              "cycle_gravity"    },
    { "super+j",              "toggle_tray"      },
    { "super+r",              "spin_window"      },
    { "super+s",              "toggle_split"     },
    { "super+w",              "group_toggle"     },
    { "super+Tab",            "group_next"       },
    { "super+shift+Tab",      "group_prev"       },
    { "super+shift+w",        "group_add"        },
    /* Send the focused window to a desktop. Tiling has no other way out: the
     * layout owns the window's geometry, so it cannot be dragged across. */
    { "super+shift+1",        "move_to:0"        },
    { "super+shift+2",        "move_to:1"        },
    { "super+shift+3",        "move_to:2"        },
    { "super+shift+4",        "move_to:3"        },
    { "super+shift+5",        "move_to:4"        },
    { "super+shift+6",        "move_to:5"        },
    { "super+shift+7",        "move_to:6"        },
    { "super+shift+8",        "move_to:7"        },
    { "super+shift+9",        "move_to:8"        },
    { "super+shift+0",        "move_to:9"        },
    { "super+shift+c",        "calm_all"         },
    { "super+shift+r",        "reload_config"    },
    { "super+shift+p",        "wallpaper_picker" },
    { "super+shift+question", "show_hints"       },
    { "super+shift+Escape",   "EXIT"             },
    { "super+Left",           "tile_focus:l"     },
    { "super+Right",          "tile_focus:r"     },
    { "super+Up",             "tile_focus:u"     },
    { "super+Down",           "tile_focus:d"     },
    { "super+shift+Left",     "tile_move:l"      },
    { "super+shift+Right",    "tile_move:r"      },
    { "super+shift+Up",       "tile_move:u"      },
    { "super+shift+Down",     "tile_move:d"      },
    { "super+1",              "view:0"           },
    { "super+2",              "view:1"           },
    { "super+3",              "view:2"           },
    { "super+4",              "view:3"           },
    { "super+5",              "view:4"           },
    { "super+6",              "view:5"           },
    { "super+7",              "view:6"           },
    { "super+8",              "view:7"           },
    { "super+9",              "view:8"           },
    { "super+0",              "view:9"           },
};

static void apply_default_binds(FwmConfig *cfg) {
    int n = (int)(sizeof(default_binds) / sizeof(default_binds[0]));
    free(cfg->keys);
    cfg->keys = calloc(n, sizeof(KeyBind));
    cfg->key_count = 0;
    if (!cfg->keys) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        unsigned int mod;
        xkb_keysym_t key;
        if (!parse_bind_key(default_binds[i].bind, &mod, &key)) continue;
        cfg->keys[idx].mod = mod;
        cfg->keys[idx].key = key;
        snprintf(cfg->keys[idx].action, sizeof(cfg->keys[idx].action), "%s",
                 default_binds[i].action);
        idx++;
    }
    cfg->key_count     = idx;
    cfg->fallback_binds = 1;
}

static void load_binds(toml_table_t *root, FwmConfig *cfg) {
    cfg->keys      = NULL;
    cfg->key_count = 0;

    toml_table_t *tbl = toml_table_in(root, "binds");
    if (!tbl) {
        config_report_error(cfg, "no [binds] section — using built-in keybindings");
        apply_default_binds(cfg);
        return;
    }

    int n = toml_table_nkval(tbl);
    if (n <= 0) {
        config_report_error(cfg, "[binds] is empty — using built-in keybindings");
        apply_default_binds(cfg);
        return;
    }

    cfg->keys = calloc(n, sizeof(KeyBind));
    if (!cfg->keys) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        const char *bind_str = toml_key_in(tbl, i);
        if (!bind_str) continue;

        toml_datum_t val = toml_string_in(tbl, bind_str);
        if (!val.ok) {
            config_report_error(cfg, "[binds] \"%s\": value must be a quoted string", bind_str);
            continue;
        }

        unsigned int mod;
        xkb_keysym_t key;
        if (!parse_bind_key(bind_str, &mod, &key)) {
            config_report_error(cfg, "[binds] \"%s\": unknown key or modifier", bind_str);
            free(val.u.s);
            continue;
        }
        if (!action_is_known(val.u.s)) {
            config_report_error(cfg, "[binds] \"%s\": unknown action \"%s\"", bind_str, val.u.s);
            free(val.u.s);
            continue;
        }

        cfg->keys[idx].mod = mod;
        cfg->keys[idx].key = key;
        strncpy(cfg->keys[idx].action, val.u.s, sizeof(cfg->keys[idx].action) - 1);
        free(val.u.s);
        idx++;
    }
    cfg->key_count = idx;

    /* Every line was broken: fall back rather than hand the user a compositor
     * with no way to open a terminal or quit. */
    if (idx == 0) {
        config_report_error(cfg, "no usable binds in [binds] — using built-in keybindings");
        apply_default_binds(cfg);
    }
}

/* ── modes (submaps) ─────────────────────────────────────────────────── */

int config_mode_find(const FwmConfig *cfg, const char *name) {
    if (!name) return -1;
    /* Callers hold either "mode:resize" or "resize"; accept both rather than
     * making every one of them skip the prefix itself. */
    if (strncmp(name, FWM_MODE_ACTION, strlen(FWM_MODE_ACTION)) == 0)
        name += strlen(FWM_MODE_ACTION);
    for (int i = 0; i < cfg->mode_count; i++)
        if (strcmp(cfg->modes[i].name, name) == 0) return i;
    return -1;
}

const KeyBind *config_match_mode_bind(const FwmConfig *cfg, int mode,
                                      xkb_keysym_t sym, unsigned int mods) {
    if (mode < 0 || mode >= cfg->mode_count) return NULL;
    const ConfigMode *m = &cfg->modes[mode];
    xkb_keysym_t lower = xkb_keysym_to_lower(sym);
    for (int i = 0; i < m->key_count; i++) {
        if (m->keys[i].mod != mods) continue;
        if (m->keys[i].key == sym || m->keys[i].key == lower) return &m->keys[i];
    }
    return NULL;
}

/* One [mode.<name>] table: its binds, plus the two words that are settings
 * rather than binds. */
static void load_mode(FwmConfig *cfg, toml_table_t *tbl, const char *name) {
    if (cfg->mode_count >= CONFIG_MAX_MODES) {
        config_report_error(cfg, "[mode.%s]: too many modes (max %d) — ignored",
                            name, CONFIG_MAX_MODES);
        return;
    }
    ConfigMode *m = &cfg->modes[cfg->mode_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);

    toml_datum_t st = toml_bool_in(tbl, "sticky");
    if (st.ok) m->sticky = st.u.b ? 1 : 0;

    int n = toml_table_nkval(tbl);
    m->keys = calloc(n > 0 ? n : 1, sizeof(KeyBind));
    if (!m->keys) { perror("calloc"); return; }

    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;
        if (strcmp(key, "sticky") == 0) continue;

        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[mode.%s] \"%s\": value must be a quoted string",
                                name, key);
            continue;
        }

        /* `enter` is not a bind of the mode but a bind INTO it, so it goes into
         * the root map — appended after [binds] has been read, below. */
        if (strcmp(key, "enter") == 0) { free(val.u.s); continue; }

        unsigned int mod;
        xkb_keysym_t sym;
        if (!parse_bind_key(key, &mod, &sym)) {
            config_report_error(cfg, "[mode.%s] \"%s\": unknown key or modifier", name, key);
            free(val.u.s);
            continue;
        }
        if (!action_is_known(val.u.s)) {
            config_report_error(cfg, "[mode.%s] \"%s\": unknown action \"%s\"",
                                name, key, val.u.s);
            free(val.u.s);
            continue;
        }
        m->keys[m->key_count].mod = mod;
        m->keys[m->key_count].key = sym;
        snprintf(m->keys[m->key_count].action, sizeof(m->keys[m->key_count].action),
                 "%s", val.u.s);
        m->key_count++;
        free(val.u.s);
    }

    if (m->key_count == 0) {
        /* A mode with nothing in it can still be entered, and then only Escape
         * gets you out — a trap rather than a feature. */
        config_report_error(cfg, "[mode.%s]: no binds — mode ignored", name);
        free(m->keys);
        m->keys = NULL;
        return;
    }
    cfg->mode_count++;
}

/* Append one bind to the root map. Used for each mode's `enter` key, which is
 * written inside the mode but belongs to the map you press it from. */
static void add_root_bind(FwmConfig *cfg, unsigned int mod, xkb_keysym_t key,
                          const char *action) {
    KeyBind *grown = realloc(cfg->keys, (size_t)(cfg->key_count + 1) * sizeof(KeyBind));
    if (!grown) { perror("realloc"); return; }
    cfg->keys = grown;
    KeyBind *b = &cfg->keys[cfg->key_count++];
    memset(b, 0, sizeof(*b));
    b->mod = mod;
    b->key = key;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

static void load_modes(toml_table_t *root, FwmConfig *cfg) {
    cfg->mode_count = 0;
    toml_table_t *tbl = root ? toml_table_in(root, "mode") : NULL;
    if (!tbl) return;

    for (int i = 0; ; i++) {
        const char *name = toml_key_in(tbl, i);
        if (!name) break;
        toml_table_t *sub = toml_table_in(tbl, name);
        if (!sub) {
            config_report_error(cfg, "[mode] \"%s\": modes are tables, e.g. [mode.%s]",
                                name, name);
            continue;
        }
        load_mode(cfg, sub, name);
    }

    /* Now that every mode exists and [binds] has been read, wire up the keys
     * that step into them. Done second so `enter` can be reported against a
     * mode that was itself dropped for being empty. */
    for (int i = 0; ; i++) {
        const char *name = toml_key_in(tbl, i);
        if (!name) break;
        toml_table_t *sub = toml_table_in(tbl, name);
        if (!sub) continue;
        toml_datum_t e = toml_string_in(sub, "enter");
        if (!e.ok) continue;

        if (config_mode_find(cfg, name) < 0) {
            free(e.u.s);
            continue;   /* the mode was dropped; it already said why */
        }
        unsigned int mod;
        xkb_keysym_t sym;
        if (!parse_bind_key(e.u.s, &mod, &sym)) {
            config_report_error(cfg, "[mode.%s] enter = \"%s\": unknown key or modifier",
                                name, e.u.s);
            free(e.u.s);
            continue;
        }
        char action[64];
        snprintf(action, sizeof(action), FWM_MODE_ACTION "%s", name);
        add_root_bind(cfg, mod, sym, action);
        free(e.u.s);
    }
}

/* ── mouse section ───────────────────────────────────────────────────── */

/* "super+shift+left" -> mods + FWM_BTN_*. Shares parse_mod_token with the
 * keyboard, so the modifier spelling is the same in both tables. */
static int parse_mouse_key(const char *str, unsigned int *mod_out, int *btn_out) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", str);

    char *tokens[8];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t && n < 8; t = strtok_r(NULL, "+", &save))
        tokens[n++] = t;
    if (n == 0) return 0;

    *mod_out = 0;
    for (int i = 0; i < n - 1; i++) {
        unsigned int m = parse_mod_token(tokens[i]);
        if (!m) return 0;   /* an unknown word here is a typo, not a button */
        *mod_out |= m;
    }

    const char *b = tokens[n - 1];
    if      (strcmp(b, "left")   == 0) *btn_out = FWM_BTN_LEFT;
    else if (strcmp(b, "right")  == 0) *btn_out = FWM_BTN_RIGHT;
    else if (strcmp(b, "middle") == 0) *btn_out = FWM_BTN_MIDDLE;
    else if (strcmp(b, "side")   == 0) *btn_out = FWM_BTN_SIDE;
    else if (strcmp(b, "extra")  == 0) *btn_out = FWM_BTN_EXTRA;
    else return 0;
    return 1;
}

int config_action_is_drag(const char *action) {
    return strcmp(action, FWM_MOUSE_MOVE) == 0
        || strcmp(action, FWM_MOUSE_MOVE_NOCOLLIDE) == 0
        || strcmp(action, FWM_MOUSE_RESIZE) == 0
        || strcmp(action, FWM_MOUSE_SWAP) == 0
        || strcmp(action, FWM_MOUSE_TWIST) == 0;
}

static void add_mouse_bind(MouseConfig *mc, unsigned int mod, int button, const char *action) {
    if (mc->bind_count >= CONFIG_MAX_MOUSE) return;
    MouseBind *b = &mc->binds[mc->bind_count++];
    b->mod    = mod;
    b->button = button;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

/* What the button handler did before any of this was configurable. Also what
 * you get back by deleting the [mouse] section. */
static const struct { const char *bind; const char *action; } default_mouse[] = {
    { "super+left",       FWM_MOUSE_MOVE },
    { "super+shift+left", FWM_MOUSE_MOVE_NOCOLLIDE },
    { "super+right",      FWM_MOUSE_RESIZE },
};

static void apply_default_mouse(FwmConfig *cfg) {
    cfg->mouse.bind_count = 0;
    for (size_t i = 0; i < sizeof(default_mouse) / sizeof(default_mouse[0]); i++) {
        unsigned int mod;
        int btn;
        if (!parse_mouse_key(default_mouse[i].bind, &mod, &btn)) continue;
        add_mouse_bind(&cfg->mouse, mod, btn, default_mouse[i].action);
    }
}

static void load_mouse(toml_table_t *root, FwmConfig *cfg) {
    toml_table_t *tbl = root ? toml_table_in(root, "mouse") : NULL;
    if (!tbl) { apply_default_mouse(cfg); return; }

    cfg->mouse.bind_count = 0;
    int n = toml_table_nkval(tbl);
    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;

        unsigned int mod;
        int btn;
        if (!parse_mouse_key(key, &mod, &btn)) {
            config_report_error(cfg, "[mouse] \"%s\": not a button "
                                "(want e.g. \"super+left\" or \"super+shift+right\")", key);
            continue;
        }
        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[mouse] \"%s\": value must be a quoted string", key);
            continue;
        }
        if (!config_action_is_drag(val.u.s) && !action_is_known(val.u.s)) {
            config_report_error(cfg, "[mouse] \"%s\": unknown action \"%s\"", key, val.u.s);
            free(val.u.s);
            continue;
        }
        if (cfg->mouse.bind_count >= CONFIG_MAX_MOUSE) {
            config_report_error(cfg, "too many [mouse] entries — only the first %d are used",
                                CONFIG_MAX_MOUSE);
            free(val.u.s);
            break;
        }
        add_mouse_bind(&cfg->mouse, mod, btn, val.u.s);
        free(val.u.s);
    }

    /* An empty or wholly broken [mouse] table leaves a compositor whose windows
     * cannot be moved with the mouse at all. That is a legitimate thing to want
     * — someone may drive everything from the keyboard — so it is honoured
     * rather than overridden; the errors above say what went wrong. */
}

const MouseBind *config_match_mouse(const FwmConfig *cfg, int button, unsigned int mods) {
    for (int i = 0; i < cfg->mouse.bind_count; i++) {
        const MouseBind *b = &cfg->mouse.binds[i];
        if (b->button == button && b->mod == mods) return b;
    }
    return NULL;
}

/* ── gestures section ────────────────────────────────────────────────── */

/* The gesture vocabulary is [binds]' plus the one action that only a gesture
 * can express. */
static int gesture_action_is_known(const char *a) {
    return action_is_known(a) || strcmp(a, GESTURE_ACTION_PAN) == 0;
}

/* "swipe3+left", "pinch2+in". Returns 0 on anything else. */
static int parse_gesture_key(const char *str, int *fingers_out, int *dir_out) {
    int swipe;
    if      (strncmp(str, "swipe", 5) == 0) swipe = 1;
    else if (strncmp(str, "pinch", 5) == 0) swipe = 0;
    else return 0;

    char *end;
    long n = strtol(str + 5, &end, 10);
    if (end == str + 5 || *end != '+') return 0;
    /* libinput reports 2..5; a one-finger "swipe" is just pointer motion. */
    if (n < 2 || n > 5) return 0;

    const char *dir = end + 1;
    int d;
    if (swipe) {
        if      (strcmp(dir, "left")  == 0) d = GESTURE_SWIPE_LEFT;
        else if (strcmp(dir, "right") == 0) d = GESTURE_SWIPE_RIGHT;
        else if (strcmp(dir, "up")    == 0) d = GESTURE_SWIPE_UP;
        else if (strcmp(dir, "down")  == 0) d = GESTURE_SWIPE_DOWN;
        else return 0;
    } else {
        if      (strcmp(dir, "in")  == 0) d = GESTURE_PINCH_IN;
        else if (strcmp(dir, "out") == 0) d = GESTURE_PINCH_OUT;
        else return 0;
    }

    *fingers_out = (int)n;
    *dir_out     = d;
    return 1;
}

static void add_gesture(GesturesConfig *g, int fingers, int dir, const char *action) {
    if (g->bind_count >= CONFIG_MAX_GESTURES) return;
    GestureBind *b = &g->binds[g->bind_count++];
    b->fingers = fingers;
    b->dir     = dir;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

static void load_gestures(toml_table_t *root, FwmConfig *cfg) {
    GesturesConfig *g = &cfg->gestures;
    g->sensitivity = 1.0;
    g->natural     = 1;
    g->bind_count  = 0;

    /* No gestures unless the config asks for them, by name. Unlike [binds] —
     * where an empty table would leave a compositor nobody can drive, so the
     * built-ins step in — a gesture that nobody asked for is a surprise: it
     * takes a swipe away from the application under the cursor, and on hardware
     * that reports its fingers coarsely it can fire on a stray palm. The set
     * worth copying is written out (commented) in config.toml.example. */
    toml_table_t *tbl = root ? toml_table_in(root, "gestures") : NULL;
    if (!tbl) return;

    toml_datum_t s = toml_double_in(tbl, "sensitivity");
    if (s.ok) {
        if (s.u.d > 0.0 && s.u.d <= 10.0) g->sensitivity = s.u.d;
        else config_report_error(cfg, "[gestures] sensitivity %.3f out of range 0..10 — using 1.0", s.u.d);
    }
    toml_datum_t nat = toml_bool_in(tbl, "natural");
    if (nat.ok) g->natural = nat.u.b ? 1 : 0;

    int n = toml_table_nkval(tbl);
    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;
        /* The two scalars share the table with the binds. */
        if (strcmp(key, "sensitivity") == 0 || strcmp(key, "natural") == 0) continue;

        int fingers, dir;
        if (!parse_gesture_key(key, &fingers, &dir)) {
            config_report_error(cfg, "[gestures] \"%s\": not a gesture "
                                "(want e.g. \"swipe3+left\" or \"pinch2+in\")", key);
            continue;
        }
        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[gestures] \"%s\": value must be a quoted string", key);
            continue;
        }
        if (!gesture_action_is_known(val.u.s)) {
            config_report_error(cfg, "[gestures] \"%s\": unknown action \"%s\"", key, val.u.s);
            free(val.u.s);
            continue;
        }
        if (strcmp(val.u.s, GESTURE_ACTION_PAN) == 0 &&
            dir != GESTURE_SWIPE_LEFT && dir != GESTURE_SWIPE_RIGHT) {
            /* The desktop strip runs left to right; there is nothing above it
             * to pan to. */
            config_report_error(cfg, "[gestures] \"%s\": " GESTURE_ACTION_PAN
                                " needs a horizontal swipe", key);
            free(val.u.s);
            continue;
        }
        if (g->bind_count >= CONFIG_MAX_GESTURES) {
            config_report_error(cfg, "too many [gestures] entries — only the first %d are used",
                                CONFIG_MAX_GESTURES);
            free(val.u.s);
            break;
        }
        add_gesture(g, fingers, dir, val.u.s);
        free(val.u.s);
    }
}

/* ── public api ──────────────────────────────────────────────────────── */

/* Expand a leading "~/" — config paths are hand-written, and the shell that
 * would normally do this is not involved. */
static void expand_tilde(const char *in, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, cap, "%s%s", home, in + 1);
    else
        snprintf(out, cap, "%s", in);
}

static void load_wallpaper_picker(toml_table_t *root, FwmConfig *cfg) {
    expand_tilde("~/Pictures", cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir));
    cfg->wallpaper_picker_fps = 0.0; /* 0 = clip's own rate */
    if (!root) return;

    toml_table_t *tbl = toml_table_in(root, "wallpaper_picker");
    if (!tbl) return;

    /* `dir` and `fps` are independent — parse both, so an fps with no dir (or a
     * dir with no fps) still takes effect. */
    toml_datum_t d = toml_string_in(tbl, "dir");
    if (d.ok) {
        expand_tilde(d.u.s, cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir));
        free(d.u.s);
        if (access(cfg->wallpaper_dir, R_OK | X_OK) != 0)
            config_report_error(cfg, "[wallpaper_picker] dir: cannot read \"%s\"",
                                cfg->wallpaper_dir);
    }

    toml_datum_t f = toml_double_in(tbl, "fps");
    if (f.ok) cfg->wallpaper_picker_fps = f.u.d;
}

static void load_wallpaper(toml_table_t *root, FwmConfig *cfg) {
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;

    toml_array_t *arr = toml_array_in(root, "wallpaper");
    if (!arr) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;

    cfg->wallpapers = calloc(n, sizeof(WallpaperLayer));
    if (!cfg->wallpapers) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        toml_table_t *tbl = toml_table_at(arr, i);
        if (!tbl) continue;

        toml_datum_t path = toml_string_in(tbl, "path");
        if (!path.ok) {
            config_report_error(cfg, "[[wallpaper]] #%d: missing or unquoted \"path\"", i + 1);
            continue;
        }
        if (access(path.u.s, R_OK) != 0) {
            config_report_error(cfg, "[[wallpaper]] #%d: cannot read \"%s\"", i + 1, path.u.s);
            free(path.u.s);
            continue;
        }

        strncpy(cfg->wallpapers[idx].path, path.u.s, sizeof(cfg->wallpapers[idx].path) - 1);
        free(path.u.s);

        toml_datum_t fit = toml_string_in(tbl, "fit");
        int mode = WALLPAPER_FIT_COVER;
        if (fit.ok) {
            if (strcmp(fit.u.s, "contain") == 0)   mode = WALLPAPER_FIT_CONTAIN;
            else if (strcmp(fit.u.s, "pan") == 0)  mode = WALLPAPER_FIT_PAN;
            else if (strcmp(fit.u.s, "video") == 0) mode = WALLPAPER_FIT_VIDEO;
            else if (strcmp(fit.u.s, "cover") != 0)
                config_report_error(cfg, "[[wallpaper]] #%d: unknown fit \"%s\" — using cover",
                        i + 1, fit.u.s);
            free(fit.u.s);
        }
        cfg->wallpapers[idx].fit = mode;

        toml_datum_t crop = toml_double_in(tbl, "pan_crop");
        double pc = crop.ok ? crop.u.d : 0.0;
        if (pc < 0.0) pc = 0.0;
        if (pc > 0.9) pc = 0.9;   /* past this nothing recognisable is left */
        cfg->wallpapers[idx].pan_crop = pc;

        toml_datum_t zoom = toml_double_in(tbl, "zoom");
        cfg->wallpapers[idx].zoom = zoom.ok ? zoom.u.d : 0.0; /* 0 = auto (native) */

        toml_datum_t fps = toml_double_in(tbl, "fps");
        cfg->wallpapers[idx].fps = fps.ok ? fps.u.d : 0.0; /* 0 = source rate */

        idx++;
    }
    cfg->wallpaper_count = idx;
}

/* ── window rules ────────────────────────────────────────────────────── */

/* Compile one matcher. Returns 1 if the pattern was present AND valid; a
 * broken regex is reported and the rule simply loses that matcher — but a rule
 * left with NO matchers is dropped entirely by the caller, because a matcher-
 * less rule would silently apply to every window. */
static int compile_matcher(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                           int index, regex_t *re, char *pat, size_t patcap,
                           int *present) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (!d.ok) return 0;
    *present = 1;

    int rc = regcomp(re, d.u.s, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char reason[128];
        regerror(rc, re, reason, sizeof(reason));
        config_report_error(cfg, "[[rule]] #%d: bad %s regex \"%s\" — %s",
                            index + 1, key, d.u.s, reason);
        free(d.u.s);
        return 0;
    }
    snprintf(pat, patcap, "%s", d.u.s);
    free(d.u.s);
    return 1;
}

/* Reads an optional boolean property into a tri-state: -1 keeps "unset", so a
 * later rule can leave an earlier rule's decision standing. */
static int rule_tristate(toml_table_t *tbl, const char *key) {
    toml_datum_t d = toml_bool_in(tbl, key);
    return d.ok ? (d.u.b ? 1 : 0) : -1;
}

/* The same idea for a number, where -1 is a value someone may well mean
 * (gravity = -1 is a window that falls upward). NAN is the "unset" state, and
 * an integer in the file is accepted as the double it obviously is — writing
 * `mass = 8` and getting silence would be a mean trick. */
static double rule_number(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                          int index, double min, double max) {
    toml_datum_t d = toml_double_in(tbl, key);
    if (!d.ok) {
        toml_datum_t di = toml_int_in(tbl, key);
        if (!di.ok) return NAN;
        d.u.d = (double)di.u.i;
    }
    if (d.u.d < min || d.u.d > max) {
        config_report_error(cfg, "[[rule]] #%d: %s %g out of range %g..%g — ignored",
                            index + 1, key, d.u.d, min, max);
        return NAN;
    }
    return d.u.d;
}

static void load_rules(toml_table_t *root, FwmConfig *cfg) {
    cfg->rules      = NULL;
    cfg->rule_count = 0;

    toml_array_t *arr = toml_array_in(root, "rule");
    if (!arr) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;
    if (n > CONFIG_MAX_RULES) {
        config_report_error(cfg, "too many [[rule]] entries (%d) — only the first %d are used",
                            n, CONFIG_MAX_RULES);
        n = CONFIG_MAX_RULES;
    }

    cfg->rules = calloc(n, sizeof(ConfigRule));
    if (!cfg->rules) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        toml_table_t *tbl = toml_table_at(arr, i);
        if (!tbl) continue;

        ConfigRule *r = &cfg->rules[idx];
        int present = 0;
        r->has_app_id = compile_matcher(cfg, tbl, "app_id", i, &r->re_app_id,
                                        r->pat_app_id, sizeof(r->pat_app_id), &present);
        r->has_title  = compile_matcher(cfg, tbl, "title", i, &r->re_title,
                                        r->pat_title, sizeof(r->pat_title), &present);

        if (!r->has_app_id && !r->has_title) {
            /* Only complain about the absence when the user never wrote a
             * matcher. If one was written but would not compile, the regex
             * error above is the actionable message — saying it twice would
             * spend two of the tray pill's slots on a single typo. */
            if (!present)
                config_report_error(cfg, "[[rule]] #%d: no app_id/title matcher — rule ignored",
                                    i + 1);
            continue;   /* nothing compiled, so nothing to regfree */
        }

        r->nocollide = rule_tristate(tbl, "nocollide");
        r->pin       = rule_tristate(tbl, "pin");

        r->desktop = -1;
        toml_datum_t desk = toml_int_in(tbl, "desktop");
        if (desk.ok) {
            if (desk.u.i < 0 || desk.u.i > 9)
                config_report_error(cfg, "[[rule]] #%d: desktop %lld out of range 0..9 — ignored",
                                    i + 1, (long long)desk.u.i);
            else
                r->desktop = (int)desk.u.i;
        }

        /* Material. Ranges are wide on purpose — a window 100x heavier than
         * normal or one that falls upward is a thing someone may genuinely
         * want — and only bar the values that would break the simulation
         * outright (a massless body, restitution over 1 gaining energy on
         * every bounce, retention over 1 accelerating for free). */
        r->mass     = rule_number(cfg, tbl, "mass",     i, 0.001, 1000.0);
        r->gravity  = rule_number(cfg, tbl, "gravity",  i, -100.0, 100.0);
        r->bounce   = rule_number(cfg, tbl, "bounce",   i, 0.0, 1.0);
        r->friction = rule_number(cfg, tbl, "friction", i, 0.0, 1.0);

        if (r->nocollide < 0 && r->pin < 0 && r->desktop < 0 &&
            isnan(r->mass) && isnan(r->gravity) && isnan(r->bounce) && isnan(r->friction))
            config_report_error(cfg, "[[rule]] #%d: matches but sets nothing", i + 1);

        idx++;
    }
    cfg->rule_count = idx;
}

int config_match_rules(const FwmConfig *cfg, const char *app_id, const char *title,
                       ConfigRule *out) {
    out->nocollide = -1;
    out->pin       = -1;
    out->desktop   = -1;
    out->mass = out->gravity = out->bounce = out->friction = NAN;

    int matched = 0;
    for (int i = 0; i < cfg->rule_count; i++) {
        const ConfigRule *r = &cfg->rules[i];

        /* Every matcher the rule declares has to hit. A window with no app_id
         * (some X11 clients) can never satisfy an app_id matcher. */
        if (r->has_app_id) {
            if (!app_id || regexec(&r->re_app_id, app_id, 0, NULL, 0) != 0) continue;
        }
        if (r->has_title) {
            if (!title || regexec(&r->re_title, title, 0, NULL, 0) != 0) continue;
        }

        /* Later rules override earlier ones field by field. */
        if (r->nocollide >= 0) out->nocollide = r->nocollide;
        if (r->pin       >= 0) out->pin       = r->pin;
        if (r->desktop   >= 0) out->desktop   = r->desktop;
        if (!isnan(r->mass))     out->mass     = r->mass;
        if (!isnan(r->gravity))  out->gravity  = r->gravity;
        if (!isnan(r->bounce))   out->bounce   = r->bounce;
        if (!isnan(r->friction)) out->friction = r->friction;
        matched = 1;
    }
    return matched;
}

/* ── runtime-settable options ────────────────────────────────────────── */

/* physics.tick_rate is deliberately absent: the tick timer is armed once at
 * startup, so accepting a new value here would report success and change
 * nothing. Same reasoning excludes the string options (icon_theme, kbd_*) —
 * they are re-read only by a full reload. */
static const ConfigOption config_option_table[] = {
    { "physics.friction",               CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.friction),               0.0,     1.0,     "velocity retained per tick" },
    { "physics.mass_density",           CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.mass_density),            0.0,     1.0,     "mass per pixel of window area" },
    { "physics.throw_speed_multiplier", CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.throw_speed_multiplier),  0.0,    10.0,     "how hard a drag throws" },
    { "physics.max_throw_speed",        CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.max_throw_speed),         0.0, 100000.0,    "throw speed cap, px/s" },
    { "physics.stop_speed_threshold",   CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.stop_speed_threshold),    0.0,  1000.0,    "below this a body is put to sleep" },
    { "physics.restitution",            CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.restitution),             0.0,     1.0,     "bounciness, 0 = dead stop" },
    { "physics.gravity",                CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.gravity),           -100000.0, 100000.0,    "px/s^2; 981 = earth at 100px/m" },

    { "tiling.gaps_in",                 CFG_OPT_INT,    offsetof(FwmConfig, tiling.gaps_in),                  0.0,   500.0,    "gap between tiles, px" },
    { "tiling.gaps_out",                CFG_OPT_INT,    offsetof(FwmConfig, tiling.gaps_out),                 0.0,   500.0,    "gap to the screen edge, px" },
    { "tiling.anim_speed",              CFG_OPT_DOUBLE, offsetof(FwmConfig, tiling.anim_speed),               0.0,  1000.0,    "tile-glide rate, 1/s; 0 disables" },

    { "camera.anim_ms",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.anim_ms),                  0.0, 10000.0,    "desktop-switch slide, ms" },
    { "camera.free_speed",              CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.free_speed),               0.0,  1000.0,    "held move_camera chase rate, 1/s" },

    { "decor.border_width",             CFG_OPT_INT,    offsetof(FwmConfig, decor.border_width),              0.0,    64.0,    "focus border, px; 0 disables" },
    { "decor.col_active",               CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_active),                0.0,     0.0,    "focused border colour" },
    { "decor.col_inactive",             CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_inactive),              0.0,     0.0,    "unfocused border colour" },
    { "decor.fade_in_ms",               CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.fade_in_ms),                0.0, 10000.0,    "window open animation, ms" },
    { "decor.wallpaper_fade_ms",        CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.wallpaper_fade_ms),         0.0, 10000.0,    "wallpaper cross-fade, ms" },
    { "decor.tray_opacity",             CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tray_opacity),              0.0,     1.0,    "tray island fill alpha" },
    { "decor.launcher_opacity",         CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.launcher_opacity),          0.0,     1.0,    "launcher island fill alpha" },
    { "decor.tint_strength",            CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tint_strength),             0.0,     1.0,    "island tint toward the wallpaper hue" },

    { "effects.camera_shake",           CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.camera_shake),            0.0,     4.0,    "impact shake; 0 disables" },
    { "effects.squash",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.squash),                  0.0,     4.0,    "impact squash & stretch; 0 disables" },
    { "effects.jelly",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.jelly),                   0.0,     4.0,    "drag wobble; 0 disables" },
    { "effects.spin",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.spin),                    0.0,     4.0,    "free rotation kick (experimental); 0 disables" },
    { "effects.live",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.live),                    0.0,     1.0,    "live content under spin/wobble; 0 = still frame" },

    { "input.repeat_rate",              CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_rate),               0.0,   200.0,    "key repeat, chars/s" },
    { "input.repeat_delay",             CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_delay),              0.0,  5000.0,    "ms before repeat starts" },

    { "gestures.sensitivity",           CFG_OPT_DOUBLE, offsetof(FwmConfig, gestures.sensitivity),            0.1,    10.0,    "camera px per finger px" },
    { "gestures.natural",               CFG_OPT_INT,    offsetof(FwmConfig, gestures.natural),                0.0,     1.0,    "1 = the strip follows the fingers" },
};

const ConfigOption *config_options(int *count) {
    if (count) *count = (int)(sizeof(config_option_table) / sizeof(config_option_table[0]));
    return config_option_table;
}

const ConfigOption *config_option_find(const char *name) {
    if (!name) return NULL;
    int n;
    const ConfigOption *tbl = config_options(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(tbl[i].name, name) == 0) return &tbl[i];
    return NULL;
}

int config_option_set(FwmConfig *cfg, const ConfigOption *opt,
                      const char *value, char *err, size_t errcap) {
    if (!value || !*value) {
        snprintf(err, errcap, "%s needs a value", opt->name);
        return 0;
    }
    char *field = (char *)cfg + opt->offset;

    if (opt->type == CFG_OPT_COLOR) {
        float rgba[4];
        if (!parse_hex_color(value, rgba)) {
            snprintf(err, errcap, "%s: expected #RRGGBB or #RRGGBBAA, got \"%s\"",
                     opt->name, value);
            return 0;
        }
        memcpy(field, rgba, sizeof(rgba));
        return 1;
    }

    char *end;
    double v = strtod(value, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == value || *end) {
        snprintf(err, errcap, "%s: expected a number, got \"%s\"", opt->name, value);
        return 0;
    }
    /* Rejected rather than clamped: over a socket a silent clamp is
     * indistinguishable from the value having been accepted. */
    if (v < opt->min || v > opt->max) {
        snprintf(err, errcap, "%s: %g is outside %g..%g", opt->name, v, opt->min, opt->max);
        return 0;
    }

    if (opt->type == CFG_OPT_INT) *(int *)field = (int)v;
    else                          *(double *)field = v;
    return 1;
}

void config_option_get(const FwmConfig *cfg, const ConfigOption *opt,
                       char *out, size_t cap) {
    const char *field = (const char *)cfg + opt->offset;

    switch (opt->type) {
    case CFG_OPT_INT:
        snprintf(out, cap, "%d", *(const int *)field);
        break;
    case CFG_OPT_COLOR: {
        /* parse_hex_color stores premultiplied alpha for wlr_scene_rect, so
         * undo that on the way out or every colour reads back darkened. */
        const float *c = (const float *)field;
        float a = c[3];
        float r = a > 0.0f ? c[0] / a : 0.0f;
        float g = a > 0.0f ? c[1] / a : 0.0f;
        float b = a > 0.0f ? c[2] / a : 0.0f;
        snprintf(out, cap, "#%02X%02X%02X%02X",
                 (unsigned)(r * 255.0f + 0.5f), (unsigned)(g * 255.0f + 0.5f),
                 (unsigned)(b * 255.0f + 0.5f), (unsigned)(a * 255.0f + 0.5f));
        break;
    }
    default:
        snprintf(out, cap, "%g", *(const double *)field);
        break;
    }
}

void config_load(FwmConfig *cfg, const char *path) {
    cfg->physics         = physics_defaults;
    cfg->tiling          = (TilingConfig){ .gaps_in = 6, .gaps_out = 14, .anim_speed = 12.0 };
    cfg->camera          = (CameraConfig){ .anim_ms = 350.0, .free_speed = 14.0 };
    // Defaults for the no-config-file path; load_decor re-applies them anyway.
    cfg->decor.border_width = 2;
    parse_hex_color("#7aa2f7", cfg->decor.col_active);
    parse_hex_color("#3b4261", cfg->decor.col_inactive);
    cfg->decor.fade_in_ms = 260.0;
    cfg->decor.wallpaper_fade_ms = 420.0;
    cfg->decor.tray_opacity = 0.92;
    cfg->decor.launcher_opacity = 0.92;
    cfg->decor.icon_theme[0] = '\0';
    cfg->decor.color_source = COLOR_SOURCE_CONFIG;
    cfg->decor.tint_strength = 0.4;
    cfg->keys            = NULL;
    cfg->key_count       = 0;
    cfg->mode_count      = 0;
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
    cfg->rules           = NULL;
    cfg->rule_count      = 0;
    cfg->error_count     = 0;
    cfg->error_total     = 0;
    cfg->fallback_binds  = 0;
    snprintf(cfg->source, sizeof(cfg->source), "%s", path ? path : "");
    load_input(NULL, cfg); /* defaults for the no-config-file path */
    load_focus(NULL, &cfg->focus, cfg);
    load_effects(NULL, &cfg->effects);
    load_session(NULL, &cfg->session, cfg);
    load_gestures(NULL, cfg);
    load_mouse(NULL, cfg);   /* the built-in drag verbs, for every early-out below */

    FILE *f = fopen(path, "r");
    if (!f) {
        config_report_error(cfg, "cannot open %s — using defaults", path);
        apply_default_binds(cfg);
        load_wallpaper_picker(NULL, cfg);
        return;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    /* A syntax error used to abandon the whole load, leaving zero binds — a
     * running compositor the user could not control. Now the defaults stand in
     * and the tray reports the error. */
    if (!root) {
        config_report_error(cfg, "syntax error: %s", errbuf);
        config_report_error(cfg, "config ignored — using defaults and built-in keybindings");
        apply_default_binds(cfg);
        load_wallpaper_picker(NULL, cfg);
        return;
    }

    load_physics(root, cfg);
    load_tiling(root, &cfg->tiling);
    load_camera(root, &cfg->camera);
    load_decor(root, cfg);
    load_input(root, cfg);
    load_focus(root, &cfg->focus, cfg);
    load_effects(root, &cfg->effects);
    load_session(root, &cfg->session, cfg);
    load_binds(root, cfg);
    load_modes(root, cfg);   /* after [binds]: each mode's `enter` key joins the root map */
    load_mouse(root, cfg);
    load_gestures(root, cfg);
    load_wallpaper(root, cfg);
    load_wallpaper_picker(root, cfg);
    load_rules(root, cfg);

    toml_free(root);
}

void config_free(FwmConfig *cfg) {
    free(cfg->keys);
    cfg->keys      = NULL;
    cfg->key_count = 0;
    for (int i = 0; i < cfg->mode_count; i++) free(cfg->modes[i].keys);
    cfg->mode_count = 0;
    free(cfg->wallpapers);
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
    /* Compiled regexes own heap memory of their own: without regfree every
     * hot reload (super+shift+r) would leak one allocation per matcher. */
    for (int i = 0; i < cfg->rule_count; i++) {
        if (cfg->rules[i].has_app_id) regfree(&cfg->rules[i].re_app_id);
        if (cfg->rules[i].has_title)  regfree(&cfg->rules[i].re_title);
    }
    free(cfg->rules);
    cfg->rules      = NULL;
    cfg->rule_count = 0;
}

/* ── bind lookup ─────────────────────────────────────────────────────── */

const KeyBind *config_match_bind(const FwmConfig *cfg, xkb_keysym_t sym, unsigned int mods) {
    if (!cfg) return NULL;
    /* Compare case-insensitively: xkb_state_key_get_syms() reflects the live
     * CapsLock state, so with Caps Lock on a letter key resolves to its
     * uppercase keysym ('q' -> 'Q') while binds are parsed from lowercase
     * config strings. Without normalising, every letter bind silently stops
     * working the moment Caps Lock is on, while digit and Return binds — which
     * Caps Lock does not touch — keep working, which is a maddening way to
     * find out. */
    xkb_keysym_t want = xkb_keysym_to_lower(sym);
    for (int i = 0; i < cfg->key_count; i++) {
        const KeyBind *bind = &cfg->keys[i];
        if (xkb_keysym_to_lower(bind->key) == want && bind->mod == mods)
            return bind;
    }
    return NULL;
}

int config_action_is_repeatable(const char *action) {
    if (!action) return 0;
    return strncmp(action, "move_camera:", 12) == 0;
}
