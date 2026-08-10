# goober.gui

Windowed GUI apps on the VESA desktop — text lines **or** a colorful 2D canvas
for artwork and simple games.

## use

```gooberc
use goober.gui
app gui
```

## Window + text lines

- `window "Title" WxH` — create a window
- `text "..."` / `text <expr>` — append a text line
- `cleargui` — clear lines (marks window interactive)
- `wait` — block until the user dismisses the window (non-interactive)

## Color canvas (v2.3)

Draw list for panels, tiles, and scenery. Prefer **named colors** (`NAVY`,
`RED`, `PANEL`, `INK`, …) — see [`color.md`](color.md). Hex `0xRRGGBB` still
works for custom shades.

- `fill color` — clear the canvas and set the background
- `rect x y w h color` — filled rectangle (client coords)
- `label x y str fg bg` — draw a string at `(x,y)`
- `present` — mark the window dirty (call after a frame’s draw list)

Example frame:

```gooberc
fill NAVY
rect 8 8 200 24 PANEL
label 16 14 "Score " + str score INK PANEL
rect 40 40 28 28 LIME
present
```

## Input & timing

- `getkey` — poll one key (0 if none); pumps a desktop frame
- `keyheld k` — `1` if key code `k` is currently held (does not consume queue)
- `millis` — monotonic milliseconds since boot (for `dt` in game loops)
- `winclosed` — 1 if the window was closed
- `str n` — int → decimal string
- `num s` — decimal string → int (for saved scores)
- `exit` — terminate
- `sleep N` — wait about `N` milliseconds (wall clock) while pumping the desktop

Key labels: `KEY_W` / `KEY_A` / `KEY_S` / `KEY_D`, arrows, `KEY_ESC`,
`KEY_SPACE`, `KEY_ENTER`, `KEY_F1`… — see [`keys.md`](keys.md).

Game-loop pattern (pace with `millis`, yield with a short `sleep`):

```gooberc
var last = millis
while winclosed == 0
  var now = millis
  var dt = now - last
  last = now
  # ... update using dt / keyheld KEY_W ...
  present
  sleep 1
end
```

Interactive games should `fill` + redraw each frame (or `cleargui` + `text`),
poll `getkey` / `keyheld`, and exit on ESC / `winclosed`. Prefer the canvas API
for boards and artwork — text lines are best for dialogs.

## Notes for agents

Start → Games titles (`Minesweeper`, `CubeDip`, `SnakeGame`, `DoomRay`) use the
canvas API. Keep draw lists under ~384 commands per frame. Named colors/keys and
hex literals work in both host and in-OS compilers.
