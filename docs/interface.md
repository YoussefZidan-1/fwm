# The interface

fwm draws its own status strip, launcher, desktop overview and menus. There are no
titlebars anywhere (windows are server-side decorated with a focus border and
nothing else), and there is no panel process to start — all of it is the
compositor.

Contents: [the tray](#the-tray) · [the modes menu](#the-modes-menu) ·
[the launcher](#the-launcher) · [the desktop strip](#the-desktop-strip) ·
[the wallpaper picker](#the-wallpaper-picker) · [key hints](#key-hints) ·
[config problems](#config-problems) · [the visualiser](#the-audio-visualiser) ·
[collision sound](#collision-sound)

## The tray

One strip along the top of **each** monitor, built from flat chevron-ended
islands. `Super+J` hides it; a real-fullscreen window hides it automatically
(fake fullscreen deliberately keeps it).

Left to right:

- **Focused window** — its title, plus a live physics readout: speed, direction
  and mass. Each monitor reports the window on *its own* desktop.
- **Desktop indicators** — ten dots, the current one marked. The marker slides as
  the camera pans, so it tracks a free pan rather than jumping between desktops.
- **The modes pill** — four icons (tiling, floating, gravity, visualiser), lit
  when that mode is on. Click for the menu below. Fixed width, and dropped rather
  than squeezed on a screen too narrow for it: the clock grows with the locale's
  date and the desktop island is centred, so something has to give, and losing a
  pill that is also a keybind beats overlapping the clock.
- **A ⚠ pill** when the config has problems — click to read them.
- **A ⌨ pill** in the accent colour while a `[mode.*]` submap is open, naming it —
  a mode is a state the keyboard is *in*, and nothing else says so.
- **The clock** and date, with the keyboard layout in front of it when you have
  more than one configured (`RU • 11:02 • Thu, 30/07`).

Opacity is `[decor] tray_opacity`; the whole palette can be derived from the
wallpaper with `[decor] color_source = "wallpaper"`.

## The modes menu

Click the modes pill (or bind `modes_menu`). Eight rows:

| Row | Control | What it is |
|---|---|---|
| Tiling | switch | BSP tiling on the desktop you are looking at |
| Floating | switch | floating mode on that desktop |
| Gravity | switch | on = the heaviest step in `gravity_steps`, off = zero-g |
| Mass | `size` / `ram` | what decides how heavy a window is — see [Physics](physics.md#mass) |
| Sound | switch | the collision knock |
| Cava | `off` / `visual` / `physical` | the audio visualiser |
| Ring | switch | close the desktop strip into a ring (`[camera] wrap`) |
| Breakable | switch | a hard enough collision destroys a window ([`[physics] hp`](configuration.md#breakable-windows)) |

Two rows are segmented controls rather than switches because they are not on-off
things. The config's fourth cava mode (`both`, bars that are drawn *and* push) is
what the `physical` segment actually selects; a mode you can only discover in the
file is better than one you can land on by clicking past it.

Breakable sits last on purpose: it is the only row that can destroy someone's
unsaved work, so it is not the one the hand lands on by accident. For the same
reason it is the one row that is **never remembered** — every session starts with
windows unbreakable, and no config key can start one otherwise.

**The Mass and Sound choices are remembered** across restarts, in
`~/.local/state/fwm/modes`. They are written the moment you click and applied over
the config on every load, so your `config.toml` is never rewritten to record a
click. Delete that file to go back to whatever the config says.

Layout, gravity and the ring are *not* remembered either: they are per-session
state that a keybind changes just as often as the menu does. Breakable is not
remembered for a different reason — see above.

## The launcher

`Super+Space`. Type to filter, `↑`/`↓` (or `Tab`) to move, `Return` to run,
`Escape` to close. It reads the same `.desktop` files everything else does and
draws real icons from your icon theme (`[decor] icon_theme`, or auto-detected from
the gtk3 setting and then hicolor).

The tiles have physics of their own — they settle as the list filters — and the
launcher owns the keyboard while it is open, so no keystroke reaches a client you
cannot see.

## The desktop strip

`Super+A` (`expo`). Every desktop becomes a live-looking card in a row you can
pan, click into, and drag windows between. `z` steps out to the wider view, where
the cards become a ring in 3D you can orbit; `x` closes the strip into a ring
(the same `toggle_wrap` the config's `[camera] wrap` sets).

Keys are listed in [Keybindings](keybindings.md#keys-inside-the-desktop-strip).
While the strip is up the **simulation is frozen** — otherwise the windows in
those pictures would quietly have moved by the time you dropped one.

## The wallpaper picker

`Super+Shift+P`. Browses `[wallpaper_picker] dir` (default `~/Pictures`), stills
and videos alike, and applies the choice with a cross-fade. The pick is remembered
in `~/.local/state/fwm/wallpaper` and re-applied after every config load, so a
reload keeps it. `[wallpaper_picker] fps` caps the frame rate of videos chosen
this way.

## Key hints

`Super+Shift+?` draws the live bind list — read from your config, not from a
hard-coded table, so it is accurate for whatever you have bound.

## Config problems

A broken config never costs the session: fwm carries on with defaults and puts a
⚠ pill in the tray with the count. Clicking it (or `show_errors`) opens the panel
listing what went wrong, line by line. Fix the file and press `Super+Shift+R`.

## The audio visualiser

A row of spectrum bars along the bottom of the screen, fed by loopback capture of
whatever is playing — no external `cava` process, and fwm does its own FFT.

The point is the second half: with `mode = "physical"` or `"both"` each band is a
solid moving body along the floor, so a window sitting at the bottom of the screen
stands **on** the spectrum, gets thrown by the bass, and lands back down between
beats.

Configured in [`[cava]`](configuration.md#cava). It captures through PipeWire or
PulseAudio, whichever is actually running, and never starts a sound server of its
own — libpulse would happily autospawn one, and a second daemon fighting the first
for the card costs you your audio, not just your bars. If no server is running
yet, fwm keeps looking: bars appear when something starts playing, however long
after login that is.

## Collision sound

With `[sound] collisions = true`, windows knock when they hit each other, the
floor or the walls. How hard the hit was sets the volume; how heavy the window is
sets the pitch, so with `mass = "ram"` a two-gigabyte browser knocks like a
wardrobe. `path` takes a WAV of your own; without one the click is synthesised — a
short noise burst for the contact and a low decaying sine for the weight behind
it.

Nothing is opened until the first collision and the device is handed back a few
seconds after the last one, so a quiet desktop holds no audio stream. Playback
needs libpulse-simple (pipewire-pulse serves it too).
