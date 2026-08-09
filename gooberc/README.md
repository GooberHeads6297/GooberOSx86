# GooberC (v2)

GooberC is the programming language for **GooberOS apps**. Source files use
the `.gc` extension and compile to `.gob` bytecode (version **2**) that runs
on an installed GooberOS volume.

Easy syntax for humans and agents: keywords, `end` blocks, no C braces or
semicolons. Not a C clone — see [SPEC.md](SPEC.md).

## Why GooberC

- Short, explicit surface — easy for humans and coding agents
- `use goober.*` modules map to real builtins + docs in [`libs/`](libs/)
- Named colors (`RED`, `NAVY`, `PANEL`, …) and keys (`KEY_UP`, …) — [`libs/color.md`](libs/color.md)
- Dual compilers stay in sync: in-OS `gooberc` and host `gooberc/gooberc.py`
- Strong enough for installers/FS tools; DOS guest is a later follow-up

## Quick start

On installed GooberOS:

```text
cd Apps/src
gooberc Welcome.gc -o Apps/Welcome.gob
run Apps/Welcome.gob
```

Host-side (bring-up / agents):

```bash
python3 gooberc/gooberc.py gooberc/examples/HelloArgs.gc -o HelloArgs.gob
```

## Console + args + for

```gooberc
use goober.console

fn greet name
  print "hi "
  print name
end

call greet "Goober"
for i = 1 to 3
  print i
end
exit
```

## Lists and FS

```gooberc
use goober.console
use goober.fs

var xs = [10, 20, 30]
print get xs 1
push xs 40

var data = read "Config/settings.cfg"
if data
  print len data
end
exit
```

## GUI

Text dialogs or a colorful 2D canvas (`fill` / `rect` / `label` / `present`):

```gooberc
use goober.gui
app gui
window "My App" 400x200
fill NAVY
rect 20 20 120 40 PANEL
label 28 32 "Hello" INK PANEL
present
while 1
  if winclosed
    exit
  end
  var k = getkey
  if k == 27
    exit
  end
  sleep 20
end
```

See [libs/gui.md](libs/gui.md). Games: `Minesweeper.gc`, `CubeDip.gc`,
`SnakeGame.gc`. Other examples: `Welcome.gc`, `HelloArgs.gc`, `FsTool.gc`.
Full surface in [SPEC.md](SPEC.md).

## Paths, strings, maps

```gooberc
use goober.console
use goober.fs

var p = path_join "Apps" "src"
print basename p
print slice "GooberOS" 0 6

var m = map
set m "ver" 2
print get m "ver"

var names = listdir "Apps"
if names
  print len names
end
exit
```

## GooberDOS

Soft MS-DOS guest (BSD). Shared folder `/Dos`, Explorer `.com`/`.exe`,
shell `rundos`, GooberC `dos_run`. See [`dosemu/README.md`](../dosemu/README.md)
and [`libs/dos.md`](libs/dos.md).

```gooberc
dos_run "Dos/Apps/HELLO.COM"
exit
```

## Modules

| Module | Docs |
|--------|------|
| console | [`libs/console.md`](libs/console.md) |
| gui | [`libs/gui.md`](libs/gui.md) |
| fs | [`libs/fs.md`](libs/fs.md) |
| auto / gfx3d | stubs — [`libs/auto.md`](libs/auto.md), [`libs/gfx3d.md`](libs/gfx3d.md) |

## Layout on installed disk

| Path | Purpose |
|------|---------|
| `/Config/boot.cfg` | `HideGRUB=true\|false` |
| `/Apps/*.gob` | Installed Goober apps |
| `/Apps/src/*.gc` | Example / user sources |
| `/lib/goober/` | Module notes for agents |

**Note:** Old v1 `.gob` files are rejected (version must be 2). Recompile
sources with current `gooberc`.

## Follow-up (not this release)

DOS-compatible guest / V86, shared `/Dos` or `/Share`, GooberC as
installers/launchers for guest tooling.
