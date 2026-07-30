# Physics

Windows in fwm are rigid bodies in a real solver — [Box2D](https://box2d.org/)
v3 — not an animation that looks like one. This page is what that means in
practice: the units, what decides how a window behaves, and where the simulation
is deliberately bounded.

Contents: [the world](#the-world) · [units](#units) · [what a window is made
of](#what-a-window-is-made-of) · [mass](#mass) · [gravity](#gravity) ·
[throwing and dragging](#throwing-and-dragging) · [speed](#speed) ·
[rotation](#rotation) · [what physics does not own](#what-physics-does-not-own)

## The world

One strip, ten screens wide, with walls: a floor, a ceiling, and an end at each
extreme. Desktop *n* is the region from `n × screen_width` to
`(n+1) × screen_width`; a window's desktop is simply where its centre is. That is
why dragging a window past the screen edge moves it to the next desktop and why
throwing one can send it there.

With `[camera] wrap = true` the strip closes into a ring and the two end walls
are removed. A window thrown off one end flies on and arrives at the other with
its velocity intact — only once it is *entirely* past, so it is never visible at
both ends at once.

Windows on a **tiling** or **floating** desktop are not simulated: the layout (or
you) owns their geometry, so physics treats them as immovable anchors. Switch the
desktop back to physics mode and they are objects again.

## Units

| Quantity | Unit | Note |
|---|---|---|
| Length | px | Box2D works in metres internally at **100 px per metre**, so a window is a 1–20 m box — the size range the solver is tuned for. |
| Speed | px/s | 1800 px/s, the default cap, is 18 m/s. |
| Acceleration | px/s² | `gravity = 981` is earth at that scale. |
| Mass | area × `mass_density` | Arbitrary units; only ratios matter. |
| Time | fixed 1/60 s steps | Four solver substeps each. The tick catches up with whole steps when a frame runs late, so the simulation never depends on the frame rate. |

## What a window is made of

Four properties, resolved fresh every step, in this order:

1. **`[physics]`** — the world's values.
2. **`[physics.<name>]`** — the profile assigned to the desktop the window is
   currently on. A window dragged from the moon desktop onto the water desktop
   changes as it crosses the edge, mid-flight.
3. **`[[rule]]`** — what matched this window when it opened. `mass` and `gravity`
   multiply the desktop's; `bounce` and `friction` replace it.
4. **The mass mode** — if `mass = "ram"`, what the application's memory footprint
   says (below).

Resolving per step rather than per window is what lets all four coexist: a heavy
window stays heavy when it is dragged onto the moon, and a config reload changes
what everything is made of without touching a body.

## Mass

`[physics] mass` picks what decides a window's weight.

**`"size"`** (default) — `mass = width × height × mass_density`. A big window is
heavy, a small one skitters.

**`"ram"`** — how much memory the application is using, with size ignored
entirely. The window's own process tree is summed (a browser's tabs count toward
its window), so the browser that has eaten two gigabytes becomes the heaviest
thing on the desktop and shoves everything else aside. `/proc` is walked once
every 1.5 s and never at all while the mode is off.

The arithmetic: a window using `mass_ram_ref` MB weighs what a 1280×720 window
weighed under `"size"`, scaled linearly by its actual footprint and clamped to
`mass_ram_max` times that either way. The clamp is not optional — an uncapped 6GB
browser beside a 20MB terminal is not a heavy window, it is a wall, and Box2D
answers a 300:1 mass ratio by pushing the light body through the floor.

Both modes are switchable live from the modes menu, which remembers the choice.

## Gravity

fwm starts in **zero-g** whatever the config says, and `cycle_gravity`
(`Super+G`) walks `gravity_steps` — by default zero-g, a lick of gravity (0.15),
and earth (1.0). The step is a multiplier on `[physics] gravity`, and 0 anywhere
is 0 everywhere: it is the master switch.

In zero-g a window keeps `friction` of its speed every tick and glides a long
way. Under gravity, damping is nearly switched off and windows are stopped by
contact friction instead — which is what real objects do, and why a window slides
along the floor and grinds to a halt rather than gliding as if on ice.

A `[physics.<name>]` profile can give one desktop its own gravity (the moon), and
a `[[rule]]` can give one window a multiple of it — `gravity = 0` for a window
that hangs in a room where everything falls, `-0.2` for a balloon.

## Throwing and dragging

**Throwing** is not a gesture fwm recognises; it is what happens when you let go
of a moving window. The drag samples the cursor over the last frames, the release
hands that velocity to the body times `throw_speed_multiplier`, and
`max_throw_speed` caps it.

**Dragging** makes the window kinematic — infinitely heavy, its position owned by
the mouse — so it shoves the windows it meets instead of being pushed back by
them. The velocity Box2D is told is derived from how far the cursor actually
moved it since the last step, which matters more than it sounds:

> A contact with a body that has no approach speed produces no hit event. Handing
> Box2D a standing-still velocity while teleporting the transform across the
> screen is why shoving one window into another once produced no squash and no
> sound, while a thrown window bouncing off a wall — dynamic and honestly moving —
> always did.

**Spinning** is `spin_window` (`Super+R`) or the `twist` mouse verb; see
[rotation](#rotation).

## Speed

There is exactly **one speed limit**: `[physics] max_throw_speed`. No dynamic
window ever exceeds it, however it was set moving — thrown, shoved by a drag,
kicked by a visualiser bar, or launched by a stack collapsing on itself. Setting
it to 0 removes the limit.

One limit rather than several is a deliberate change. What it buys:

- A shove can never outrun the hardest deliberate throw, which is the property a
  drag-specific ceiling used to protect by *lying to the solver* about how fast
  the dragged window was moving — and thereby breaking collision outright above
  600 px/s of mouse movement.
- At the 1800 px/s default a window covers 30 px per step, an order of magnitude
  less than the smallest window, so window-through-window tunnelling is not
  reachable.
- Box2D's own hidden limit (400 m/s, i.e. 40 000 px/s) no longer silently clips a
  raised `max_throw_speed`: the engine's ceiling is kept above fwm's.

It is invisible in ordinary use. A window falling the full height of a 1080px
screen under earth gravity arrives at about 1456 px/s, below the default cap, so
gravity, bounces and throws behave exactly as they always did.

### Measured limits

From `tests/test_physics_speed.c` and the diagnostics behind it:

| Situation | Result |
|---|---|
| Throw at 12 000 / 48 000 / 96 000 px/s into a wall, with the ceiling lifted | contained, every time, no NaN, no escape |
| Drag into a window at up to 3000 px/s | clean shove, no visible overlap |
| Drag through a row of three windows at up to 4000 px/s | all three pushed along |
| Drag at 6000 px/s and above | the hand outruns what it is pushing — see below |
| A settled stack hit at the default cap | worst overlap 3 px, nothing leaves the screen |

**The one honest limit.** A dragged window's transform is teleported to wherever
the cursor is, so at 6000 px/s it enters its neighbour by 100 px in a single step.
No solver can undo that within the same step, and the neighbour cannot move away
faster than the hand pushing it: above roughly three times the speed cap, a drag
passes through. Letting go resolves it — the window becomes dynamic, bounded by
the same ceiling, and collides normally. Fixing the residue properly means
sweeping the kinematic body in sub-steps, which is not worth its cost for a case
you can only reach by flinging the mouse across the screen.

## Rotation

Off by default and experimental. `spin_window` hands a window's rotation to the
simulation: the collision box really turns, so it wedges into corners and shoves
its neighbours corner-first — it is not a picture spinning over an upright box.

The press itself is only a nudge (about a quarter turn a second, scaled by
`[effects] spin`). The spinning comes from what happens next: dragged, a spinning
window hangs from the point you took hold of, swings behind your hand, settles
hanging down under gravity, and keeps whatever spin it had when you let go.
Stirring the mouse winds it up. `twist` in `[mouse]` turns one by hand.

A window is only as free to turn as the room around it: one whose diagonal is
taller than the screen wedges against the floor and ceiling instead of coming
round.

## What physics does not own

- **Tiled and floating windows.** Immovable anchors; the layout owns them.
- **Fullscreen windows.** Likewise, while they are fullscreen.
- **Pinned windows** (`pin_window`, or `pin` in a rule).
- **The window being resized**, and one being turned by hand: frozen so the mouse
  owns them completely.
- **The desktop strip** (`expo`). While it is up the simulation is frozen —
  otherwise the windows in those still pictures would quietly have moved by the
  time you dropped one.
- **`no_collide` windows** pass through other windows but never through a wall:
  the play area is not optional.
