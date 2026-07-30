# fwm documentation

The [top-level README](../README.md) is the tour: what fwm is and what it looks
like. These pages are the manual — what every setting does, what every action is
called, and how the pieces work.

| Page | What is in it |
|---|---|
| [Getting started](getting-started.md) | Building, installing, starting a session, the first five minutes |
| [Configuration](configuration.md) | Every section of `config.toml`, key by key |
| [Keybindings and actions](keybindings.md) | The default binds, the full action vocabulary, modes, mouse drags, gestures |
| [Physics](physics.md) | How the simulation works, its units and limits, and what it deliberately does not promise |
| [The interface](interface.md) | Tray, modes menu, launcher, desktop strip, wallpaper picker, visualiser, collision sound |
| [fwmctl and the IPC](fwmctl.md) | Reading state, changing settings live, streaming events, scripting |
| [Troubleshooting](troubleshooting.md) | When something does not work, and how to find out why |
| [Architecture](architecture.md) | For anyone changing the code: the mirror, the threads, the file map, the tests |

Two conventions used throughout:

- **px/s, px/s²** — fwm's world is measured in pixels and its physics scale is
  100 px per metre, so earth gravity is 981 px/s². Every speed and acceleration
  in the config is in those units.
- **The world is one strip.** Ten desktops side by side in one coordinate space,
  10 screens wide; the screen is a window onto it. "Desktop 3" is a position on
  that strip, not a separate space.
