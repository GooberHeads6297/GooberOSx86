# Key labels & held keys

Friendly `KEY_*` names are integer literals matching codes from
`getkey` / the keyboard driver. Prefer these over magic numbers.

User variables with the same name take precedence.

## Special keys

| Name | Code |
|------|------|
| `KEY_ESC` | 27 |
| `KEY_ENTER` | 13 |
| `KEY_TAB` | 9 |
| `KEY_BACKSPACE` | 8 |
| `KEY_SPACE` | 32 |
| `KEY_UP` / `KEY_DOWN` / `KEY_LEFT` / `KEY_RIGHT` | 128–131 |
| `KEY_F1` … `KEY_F12` | `0x8B`–`0x96` |

## Letters & digits

`KEY_A` … `KEY_Z` are the **lowercase** codes actually delivered when
unshifted (`KEY_W` = 119 = `'w'`). `getkey` may return uppercase when Shift
is held; `keyheld KEY_W` still reports held for either case.

`KEY_0` … `KEY_9` are digit character codes (`'0'`…`'9'`).

## Punctuation (unshifted)

| Name | Char |
|------|------|
| `KEY_MINUS` | `-` |
| `KEY_EQUALS` | `=` |
| `KEY_LBRACKET` / `KEY_RBRACKET` | `[` `]` |
| `KEY_SEMICOLON` | `;` |
| `KEY_QUOTE` | `'` |
| `KEY_BACKTICK` | `` ` `` |
| `KEY_BACKSLASH` | `\` |
| `KEY_COMMA` / `KEY_DOT` / `KEY_SLASH` | `,` `.` `/` |

## `getkey` vs `keyheld`

- `getkey` — one pending press (0 if none); pumps a desktop frame.
- `keyheld k` — `1` if code `k` is currently down (PS/2 or USB), else `0`.
  Does not consume the press queue. Use for WASD-style game loops.

```gooberc
use goober.gui
app gui

window "Keys" 360x160
while winclosed == 0
  var k = getkey
  if k == KEY_ESC
    exit
  end
  fill NAVY
  if keyheld KEY_W
    label 16 40 "W held" LIME NAVY
  end
  if keyheld KEY_A
    label 16 60 "A held" LIME NAVY
  end
  present
  sleep 16
end
exit
```

Also see [`color.md`](color.md) for named RGB colors.
