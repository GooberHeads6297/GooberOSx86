# GooberC Specification (v2)

GooberC is a **concise, line-oriented** language for GooberOS apps — not a C
clone. Keywords, optional indentation, blocks closed with `end`, minimal
punctuation. Humans and agents both author it easily.

**Breaking change:** `.gob` version is **2**. Version-1 bytecode is rejected.

## Source language

| Form | Meaning |
|------|---------|
| `use goober.console` / `use goober.gui` / `use goober.fs` | Module hint (stdlib builtins) |
| `use goober.auto` / `use goober.gfx3d` | Automation / 3D stub modules |
| `app console` / `app gui` / `app auto` / `app gfx3d` | App kind |
| `var name = <expr>` | Declare + assign (global, or local inside `fn`) |
| `name = <expr>` | Assign |
| `print "..."` | Console write string |
| `print <expr>` | Print int or string + newline |
| `if <cond>` … `end` | Conditional |
| `while <cond>` … `end` | Loop |
| `for name = a to b` … `end` | Inclusive integer loop |
| `break` / `continue` | Loop control |
| `fn name a b` … `end` | Function with named args (locals) |
| `call name args…` | Call with 0+ args |
| `return` / `return <expr>` | Return (optional value) |
| `window "Title" WxH` / `text "..."` / `text expr` / `wait` | GUI |
| `cleargui` / `getkey` / `winclosed` / `str n` | Interactive GUI |
| `if` / `else` / `end` | Conditional with optional else |
| `sleep N` / `clear 0xRRGGBBAA` | wall-clock ms sleep (GUI pumps desktop) / gfx3d stub |
| `exit` | Terminate |
| `# comment` | Comment |
| `RED` / `NAVY` / `PANEL` / … | Named RGB color literals (see `libs/color.md`) |
| `KEY_UP` / `KEY_ESC` / … | Named key-code literals |

### Values and builtins

| Kind | Forms |
|------|--------|
| int / bool | Literals, comparisons → 0/1 |
| str | `"..."`; `+` concat; `len`; `slice s a b`; `find hay needle` |
| list | `[1, 2, 3]`; `push` / `get` / `set` / `len` (grow up to 256) |
| map | `map`; `set m "k" v`; `get m "k"`; `len` (max 64 string keys) |
| heap | `alloc n` / `free p` (blob handles; no pointer syntax) |
| path | `path_join a b`, `dirname p`, `basename p` |
| fs | `exists`, `read`, `write`, `listdir path` → list of name strings |
| meta | `typeof v` (0 int, 1 str, 2 list, 3 blob, 4 map), `errmsg` |

Statement forms: `write path data`, `push list val`, `set container key val`.

### Expressions

Integers, names (global/local), string/list literals, `+ - * /`, parentheses,
comparisons `== != < <= > >=`. Globals ≤ 128; locals ≤ 32 per frame.

### Example

```text
use goober.console
use goober.fs

fn greet name
  print "hi "
  print name
end

var n = 3
while n > 0
  call greet "tick"
  n = n - 1
end

var data = read "Config/settings.cfg"
if data
  print len data
end
exit
```

### Out of syntax (by design)

C types everywhere, `{}`, `;`, `->`, headers, `int main(void)`, pointer
arithmetic in source.

### GooberDOS (soft MS-DOS guest)

