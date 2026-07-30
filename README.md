# hyprphysics

Gravity, bouncing, and throwable windows for [Hyprland](https://github.com/hyprwm/Hyprland),
loosely inspired by the physics model in [iluaii/fwm](https://github.com/iluaii/fwm)
(a standalone compositor that drives its windows with Box2D). This plugin
reimplements the same idea — mass, gravity, restitution, friction, throwing —
as a lightweight custom stepper on top of Hyprland's own animation system,
since Hyprland plugins can't embed a full physics engine the way a compositor
core can.

What it does:
- Floating windows fall, under a configurable gravity.
- They bounce off monitor edges with configurable restitution (bounciness)
  and lose energy to friction over time.
- Windows can push each other around (togglable).
- Dragging a window and letting go "throws" it — the plugin measures your
  drag speed and hands the window to gravity with that velocity.
- A `physics:throw` dispatcher lets you throw the active window from a
  keybind with an explicit velocity.

Windows that are tiled, pinned, fullscreen, or on a workspace that isn't
currently visible are left alone.

## Building

Requires the Hyprland headers for the version you're running (install the
`-devel`/`-git` package for your distro, or point pkg-config at a built
Hyprland source tree) plus a C++23 compiler.

### Via hyprpm (recommended)

```sh
hyprpm add https://github.com/alexankitty/hyprphysics
hyprpm enable hyprphysics
```

### Manually

```sh
make all
# then, either point your config at the .so directly (see below),
# or: hyprctl plugin load "$(pwd)/hyprphysics.so"
```

`CMakeLists.txt` and `meson.build` are also provided if you'd rather build
with those.

## Loading the plugin

**hyprland.conf (legacy hyprlang):**
```
plugin = /absolute/path/to/hyprphysics.so
```

**hyprland.lua (current, since Hyprland 0.55):**
```lua
hl.plugin_load("/absolute/path/to/hyprphysics.so")
```

If you install via `hyprpm`, it handles loading for you either way — just
`hyprpm enable hyprphysics` and it's active on the next reload.

## Configuration

All options live under `plugin:physics:*`. Because the plugin registers them
through Hyprland's normal config-value system, they work identically from
either config language — set whichever one matches your `hyprland.conf` /
`hyprland.lua`.

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Master on/off switch |
| `collisions` | `true` | Let windows push each other around |
| `affect_tiled` | `false` | Also simulate tiled windows (rarely wanted) |
| `monitor_traversal` | `false` | Let windows fall/bounce across monitor edges instead of stopping at their own monitor's bounds |
| `gravity` | `1400` | Downward acceleration, px/s² |
| `restitution` | `0.45` | Bounciness on impact, 0..1 |
| `friction` | `0.86` | Velocity retained per second, 0..1 |
| `throw_multiplier` | `1.0` | Scales `physics:throw`'s requested velocity |
| `max_velocity` | `6000` | Hard speed cap, px/s |
| `sleep_velocity` | `12` | Below this speed a resting window stops simulating, px/s |
| `grab_release_ticks` | `2` | Ticks of stillness after a drag before gravity resumes |

**hyprland.conf:**
```
plugin {
    physics {
        gravity = 1600
        restitution = 0.6
        friction = 0.9
        collisions = true
    }
}
```

**hyprland.lua:**
```lua
hl.config({
    plugin = {
        physics = {
            gravity = 1600,
            restitution = 0.6,
            friction = 0.9,
            collisions = true,
        },
    },
})
```

## Dispatchers

- `physics:throw <vx> <vy>` — throws the currently active window with the
  given velocity in px/s (negative `vy` is up).
- `physics:toggle` — flips the runtime on/off switch (separate from the
  `enabled` config value, so a keybind can pause physics without touching
  your config).
- `physics:stop` — zeroes every window's velocity in place.

These are plain Hyprland dispatchers, so they work the same way any other
plugin's dispatchers do:

**hyprland.conf:**
```
bind = SUPER SHIFT, T, exec, hyprctl dispatch physics:throw "0 -1200"
bind = SUPER SHIFT, P, physics:toggle
```

**hyprland.lua:**
```lua
-- plugin dispatchers use the legacy name+args form, invoked via exec_raw
hl.bind("SUPER SHIFT + T", hl.dsp.exec_raw('physics:throw 0 -1200'))
hl.bind("SUPER SHIFT + P", hl.dsp.exec_raw('physics:toggle'))
```

or from anywhere (a script, waybar, etc.) via IPC regardless of which config
language you use:
```sh
hyprctl dispatch physics:throw "0 -1200"
```

## Known limitations

- Reserved screen areas (bars, layer-shell exclusive zones) aren't accounted
  for — the floor/walls are the monitor's full geometry, not the usable
  work area.
- Collision response is a simple AABB/impulse model, not a real rigid-body
  solver — good enough for satisfying bounces, not for anything that needs
  to be physically exact.
- `monitor_traversal` walls/floors a window against whichever monitor it's
  currently over (falling into a gap between non-adjacent monitors falls
  back to the nearest one), so floors are respected per-monitor even when
  monitors differ in height or aren't aligned in a grid. It doesn't,
  however, reassign which monitor/workspace a window belongs to as it
  drifts across a seam — Hyprland's own window-monitor tracking is
  workspace-driven, not position-driven, so how a straddling window renders
  once it's mostly over the neighboring screen depends on Hyprland itself,
  not this plugin.
- This targets Hyprland's current (git `main`) plugin API. Plugin ABI is
  explicitly unstable between Hyprland releases, so you'll need to rebuild
  against the headers matching whatever Hyprland version you run — `hyprpm`
  does this automatically.

## License

GPLv2, to match Hyprland's own plugin ecosystem.
