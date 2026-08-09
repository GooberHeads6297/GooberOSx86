# goober.console

Console I/O and core language builtins.

## use

```gooberc
use goober.console
```

## Operations

| Form | Meaning |
|------|---------|
| `print "..."` / `print <expr>` | Write to console |
| `exit` | Terminate |
| `len x` | Length of string, list, or map |
| `slice s a b` | Substring `[a, b)` |
| `find hay needle` | Index of substring, or -1 |
| `alloc n` / `free p` | Heap blob |
| `map` | Empty string-keyed map |
| `get` / `set` / `push` | List or map access |
| `typeof v` | 0=int 1=str 2=list 3=blob 4=map |
| `errmsg` | Last runtime error string |

## Control

`fn` / `call` / `return`, `if` / `while` / `for` / `break` / `continue`.

## Notes for agents

Keep the line-oriented `end`-block style. Do not invent C syntax. Prefer
`errmsg` after failed `read` / `listdir` / `set`. In-OS tutorial documents
arrive after the DOS guest work; until then use [SPEC.md](../SPEC.md) and
these lib notes.