| Item | Status |
|------|--------|
| Soft 8086 guest | [`dosemu/`](../dosemu/) — BSD, not DOSBox |
| Shared folder | Guest `C:\` → host `/Dos` |
| Launch | Explorer `.com`/`.exe`, `rundos`, GooberC `dos_run path` |
| DnD into `/Dos` | Explorer file drag → drop on desktop / Dos |

In-OS teaching docs for GooberC+DOS can expand under `/usr/share/gooberc/`
as the guest grows. See [`dosemu/README.md`](../dosemu/README.md).

## `.gob` file format

```
offset  size  field
0       4     magic = 'GOB\0' (0x00424F47 LE)
4       2     version = 2
6       1     arch (1=i386, 2=x86_64)
7       1     kind (1=console, 2=gui, 3=auto, 4=gfx3d)
8       4     flags (bit0 = bytecode)
12      4     entry (bytecode IP / native RVA)
16      4     code_size
20      4     rodata_size
24      4     reserved
28      …     code
…       …     rodata
```

## Bytecode opcodes

| Op | Encoding | Action |
|----|----------|--------|
| 0–9 | (v1) | NOP, EXIT, WRITE, YIELD, GUI_*, SLEEP_MS, GFX3D_CLEAR |
| 10 PUSH_I | i32 | Push integer |
| 11 LOAD / 12 STORE | u8 | Global |
| 13–16 | — | ADD SUB MUL DIV (ADD also joins strings) |
| 17–22 CMP_* | — | Comparisons |
| 23 JMP / 24 JZ | u32 IP | Control |
| 25 CALL | u32 IP | Call arity 0 |
| 26 RET | — | Return |
| 27 PRINT_I | — | Print int or string + newline |
| 28 LOAD_LOCAL / 29 STORE_LOCAL | u8 | Frame locals |
| 30 CALL_N | u32 IP, u8 arity | Call with args |
| 31 RET_V | — | Return with stack value |
| 32 PUSH_STR | u32 off | Push string object from rodata |
| 33 LEN | — | str/list length |
| 34 LIST_NEW | u8 n | Pop n ints → list |
| 35 LIST_PUSH / 36 LIST_GET | — | List mutate / index |
| 37 ALLOC / 38 FREE | — | Heap blob |
| 39 FS_EXISTS / 40 FS_READ / 41 FS_WRITE | — | Filesystem |
| 42 STR_JOIN | — | Concat strings |
| 43 DUP / 44 POP | — | Stack helpers |
| 45 PRINT_RAW | — | Print string, no newline |
| 46 STR_SLICE | — | end, start, str → substr |
| 47 STR_FIND | — | needle, hay → index or -1 |
| 48 PATH_JOIN | — | b, a → joined path |
| 49 PATH_DIR / 50 PATH_BASE | — | dirname / basename |
| 51 FS_LIST | — | path → list of names |
| 52 SET | — | val, key/idx, list\|map |
| 53 TYPEOF | — | type code |
| 54 LAST_ERR | — | last runtime error string |
| 55 MAP_NEW | — | empty map |
| 56 DOS_RUN | — | path str → launch GooberDOS (0/1) |
| 57 KEY_POLL | — | push key (0=none); pump frame |
| 58 GUI_CLEAR | — | clear window lines; interactive |
| 59 GUI_TEXT_S | — | pop str → append line |
| 60 STR_I | — | pop int → decimal str |
| 61 GUI_CLOSED | — | push 1 if window closed |
| 62 GFX_FILL | — | pop rgb → canvas clear/bg |
| 63 GFX_RECT | — | pop rgb,h,w,y,x → filled rect |
| 64 GFX_LABEL | — | pop bg,fg,str,y,x → text at x,y |
| 65 GFX_PRESENT | — | dirty canvas window |
| 66 NUM | — | pop str → parse decimal int |

Surface syntax: `fill` / `rect` / `label` / `present` / `num` (see [libs/gui.md](libs/gui.md)).
Hex integer literals (`0xRRGGBB`) are accepted. Call arguments bind at sum
precedence so `mod x 10 == 9` means `(mod x 10) == 9`.

VM: operand stack (128), globals (128), frames (64) with locals (32),
object table + ~128KB heap. Lists grow on the heap (max 256). Object
handles set bit 31. Runtime errors set `errmsg` and often print a short
message; version mismatches say which version was expected.

## Syscall ABI (`int 0x80`)

| # | Name | Args |
|---|------|------|
| 1 | EXIT | code |
| 2 | WRITE | fd, buf, len |
| 3 | YIELD | — |
| 4–6 | OPEN / CLOSE / READ | path / fd / buf |
| 20–23 | GUI_WIN_* | window ops |

Bytecode apps use the in-kernel VM; native ring-3 + `int 0x80` remains for
later backends.
