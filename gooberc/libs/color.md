# Named colors & keys

GooberC recognizes friendly **color names** and **key labels** as integer
literals (`0xRRGGBB` / key codes). No `var` declaration needed.

User variables with the same name take precedence over the built-in label.

## Colors (RGB)

| Name | Value | Name | Value |
|------|-------|------|-------|
| `BLACK` | `#000000` | `WHITE` | `#FFFFFF` |
| `GRAY` / `GREY` | `#808080` | `SILVER` | `#C0C0C0` |
| `LIGHTGRAY` | `#D3D3D3` | `DARKGRAY` | `#404040` |
| `RED` | `#E74C3C` | `DARKRED` | `#8B0000` |
| `GREEN` | `#2ECC71` | `DARKGREEN` | `#196F3D` |
| `LIME` | `#4ADE80` | `BLUE` | `#3498DB` |
| `DARKBLUE` | `#1A5276` | `NAVY` | `#1B2838` |
| `SKY` | `#87CEEB` | `CYAN` | `#1ABC9C` |
| `TEAL` | `#148F77` | `AQUA` | `#00FFFF` |
| `YELLOW` | `#F1C40F` | `GOLD` | `#FFD700` |
| `ORANGE` | `#E67E22` | `BROWN` | `#8B4513` |
| `PURPLE` | `#9B59B6` | `INDIGO` | `#6D28D9` |
| `VIOLET` | `#8E44AD` | `MAGENTA` | `#FF00FF` |
| `PINK` | `#FF69B4` | `CORAL` | `#FF6B6B` |
| `MAROON` | `#800000` | `OLIVE` | `#808000` |
| `PANEL` | `#243447` | `INK` | `#EEF2F7` |
| `MUTED` | `#9FB3C8` | | |

Hex literals (`0xRRGGBB`) still work for one-off shades.

## Keys

See [`keys.md`](keys.md) for the full table (`KEY_A`…`KEY_Z`, digits, F-keys,
punctuation) and `keyheld`.

Quick reference: `KEY_ESC` / `KEY_ENTER` / `KEY_SPACE` / arrows /
`KEY_W`…`KEY_D` (lowercase codes) / `KEY_F1`…`KEY_F12`.

## Example

```gooberc
use goober.gui
app gui

window "Demo" 320x180
fill NAVY
rect 16 16 120 40 PANEL
label 24 28 "Hello" INK PANEL
present
while winclosed == 0
  if getkey == KEY_ESC
    exit
  end
  sleep 40
end
exit
```
