# goober.gui

Simple windowed GUI apps on the VESA desktop.

## use

```gooberc
use goober.gui
app gui
```

## Operations (v0 source)

- `window "Title" WxH` — create a window
- `text "..."` — append a text line
- `wait` — block until the user dismisses the window
- `exit` — terminate

## Notes for agents

Use for Welcome-style apps and settings dialogs. One window per app is the
v0 model; keep strings short.
