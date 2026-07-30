# Troubleshooting

Contents: [it will not start](#it-will-not-start) ·
[the config did nothing](#the-config-did-nothing) · [it died](#it-died) ·
[no sound](#no-sound) · [windows behave oddly](#windows-behave-oddly) ·
[monitors](#monitors) · [nested runs](#nested-runs) ·
[debug switches](#debug-switches) · [tests](#tests)

## It will not start

**"wlroots-0.20 was built without Xwayland support"** — a configure-time failure.
fwm needs Xwayland at runtime for X11 clients; install `xorg-xwayland` and a
wlroots built with it.

**wlroots version errors** — fwm pins wlroots 0.20 exactly. The API breaks between
minor versions, so the build refuses 0.21 rather than failing halfway with
confusing errors.

**"XDG_RUNTIME_DIR is unset"** — wlroots needs it for the socket. Your login setup
should provide it; from a bare TTY, a session manager or `elogind`/`seatd` usually
does.

**Nothing on screen, no error** — run with `FWM_DEBUG=1` and read the log. On a
TTY, redirect it: `FWM_DEBUG=1 fwm > /tmp/fwm.log 2>&1`.

## The config did nothing

Look at the tray: a ⚠ pill means fwm read your file and did not like parts of it.
Click it (or bind `show_errors`) for the list — it names the section and the key.

Things that are easy to get wrong:

- **A missing quote.** TOML needs `"super+q" = "killclient"`, quotes on both
  sides. A `[binds]` section where every line is broken falls back to the built-in
  binds, which is why your custom keys may all be gone at once.
- **An unknown action.** Reported at load time. The complete list is in
  [Keybindings](keybindings.md#actions).
- **A `[[rule]]` with no matcher**, or a `[physics.<name>]` profile that names no
  desktop: both are reported as useless rather than silently ignored.
- **`tick_rate`** is read once at startup — a reload will not change it.
- **You are looking at a `fwmctl set` override.** `Super+Shift+R` (or
  `fwmctl reload`) discards every override and goes back to the file.
- **The UI remembered something.** The mass mode and the collision sound come from
  `~/.local/state/fwm/modes`, which wins over the config because it was written by
  a click. Delete the file to go back.
- **The wallpaper is not the one in the config.** Same thing:
  `~/.local/state/fwm/wallpaper` holds the picker's choice.

## It died

`fwm-session` writes `~/.local/state/fwm/crash.log` and brings fwm back, with the
applications from `~/.local/state/fwm/session` relaunched on the desktops they
were on. Three failures inside a minute stop the loop rather than flapping in
front of you.

Windows that came back on the wrong desktop, or did not come back at all: an
application whose window belongs to a different process than the one launched
(some browsers, Electron apps) cannot be matched, and at most 64 are recorded. Set
`[session] restore = "never"` to switch the whole thing off.

## No sound

Neither the visualiser nor the collision knock will ever start a sound server —
libpulse would happily autospawn one, and a second daemon fighting the first for
the card costs you your audio and not just the bars. So:

1. **Is a server running?** `pactl info` or `pw-cli info 0`. If your daemon is
   autospawned by the first client that wants sound, there is none at login: fwm
   keeps looking and the bars appear when something starts playing.
2. **Was fwm built with the libraries?** The log says
   `audio: no sound server running` when there is none, and CMake prints
   `[cava] capture backends: …` at configure time. No line at all means it was
   built without both.
3. **Collision sound specifically needs libpulse-simple.** PipeWire boxes serve
   that through pipewire-pulse. Without it the log says
   `sound: built without libpulse-simple`.
4. **`[sound] path`** — if the WAV cannot be read, the log says why
   (`not 16-bit PCM or 32-bit float`, `no data chunk`, …) and the built-in click
   plays instead, which sounds exactly like a working feature.

## Windows behave oddly

**A window will not move.** It is pinned (`Super+P`), fullscreen, or the desktop
is in tiling or floating mode — in all of those, physics is not allowed to move
it. A `[[rule]]` may have pinned it; `fwmctl windows` shows `pinned` and
`nocollide` per window, which is the only way to see whether a rule took hold.

**Windows pass through each other.** Either `nocollide` is set on one of them, or
you are dragging the mouse faster than about three times `max_throw_speed` — see
[Physics](physics.md#speed) for exactly what that limit is and why it exists.

**Windows will not fall.** fwm starts in zero-g whatever `[physics] gravity` says;
`Super+G` cycles it on. The tray's gravity icon is lit when it is.

**Windows drift forever.** That is zero-g with `friction` close to 1. Lower
`[physics] friction`, or turn gravity on.

**A window sits half off the screen.** A window larger than the play area cannot
fit in it; the walls hold what they can. Resize or fullscreen it.

**Everything is jittery under a spin or a drag.** `[effects] live = 0` puts
windows under an effect back on a periodic still frame, which is the better trade
on slower hardware. `FWM_DEBUG_EFFECTS=1` prints frame timings.

## Monitors

`fwmctl outputs` lists what fwm sees, including every mode each screen offers, and
`fwmctl output <name> …` changes one live — that is the whole of display
configuration, and it applies through the same code `[[output]]` uses.

**A screen went dark and the pointer is on it.** `outputs_on` turns everything
back on and needs no pointer and no visible monitor. `output_off` refuses to turn
off the last lit screen.

**Names** are what fwm logs at startup (`output eDP-1: 1920x1080, scale 1.00`) and
what `[[output]] name` expects.

## Nested runs

fwm runs inside your session, which is how it is developed:

```sh
./dev.sh -h        # what it can do
./dev.sh -n 2 -g 1 # two terminals, gravity on
```

Three traps, all learned the hard way:

- **`FWM_SOCKET` targets whatever it points at.** A `fwmctl` call from your shell
  goes to the *outer* session unless you set it:
  `FWM_SOCKET=/run/user/1000/fwm-wayland-1.sock fwmctl state`. The socket name is
  in the inner instance's log (`Wayland socket: wayland-1`).
- **Session restore can fork fwm inside fwm.** A nested run reads the same state
  file as your real session, so it may relaunch what your session recorded. Point
  `XDG_STATE_HOME` somewhere temporary for anything experimental.
- **Keybinds cannot be tested nested.** The outer compositor claims the `Super`
  combinations first. Run from a bare TTY for those, or use
  `FWM_TEST_ACTION=<action>` to fire one action a second after startup.

## Debug switches

Environment variables, all read at startup:

| Variable | Does |
|---|---|
| `FWM_DEBUG=1` | wlroots debug logging |
| `FWM_DEBUG_EFFECTS=1` | frame timings for spin/wobble, once a second while one is running |
| `FWM_THEME_DEBUG=1` | what the palette was derived from |
| `FWM_TEST_ACTION=<action>` | dispatch one action ~1.5s after startup |
| `FWM_TEST_GRAVITY=<scale>` | start with gravity on |
| `FWM_TEST_CAMERA=<n>` | start parked on desktop *n* |
| `FWM_TEST_CAVA=1`, `FWM_TEST_CAVA_MODE=<mode>` | the visualiser on synthetic audio, for a nested run with nothing playing |
| `FWM_TEST_ORBIT`, `FWM_TEST_ORBIT_DIST` | open the desktop strip in the wider view |
| `FWM_OPEN_PICKER=1`, `FWM_SHOW_HINTS=1` | open the picker / hints at startup |

They exist because a compositor cannot be driven by a test harness: there is no
way to inject a keypress or move the pointer, so the code paths a bind reaches
have to be reachable some other way. `dev.sh` wraps most of them in flags.

## Tests

```sh
cmake -S . -B build-test -DFWM_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Ten suites, none of which need a compositor, a display or a sound card. What each
covers is in [Architecture](architecture.md#tests).
