# goober.dos

Launch GooberDOS (soft MS-DOS guest) from GooberC.

## use

```gooberc
use goober.fs
```

(No separate `use goober.dos` required — `dos_run` is a core builtin.)

## Operations

| Form | Result |
|------|--------|
| `dos_run path` | Start GooberDOS on a `.COM`/`.EXE` under `/Dos` (or any VFS path); 1 on launch ok |

```gooberc
use goober.console
dos_run "Dos/Apps/HELLO.COM"
dos_run "Dos/Apps/VER.COM"
exit
```

## Shared folder

Guest `C:\` → host `/Dos`. Drop apps into `/Dos` or `/Dos/Apps` with File
Explorer (drag-and-drop or copy), then double-click or `dos_run`.

## Notes

GooberDOS targets real-mode DOS apps (files, text, Mode 13h, mouse) **without
sound**. See [`dosemu/README.md`](../../dosemu/README.md).
