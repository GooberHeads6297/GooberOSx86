# goober.fs

Filesystem and path builtins for GooberC v2.

## use

```gooberc
use goober.fs
```

## Operations

| Form | Result |
|------|--------|
| `exists path` | 1 if path exists, else 0 |
| `read path` | File contents as string, or 0 on failure |
| `write path data` | Write string `data`; leaves 0/1 (often discarded) |
| `listdir path` | List of entry name strings, or 0 if missing |
| `path_join a b` | `"a/b"` (handles slashes) |
| `dirname p` / `basename p` | Path split |

```gooberc
use goober.console
use goober.fs

var dir = path_join "Apps" "src"
var names = listdir "Apps"
if names
  var i = 0
  while i < len names
    print get names i
    i = i + 1
  end
end
exit
```

## Errors

On failure, check `errmsg` (last VM error string) or a falsy return (0).

## GooberDOS shared folder

Guest `C:\` maps to host `/Dos`. Use Explorer DnD or copy into `/Dos` /
`/Dos/Apps`, then run via double-click, `rundos`, or `dos_run`
(see [`dos.md`](dos.md)).
