# goober.gfx3d

Reserved 3D / canvas module. Opcodes compile today; rendering is a stub.

## use

```gooberc
use goober.gfx3d
```

## Operations (v0 source)

- `clear 0xRRGGBBAA` — clear canvas to a color (no-op until the 3D module lands)
- `print "..."` — optional console note while prototyping
- `exit` — terminate

## Planned (not in this pass)

- Create canvas / present frame
- Mesh + camera helpers
- Real GPU or software raster path

## Notes for agents

Prefer documenting intent with `use goober.gfx3d` and `clear` so samples stay
forward-compatible. Do not assume pixels are drawn yet.
