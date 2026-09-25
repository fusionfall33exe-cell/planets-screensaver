# planets — the solar system in your terminal

A screensaver that tours the solar system right inside your terminal, written
in plain C (just libc and libm).

Every frame is ray traced per pixel:

- perfectly round planets with anti-aliased edges
- procedural surfaces: continents, clouds and city lights on Earth, Jupiter's
  flowing bands and Great Red Spot, craters on Mercury and the Moon
- glowing atmospheres and see-through rings with shadows on them
- moons that orbit and cast shadows on their planet
- a slowly drifting camera, a Milky Way sky, and crossfades between scenes

It draws with 24-bit ANSI colors. Each character cell holds two pixels (the
`▀` glyph with separate foreground and background colors), and only cells
that changed are redrawn. Rendering is split across up to 8 CPU cores, so it
holds 60 fps even in a fullscreen terminal.

| # | Scene | # | Scene |
|---|---|---|---|
| 1 | Mercury | 6 | Saturn and Titan |
| 2 | Venus | 7 | Uranus, on its side |
| 3 | Earth and the Moon | 8 | Neptune and Triton |
| 4 | Mars, Phobos, Deimos | 9 | The whole solar system |
| 5 | Jupiter and its four big moons | | |

The tour loops forever until you quit or close the terminal.

## Build and install (Linux / WSL)

```bash
make            # build ./planets
make install    # install the command "Planets" to ~/.local/bin
```

On Windows with WSL, put this folder on your `PATH`: `Planets.cmd` then starts
the installed program inside WSL, so `Planets` works in cmd and PowerShell too.

## Usage

```
Planets [-t SEC] [-s N] [-r] [-f FPS] [-a] [-2] [-x]
  -t SEC   seconds per planet (default 25, 0 = stay on one)
  -s N     start with scene N (1-9)
  -r       random order instead of the tour from the Sun outwards
  -f FPS   frames per second (default 60; try 30 if it stutters)
  -a       ASCII shading instead of half-block pixels
  -2       256 colors, for terminals without 24-bit color
  -x       screensaver mode: any key quits
```

Keys: `q`/`Esc` quit, `→`/`n` next, `←`/`b` previous, `space` pause,
`m` ASCII/blocks, `c` 256/24-bit colors, `+`/`-` speed.

Works best in a terminal with 24-bit color and a font where `▀` fills the
top half of the cell (Windows Terminal, GNOME Terminal, Konsole, kitty,
Alacritty, iTerm2, ...).
