# GooberC

GooberC is the programming language for **GooberOS apps**. Source files use
the `.gc` extension and compile to `.gob` executables that run on an
**installed** GooberOS volume (not the live USB environment).

## Why GooberC

- Small, explicit surface — easy for humans and coding agents
- `use goober.*` modules map 1:1 to docs in [`libs/`](libs/)
- One toolchain path: `gooberc app.gc -o Apps/app.gob` then `run Apps/app.gob`

## Quick start (on installed GooberOS)

```text
cd Apps/src
gooberc Welcome.gc -o Apps/Welcome.gob
run Apps/Welcome.gob
```

Host-side (bring-up / agents):

```bash
python3 gooberc/gooberc.py gooberc/examples/Welcome.gc -o Welcome.gob
```

## Hello console

```gooberc
use goober.console
app console
print "Hello from GooberC"
exit
```

## Hello GUI

```gooberc
use goober.gui
app gui
window "My App" 400x200
text "Hello GUI"
wait
exit
```

## Layout on installed disk

| Path | Purpose |
|------|---------|
| `/Config/boot.cfg` | `HideGRUB=true\|false` |
| `/Apps/*.gob` | Installed Goober apps |
| `/Apps/src/*.gc` | Example / user sources |
| `/usr/bin/gooberc` | Note: compiler is the shell `gooberc` command in v0 |
| `/lib/goober/` | Module notes for agents |

See [SPEC.md](SPEC.md) for bytecode, `.gob` layout, and syscall ABI.
