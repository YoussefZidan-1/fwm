<p align="center">
  <img src="assets/logo-brackets.svg" width="230" alt="fwm logo">
</p>

<p align="center">
  <a href="https://github.com/iluaii/fwm/actions/workflows/ci.yml">
    <img src="https://github.com/iluaii/fwm/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

# fwm — Physics Window Manager (Wayland)

A Wayland compositor written in C (wlroots) where windows behave as physical objects with **mass, momentum, inertia, and velocity** — simulated by a real rigid-body engine ([Box2D](https://box2d.org/) v3). Drag a window and throw it — it slides, bounces off walls, stacks under gravity, and comes to rest like a real object.

This is the primary, actively developed version. The legacy X11 version lives on the [`x11`](https://github.com/iluaii/fwm/tree/x11) branch and is no longer supported.

---

## 🎬 Demonstration
![Demonstration](demo.gif)

[Full-quality demo (mp4, 1.8 MB)](https://github.com/iluaii/fwm/releases/download/v0.3.0/demo.mp4)

## Features

### Physics
- **Real rigid-body simulation** — powered by Box2D: impulse-based collisions, proper mass ratios, resting contact and sleeping. No hand-rolled solver artifacts.
- **Drag to throw** — velocity is sampled over the last frames, a quick flick sends windows flying naturally.
- **Gravity modes** — cycle `Super+G` between zero-g, space mode, and Earth gravity (9.8 m/s² at the compositor's 100 px/m scale). Windows fall, thud, and stack on the floor.
- **Realistic feel** — dull heavy bounces (restitution 0.3), contact friction, no mid-air braking under gravity; long weighty glides in zero-g.
- **Continuous collision** — fast throws never tunnel through walls.
- **Free rotation** (`Super+R`, experimental) — hands a window's rotation to the simulation: the collision box turns with the picture, so it tumbles off the walls and shoves its neighbours corner-first. See below for how it is spun.
- **Per-window toggles** — pin (`Super+P`), collision off (`Super+N`), calm everything (`Super+Shift+C`).
- **What a window is made of** — `[[rule]]` sets `mass`, `bounce`, `friction` and `gravity` per window, so a video player can be eight times as heavy as a terminal and a terminal can be a balloon that drifts upward. The properties travel with the window, not with where it happens to be.
- **Physics per desktop** — a `[physics.<name>]` profile writes gravity, friction, restitution or density and names the desktops it applies to. Desktop 4 on the moon, desktop 8 underwater; drag a window across the edge and it changes as it goes. `Super+G` remains the master switch, and what it cycles through is `gravity_steps` in the config.

### Free rotation (experimental)

`Super+R` does not spin a window so much as let go of it: the press itself is a
nudge of about a quarter turn. The spinning comes from what you do with the
window afterwards.

Dragged, a spinning window hangs from the point you took hold of, the way a real
object held there would. Take it by a corner and it swings behind your hand,
settles hanging straight down if gravity is on, and whirls when you flick your
wrist; take it dead center and it does not swing at all. Stirring the mouse in
circles winds it up further, and it keeps whatever spin it had when you let go.
`spin_all` does the same for every window at once (unbound by default).

A window keeps playing while it turns. A window that is a single surface is
rotated straight from the texture the client just drew, at no cost at all;
anything else — and that includes plenty of ordinary clients, which draw
through a subsurface without ever showing one — is flattened into a picture
that is retaken whenever the client draws, capped at 30 times a second. Either
way the client keeps receiving frame callbacks while it is hidden, so it keeps
drawing. `effects.live = 0` turns all of that off and goes back to a still
frame refreshed a few times a second, for hardware that would rather not pay.

One thing does not change: a window is only as free to turn as the room around
it. One whose diagonal is taller than the screen wedges against the floor and
ceiling instead of coming round.

**Turning one by hand.** Bind `twist` in `[mouse]` — say
`"super+ctrl+left" = "twist"` — and a window follows the angle from its own
centre to the cursor. Stir and it winds up; let go and it keeps spinning at the
rate your hand was turning it.

### World
- **10 virtual desktops** on one continuous strip — the world is 10 screens wide, windows keep absolute positions.
- **Three modes per desktop** — physics (default), BSP tiling (`Super+T`), and floating (`Super+Alt+Space`), where windows stay exactly where you drop them and overlap freely, like an ordinary desktop environment. Each desktop picks its own.
- **Smooth camera** — scroll with `Super+H`/`Super+L` (hold to repeat), jump with `Super+1`…`0`.
- **Send windows across** — `Super+Shift+1`…`0`. Under physics and floating you can also just drag a window past the screen edge; a tiled window's geometry belongs to the layout, so this is the way out of tiling.
- **Parallax wallpaper** — multi-layer background images (PNG/JPEG/WebP) that scroll as you move across desktops. `pan` mode turns one large image into a world you walk across.
- **Animated wallpaper** — set a video (`fit = "video"`, any format ffmpeg decodes) as a looping background. Software decode on a dedicated thread; the compositor renders at the clip's fps rather than a fixed 60, and playback pauses whenever a fullscreen window covers it. A per-layer `fps` cap trims CPU further on weak hardware.

### Tiling (Hyprland-style)
- **Per-desktop BSP tiling** — toggle `Super+T`; dwindle-style splits along the longer side.
- **Smooth tile animations** — windows glide into their slots (~250 ms, configurable) instead of teleporting.
- **Configurable gaps** — inner (`gaps_in`) and outer (`gaps_out`).
- **Keyboard control** — directional focus (`Super+Arrows`), move window (`Super+Shift+Arrows`), flip split orientation (`Super+S`).
- **Mouse control** — drag BSP borders with `Super+RightDrag` (instant, no animation lag), swap tiles with `Super+Shift+Drag`. Both are `[mouse]` binds like any other; see below.

### Keyboard and mouse
- **The mouse is a bind table too** — `[mouse]` says what a drag with a modifier does: `move`, `move_nocollide`, `resize`, `swap`, `twist`, or any `[binds]` action fired on the press. The defaults are the combinations that used to be hard-coded, so deleting the section changes nothing.
- **Modes (submaps)** — `[mode.<name>]` is a second keymap you step into with one key, where bare letters mean something (`g` for gravity, `c` for calm). One-shot by default, `sticky = true` to hold it open, Escape always leaves, and the tray shows which mode you are in.

### Touchpad
- **Tap-to-click on by default** — plus the rest of libinput's per-device settings in `[input]`: tap-and-drag, natural scrolling, disable-while-typing, pointer acceleration, scroll and click methods. Applied on hotplug and on `Super+Shift+R`, and silently skipped on a device that cannot do them.

### Touchpad gestures (off until you bind them)
- **Three fingers drag the world** — `pan_desktop`: swipe sideways and the camera goes with your fingers across the desktop strip, live; lift them and it settles on a desktop, or flick and it carries on to the next one.
- **Any action, on any swipe or pinch** — `[gestures]` keys look like `"swipe3+up"` / `"pinch3+in"` and take everything `[binds]` takes, so three fingers up can open the launcher and four sideways can carry the focused window to the next desktop (the only pointer way out of a tiling layout).
- **Nothing is bound by default** — an unclaimed gesture goes to the application under the cursor instead (pointer-gestures-v1), which is where a swipe or a pinch belongs until you say otherwise. The set worth starting from is written out, commented, in `config.toml.example`.
- **Not every touchpad can** — libinput only reports gestures on pads that track each finger separately; look for `gesture` in the `Capabilities` line of `libinput list-devices`. Older semi-MT pads (many `SynPS/2`) show only `pointer` and will never send one. Two-finger scrolling is unaffected.

### Tab-stacks
- **Hyprland-style groups** — stack windows into one slot with a chevron tab bar: `Super+W` toggles a stack, `Super+Shift+W` joins the window below, `Super+Tab` / `Super+Shift+Tab` cycle tabs.

### Built-in overlays
- **App launcher** (`Super+Space`) — fuzzy search over desktop entries with icons, no external `rofi` needed. Launched windows drop into the world with physics.
- **Wallpaper picker** (`Super+Shift+P`) — browse a folder and apply an image instantly; the choice is remembered without ever rewriting your config.
- **Keybind cheat-sheet** (`Super+Shift+/`) — generated from your actual binds and gestures, not a static list.
- **Config never costs you the session** — a broken file falls back to built-in binds and reports the problem in a tray pill; fix it and press `Super+Shift+R` to reload live.
- **Window rules** — `[[rule]]` matches `app_id` / `title` with regexes and decides where a window opens and whether physics touches it.
- **A crash no longer costs your layout** — fwm records which applications are running and on which desktop, and the `fwm-session` wrapper brings them back after an unclean exit. They are relaunched, not resumed.

### Visuals
- **Focus borders** — accent color on the focused window, muted on the rest; colors and width in the config.
- **Window fade-in** — new windows ease in over ~260 ms (configurable, 0 disables).
- **Impact effects** — windows squash and stretch where they hit; optional camera shake on hard landings.
- **Wobbly windows** — a dragged window goes soft and bends, the way KDE's do. See below.
- **Rotated and bent windows** — a spinning window is drawn at any angle, not in quarter turns, and a dragged one is drawn through a deforming mesh. wlroots' scene graph is axis-aligned to its bones, so these two draw their own geometry on the renderer's GL context (`src/rotate.c`); everything else stays on the public API.
- **Wallpaper-derived palette** — optionally tint the whole UI toward the wallpaper's dominant hue (`color_source = "wallpaper"`).
- **Minimal tray** — flat chevron-ended islands: focused window + physics readout, desktop indicators, a modes pill, clock. No titlebars anywhere (server-side decorations).
- **Modes pill** — four icons between the desktop indicators and the clock (tiling, floating, gravity, cava), lit when the mode is on. Click it for a menu of switches; cava's is a three-position control (off / visual / physical) rather than a switch, because it is not an on-off thing. Fixed width, and dropped rather than squeezed on a screen too narrow to hold it — the clock grows with the locale's date and the desktop island is centred, so something has to give, and losing a pill that is also a keybind beats overlapping the clock.
- **Transparency** — client alpha (e.g. kitty `background_opacity`) is rendered as-is.
- **Fake fullscreen** (`Super+D`) keeps the tray visible; **real fullscreen** (`Super+F`) hides it and covers the whole output.

### Audio visualiser that windows can stand on

A row of spectrum bars along the bottom of the screen, fed by loopback capture
of whatever is playing. No external `cava` process and nothing to launch — fwm
opens the capture itself and does its own FFT.

The point is the second half. The bars are not a picture: with
`mode = "physical"` or `"both"` each band is a solid, moving body along the
floor, so a window sitting at the bottom of the screen stands **on** the
spectrum, gets thrown by the bass, and lands back down between beats. Windows
dance.

```toml
[cava]
mode = "both"   # off | visual | physical | both
bars = 48
push = 1.0      # how hard it throws things; 0 draws the bars without pushing
```

The two halves are genuinely independent: `"visual"` is a decoration that never
touches a window, and `"physical"` gives you invisible bars — windows jumping to
music with nothing on screen to explain it. Pushing only works on a desktop in
physics mode, since a tiled or floating desktop owns its own geometry, and the
row stands on the floor of the desktop you are looking at.

Every knob is live: `fwmctl set cava.push 2` while the music plays.

It captures through PipeWire or PulseAudio, whichever is actually running — it
probes for the socket and never starts a sound server of its own. That last part
is deliberate: libpulse will happily autospawn a daemon for any client that asks,
and a second sound server fighting the first one for the ALSA device costs you
your audio, not just your bars.

### Wobbly windows

Pick a window up and it goes soft. The window is a lattice of springs rather
than a rectangle: the part you took hold of keeps up with the cursor exactly,
the rest of the sheet arrives late and bends on the way, and shaking it builds a
wobble that keeps going for a beat after your hand stops. Nothing in it reads
how fast you are dragging — the lag is what the springs do on their own, which
is why it swings instead of following.

Grab a window by a corner and the far corner trails by about a tenth of its
width at a brisk drag; grab it dead centre and all four corners trail evenly.
Let go and the wobble rings out in about half a second, after which the window
is rigid again and lands with an ordinary impact dent.

`effects.jelly` scales how far it bends, not how fast — the timing is the same
at every setting, and 0 turns it off. The window stays live as it bends, the
same way a spinning one does. Its focus border is hidden for the duration,
since a rectangle cannot bend along with it. The effect needs the GLES2
renderer.

### Desktop integration
Runs the software you already use: **XWayland** (X11 apps as ordinary physics windows), **layer-shell** (waybar, mako, rofi, swaybg), **ext-session-lock** (hyprlock, swaylock), **idle protocols** (swayidle, no blanking during video), **xdg-activation**, **screencopy** (screenshots, screen share), **gamma-control** (wlsunset), **pointer constraints** (games and mouse-look), **pointer-gestures** (touchpad swipes and pinches reach the app when fwm has no bind for them), **foreign-toplevel** (taskbars), plus drag-and-drop and primary selection.

Known gaps: no HiDPI / fractional output scale, no multimonitor, no IME (xkb layouts do work).

---

## Requirements

| Library | Arch package |
|---|---|
| wlroots 0.20 | `wlroots0.20` |
| wayland-server | `wayland` |
| xkbcommon | `libxkbcommon` |
| cairo + pango | `cairo`, `pango` |
| gdk-pixbuf | `gdk-pixbuf2` |
| Box2D 3.x | `box2d` |
| ffmpeg (libav*) — video wallpaper | `ffmpeg` |
| PipeWire and/or PulseAudio — *optional*, `[cava]` visualiser | `libpipewire`, `libpulse` |
| CMake + pkg-config | `cmake`, `pkgconf` |
| Xwayland (runtime) | `xorg-xwayland` |

The sound libraries are the only optional ones: build without them and
everything else works, minus the audio visualiser. Build against **both** if you
can — which server a machine actually runs is not a build-time fact, and the
visualiser probes for a socket at startup and uses whichever it finds. CMake
prints the backends it compiled in.

wlroots must be built with Xwayland support — CMake refuses to configure otherwise. Xwayland itself starts lazily, only when the first X11 client appears.

**Which terminal `super+Return` opens.** The `terminal` action runs `$TERMINAL` if you have set one — arguments included, since the value goes through a shell — and otherwise the first emulator it finds installed, cheapest to start first. It deliberately ignores `$TERM`: that is the terminfo entry name, so honouring it would try to run `xterm-256color` from inside kitty. If nothing is installed, the tray says so once rather than leaving a key that quietly does nothing.

Why "cheapest first": `kitty` draws through OpenGL 3.3 and pays for that at startup. On old integrated graphics this is slow in a way that looks like the compositor hanging — on a 2012 laptop kitty takes ~2s to appear where [foot](https://codeberg.org/dnkl/foot) takes ~400ms in the same session. Nothing in fwm is involved; both terminals travel the identical map path. (`kitty`'s `single_instance yes` is the other way out: you pay the startup once per session instead of per window.)

To pin a specific one, bind it directly — `spawn:` takes anything a shell does:

```toml
"super+Return" = "spawn:foot"
```

---

## Install

One command — installs dependencies (pacman / apt / dnf / xbps), builds Box2D v3 from source if your distro doesn't ship it, builds fwm and `fwmctl`, and registers a `fwm` session for your display manager. The session entry launches `fwm-session`, the supervisor that restarts the compositor after a crash (see [Session restore](#session-restore)):

```sh
git clone https://github.com/iluaii/fwm.git
cd fwm
./install.sh
```

Updating later:

```sh
./install.sh update      # git pull + rebuild + reinstall
./install.sh uninstall   # removes binary + session file, keeps your config
```

## Build (manual)

```sh
cmake -B build
cmake --build build
```

Run from a TTY:

```sh
./build/fwm
```

Or nested inside another compositor / X session for testing:

```sh
WLR_BACKENDS=x11 ./build/fwm      # nested X11 window
WLR_BACKENDS=wayland ./build/fwm  # nested Wayland window
```

---

## Development

A compositor cannot hot-restart the way an X11 window manager can: it *is* the
display server, so when the process exits every client's connection dies with
it. Nothing brings those *connections* back — preserving the listening socket
across `exec` does not help, because the new process cannot adopt connections
whose protocol objects lived in the old one's heap. ([Session restore](#session-restore)
relaunches the applications, which is a different and lesser thing.) Nesting is
the working substitute for development — run fwm inside your current session and
restart it as often as you like while the session you actually work in never
notices.

```sh
./dev.sh                      # rebuild and run nested
./dev.sh -n 2 -g 1            # ... with two terminals, gravity on
./dev.sh -a toggle_tiling_all # ... firing one action after startup
./dev.sh -s shot.png          # screenshot after a few seconds, then quit
./dev.sh -h                   # all options
```

Because keys cannot be injected into a nested compositor, a few env hooks stand
in for them: `FWM_TEST_ACTION`, `FWM_TEST_GRAVITY` (fwm boots in zero-g),
`FWM_TEST_CAMERA`, `FWM_OPEN_PICKER`, `FWM_SHOW_HINTS`, `FWM_DEBUG`. `dev.sh`
wraps each in a flag. `FWM_TEST_CAVA=1` with `FWM_TEST_CAVA_MODE=<mode>`
(`dev.sh -V both`) drives the visualiser from a synthetic wave instead of the
sound card, because a nested run has nothing playing to capture — it is the only
way to watch windows dance without music. For anything you can express as an action, `fwmctl`
(below) reaches a *running* nested instance and needs no restart at all.

`FWM_DEBUG_EFFECTS=1` is for one specific question: whether a spin or a wobble
is actually reaching the screen evenly. It logs, once a second while an effect
runs, how many frames were presented, the shortest and longest gap between
them, and how many composited re-photographs were taken and what each cost —
which is how the 5ms-per-snapshot stall behind a juddering slow spin was found.
It also says, once per effect, whether the window went down the live path or
the composited one and why. Off, and free, unless the variable is set.

## Scripting — `fwmctl`

fwm listens on a control socket, so you do not have to be a C programmer to
change how it behaves. Anything a keybind can do, a script can do:

```sh
fwmctl state                    # compositor state as JSON
fwmctl windows                  # open windows as JSON
fwmctl dispatch view:3          # run any action from config.toml
fwmctl reload                   # reload the config
fwmctl config                   # every settable option, with values and ranges
fwmctl get physics.gravity      # read one option
fwmctl set physics.gravity 200  # change it, live
fwmctl subscribe                # stream events as they happen
```

Every numeric and colour option in the config is addressable by name, so you can
tune the feel while watching it rather than editing, reloading and guessing:

```sh
fwmctl set physics.gravity 200        # watch a window fall in slow motion
fwmctl set effects.squash 0           # turn impact deformation off
fwmctl set decor.col_active "#ff0000"
```

`set` is **runtime-only** — `config.toml` stays the source of truth and is never
rewritten, so `fwmctl reload` (or `Super+Shift+R`) puts everything back. Values
outside an option's range are refused rather than clamped, because over a socket
a silent clamp is indistinguishable from the value having been accepted.

Replies are JSON, so `jq` does the rest:

```sh
# how many windows are open?
fwmctl windows | jq '.windows | length'

# jump to whichever desktop Firefox is on
fwmctl dispatch view:$(fwmctl windows | jq -r '
    .windows[] | select(.app_id=="firefox") | .desktop' | head -1)

# turn gravity on from a script, a panel button, a hotkey daemon…
fwmctl dispatch cycle_gravity
```

`dispatch` takes exactly the action names the `[keys]` section uses, so
anything you can bind you can also script. Commands that change state are
refused while the session is locked.

The socket is `$XDG_RUNTIME_DIR/fwm-$WAYLAND_DISPLAY.sock` and is also exported
as `$FWM_SOCKET`, which children inherit — a program spawned from a keybind can
talk back to the compositor that started it without being told where it is.
Naming the socket after the Wayland display means a nested dev instance and
your real session never collide.

### Reacting to things — `fwmctl subscribe`

Polling tells you what is true now; `subscribe` tells you when it changes. It
keeps the connection open and streams one JSON object per line until you kill
it:

```sh
fwmctl subscribe                      # everything
fwmctl subscribe window_open,desktop  # just these
```

```json
{"event":"window_open","id":7,"title":"nvim","app_id":"foot","desktop":2}
{"event":"desktop","desktop":2,"mode":"tiling"}
{"event":"gravity","gravity":1.000}
```

| event | fires when |
|---|---|
| `window_open` / `window_close` | a window maps / unmaps |
| `window_focus` | focus moves (`"id":null` when it lands nowhere) |
| `window_title` | a mapped window renames itself |
| `desktop` | the camera settles on a different desktop |
| `mode` | a desktop switches physics / tiling / floating |
| `gravity` | the gravity mode is cycled |
| `config_reload` | the config was re-read, so anything cached is stale |

An unknown event name is refused outright rather than ignored, since a typo
that silently subscribed you to nothing looks exactly like an event that never
fires.

**This is the extension mechanism.** A "plugin" for fwm is any process that
subscribes at one end and calls `dispatch` at the other — in any language,
with no ABI to track and no shared address space, so a script that crashes
takes nothing down with it:

```sh
# float every mpv window the moment it opens
fwmctl subscribe window_open | while read -r ev; do
    echo "$ev" | jq -e 'select(.app_id=="mpv")' >/dev/null &&
        fwmctl dispatch toggle_float
done
```

Panels and widgets need nothing new either: fwm implements `wlr-layer-shell`,
so waybar, eww and ags already work, and `subscribe` is how a module reads the
things only fwm has — gravity mode, per-desktop mode, the window strip.

fwm deliberately has no in-process plugin API. wlroots has no stable ABI, so
loadable modules would break on every wlroots release and every crash in one
would be a lost session; the socket costs a few microseconds per event and
neither is true of it.

Subscribers are never allowed to slow the compositor down: writes are queued
and flushed asynchronously, and a subscriber that stops reading is disconnected
once its backlog passes 256 KiB rather than being waited on.

## Session restore

Because a compositor crash takes every client with it, fwm keeps a note of what
is running — the command line of each application and the desktop it is on — in
`~/.local/state/fwm/session`, refreshed every few seconds. `install.sh`
registers `fwm-session` as your display-manager entry: it starts the compositor,
and if it dies unexpectedly, starts it again and lets the note put your
applications back.

Applications are **relaunched, not resumed**. Anything unsaved inside them is
still gone; what you get back is the layout, not the state.

By default this only happens after an unclean exit. fwm deletes the note on a
normal shutdown, so a file that survived is itself the evidence that the last
run died — a deliberate logout gives you an empty desktop, as it should.

```toml
[session]
restore = "crash"    # default: only after an unclean exit
# restore = "always" # every start, including a normal login
# restore = "never"  # nothing recorded, nothing relaunched
```

Three failures within a minute and `fwm-session` gives up rather than spinning
in front of a user with no way to intervene; the reason lands in
`~/.local/state/fwm/crash.log`.

Two known limits: an application whose window belongs to a different process
than the one launched (some browsers, Electron apps) will come back on the
current desktop rather than its old one, and at most 64 applications are
recorded.

---

## Configuration

Everything lives in `~/.config/fwm/config.toml`. All sections are optional — missing values fall back to sane defaults.

```toml
[physics]
friction              = 0.985   # zero-g glide brake (per-frame factor)
restitution           = 0.3     # bounciness: 0 = dead, 1 = superball
gravity               = 981.0   # px/s^2; 981 = Earth at 100 px/m
mass_density          = 0.0005  # window mass = area * density
throw_speed_multiplier = 0.65
max_throw_speed       = 1800.0
stop_speed_threshold  = 1.0     # below this a window is considered at rest
tick_rate             = 60.0    # physics steps per second

[input]
# Several layouts + a grp:* option gives you layout switching for free.
# Keep a Latin layout FIRST: binds fall back to it, so ctrl+c and super+q
# keep working while you are typing in another script.
kbd_layout   = "us,ru"
kbd_options  = "grp:alt_shift_toggle"
kbd_variant  = ""
repeat_rate  = 25
repeat_delay = 600
# Touchpad, via libinput. Everything except `tap` keeps libinput's own
# per-model default unless you set it; fwm turns tap-to-click ON because
# libinput ships it off and a laptop with no mouse then cannot click at all.
# A pad that cannot do one of these ignores it — `libinput list-devices` shows
# which: "disabled" means it can, "n/a" means it cannot.
tap = true
#tap_drag         = true            # tap, then slide = drag
#drag_lock        = false
#natural_scroll   = false           # content follows the fingers
#dwt              = true            # ignore the pad while typing
#middle_emulation = false
#left_handed      = false
#accel_speed      = 0.0             # -1 (slow) .. 1 (fast)
#accel_profile    = "adaptive"      # or "flat"
#scroll_method    = "two_finger"    # or "edge", "button", "none"
#click_method     = "button_areas"  # or "clickfinger", "none"

# Touchpad gestures: "swipe<2..5>+left|right|up|down", "pinch<2..5>+in|out",
# and any action [binds] accepts. Nothing is bound unless you bind it — without
# this section fwm claims no gesture and every one reaches the application under
# the cursor. This is the set worth starting from.
[gestures]
sensitivity    = 1.0   # camera px per finger px
natural        = true  # the desktop strip follows your fingers, as on a phone
"swipe3+left"  = "pan_desktop"      # live pan across the strip; bind BOTH
"swipe3+right" = "pan_desktop"      # directions — it is one gesture
"swipe3+up"    = "launcher"
"swipe3+down"  = "toggle_tray"
"swipe4+left"  = "move_to_view:prev"  # take the focused window one desktop over
"swipe4+right" = "move_to_view:next"
#"pinch3+in"   = "calm_all"          # leave two-finger pinch to the apps

[tiling]
gaps_in    = 6       # px between tiles
gaps_out   = 14      # px between tiles and screen edges
anim_speed = 12.0    # tile glide speed (1/s); 0 = instant

[camera]
anim_ms    = 350.0   # desktop-switch slide (ease-in-out); 0 = instant snap
free_speed = 14.0    # how tightly the camera follows a held move_camera: bind
wrap       = false   # make the desktops a ring: past the last one is the first

[effects]
camera_shake = 0.0   # jolt the view on hard impacts; off by default, 1.0 to enable
squash       = 1.0   # windows deform on impact, scaled by speed; 0 disables
jelly        = 1.0   # how far a dragged window bends (wobbly windows); 0 disables
spin         = 1.0   # strength of the spin_window kick (experimental); 0 disables

[focus]
# When an app asks to be raised (xdg-activation): "never" ignores it,
# "same_desktop" (default) focuses it only if it is already on screen,
# "always" also pans the camera to whichever desktop it lives on.
on_activate = "same_desktop"

[session]
restore = "crash"    # "crash" (default) | "always" | "never" — see above

# Per-window rules, applied once when the window opens. app_id and title are
# POSIX extended regexes; a rule with both needs both to match, and later rules
# override earlier ones field by field.
# There is deliberately no per-window "float": tiling here is a property of the
# DESKTOP, not of the window.
[[rule]]
app_id    = "^mpv$"
nocollide = true     # video should not get shoved around by physics
# pin     = true     # immovable: physics never pushes it
# desktop = 3        # always open on desktop 4

[decor]
border_width = 2
col_active   = "#7aa2f7"   # "#RRGGBB" or "#RRGGBBAA"
col_inactive = "#3b4261"
fade_in_ms   = 260.0       # window appear/close animation; 0 disables
wallpaper_fade_ms = 420.0  # cross-fade on runtime wallpaper swap; 0 = cut
tray_opacity     = 0.92    # island fill opacity (0..1)
launcher_opacity = 0.92
icon_theme   = ""          # launcher icons; "" = auto (gtk settings, hicolor)
# Where the UI palette comes from:
#   "config"    — the colours above plus the built-in dark scheme (default)
#   "wallpaper" — tint the tray/panels toward the wallpaper's dominant hue and
#                 take the accent (focus border, desktop marker, tab underline)
#                 from its most vivid colour. Islands stay dark, so text keeps
#                 its contrast whatever the image looks like.
color_source  = "config"
tint_strength = 0.4        # 0..1: how far the islands move toward that hue

# Where the built-in wallpaper picker (super+shift+p) looks for images and
# videos (a video's icon is a random frame). The choice is remembered in
# ~/.local/state/fwm/wallpaper and overrides the first [[wallpaper]] layer below
# on the next start — your config file is never rewritten.
# fps: base cap applied to any video picked here (0/unset = the clip's own
# rate). One knob for everything you pick; lower it to cut CPU on weak hardware.
[wallpaper_picker]
dir = "~/Pictures"
#fps = 20

# Wallpaper layers, drawn back-to-front.
# fit = "cover" (fill+crop) | "contain" (letterboxed) | "pan" (walk across)
#     | "video" (looping video, scaled to cover)
[[wallpaper]]
path = "/path/to/image.png"
fit  = "pan"
# "pan" moves only images wide enough to stick out past the screen at native
# scale; those pan with nothing cropped. A narrower one is shown still, since
# travel would have to be bought by cropping its height. Opt in per layer:
# pan_crop = 0.25   # trade 25% of the height for some travel
# zoom     = 1.6    # or set the render width directly (screen_w * zoom)

# Animated wallpaper: any video ffmpeg can decode (mp4/mkv/webm…) loops
# forever. Software decode on its own thread; the compositor renders at the
# clip's fps (not 60), skips the H.264 loop filter, and pauses while a
# real-fullscreen window covers it. fit = "video" is optional: a path with a
# video extension is detected on its own. It pauses whenever any fullscreen
# window (real or fake) covers it. fps caps the rate; set it to ~half the clip's
# rate (e.g. 15 for a 30fps clip) to also drop B-frames and cut the decode
# itself (~30% on 1080p, ~35% on 4K), at the cost of choppier motion. Prefer a
# 1080p clip over 4K on a 1080p screen (4K costs ~2x for nothing).
#[[wallpaper]]
#path = "/path/to/wallpaper.mp4"
#fit  = "video"
#fps  = 15

[binds]
"super+Return"       = "terminal"
"super+space"        = "launcher"
"super+q"            = "killclient"
"super+t"            = "toggle_tiling"
"super+alt+space"    = "toggle_floating"
"super+d"            = "fake_fullscreen"
"super+f"            = "real_fullscreen"
"super+h"            = "move_camera:-50"
"super+l"            = "move_camera:50"
"super+p"            = "pin_window"
"super+n"            = "toggle_nocollide"
"super+g"            = "cycle_gravity"
"super+j"            = "toggle_tray"
"super+r"            = "spin_window"
"super+s"            = "toggle_split"
"super+shift+c"      = "calm_all"
"super+shift+n"      = "toggle_nocollide_all"
"super+shift+t"      = "toggle_tiling_all"
"super+a"            = "expo"
"super+shift+slash"  = "show_hints"
"super+shift+r"      = "reload_config"
"super+shift+p"      = "wallpaper_picker"
"super+shift+l"      = "spawn:hyprlock"
"super+shift+Escape" = "EXIT"
"super+Left"         = "tile_focus:l"    # Right/Up/Down likewise
"super+shift+Left"   = "tile_move:l"
"super+w"            = "group_toggle"    # tab-stacks
"super+Tab"          = "group_next"
"super+shift+w"      = "group_add"
"super+1"            = "view:0"          # ... "super+0" = "view:9"
"super+shift+1"      = "move_to:0"       # ... "super+shift+0" = "move_to:9"
```

### Actions

| Action | Meaning |
|---|---|
| `spawn:<cmd>` | run a command |
| `killclient` | close focused window |
| `toggle_tiling` | physics ⇄ BSP tiling for the current desktop |
| `toggle_floating` | physics ⇄ floating (windows stay put and overlap) |
| `fake_fullscreen` / `real_fullscreen` | fullscreen below the tray / whole output |
| `move_camera:<px>` | scroll the camera (repeats while held) |
| `view:<0-9>` | jump to desktop; `view:next` / `view:prev` for the one beside it |
| `move_to:<0-9>` | send the focused window to a desktop, camera stays (`next`/`prev` too) |
| `move_to_view:<0-9>` | send it there and follow, keeping it focused (`next`/`prev` too) |
| `pan_desktop` | **gestures only:** pan across the strip with your fingers, settling on a desktop when they lift |
| `tile_focus:l\|r\|u\|d` | focus tile in direction |
| `tile_move:l\|r\|u\|d` | swap tile in direction |
| `toggle_split` | flip split orientation of the focused tile |
| `toggle_tray` | hide/show the tray — it stops reserving its strip, so windows fill the top |
| `pin_window`, `toggle_nocollide`, `calm_all`, `cycle_gravity` | physics toggles |
| `spin_window` / `spin_all` | **experimental:** set the focused window (or every window) spinning, picture and collision box alike; press again to settle |
| `toggle_nocollide_all` / `toggle_tiling_all` / `toggle_floating_all` | same, but every window / every desktop at once |
| `group_toggle`, `group_add`, `group_next`, `group_prev` | tab-stacks: make a stack, join it, cycle tabs |
| `launcher` | built-in app launcher |
| `toggle_wrap` | close the desktop strip into a ring: stepping past the last desktop arrives on the first (`x` inside expo) |
| `expo` | the desktop strip: the camera pulls back over the neighbouring desktops — click a window to go to it, super+drag to move it between desktops, right-click for its menu, `z` for a wider view and, from there, middle-drag (or alt+drag) to fly around the ring |
| `show_hints` | keybind cheat-sheet overlay |
| `reload_config` | re-read the config file and apply it without restarting |
| `wallpaper_picker` | built-in wallpaper browser; Enter applies the image at once |
| `show_errors` | open the config-problem panel (same as clicking the tray's ⚠ pill) |
| `modes_menu` | open the modes menu (same as clicking the tray's modes pill) |
| `EXIT` | quit the compositor |

### Mouse

| Gesture | Effect |
|---|---|
| `Super+LeftDrag` | move / throw a window (floating) |
| `Super+Shift+LeftDrag` | move through windows (no collision) — or swap tiles when tiling |
| `Super+RightDrag` | resize (floating) / drag BSP border (tiling) |

---

## License

GPLv2 — see [LICENSE](LICENSE).
