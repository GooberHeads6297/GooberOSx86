# GooberC Specification (v0)

## Source language (line-oriented v0)

| Form | Meaning |
|------|---------|
| `use goober.console` / `use goober.gui` | Import module (documentation + kind hint) |
| `use goober.auto` / `use goober.gfx3d` | Automation / 3D stub modules (kind hints) |
| `app console` / `app gui` / `app auto` / `app gfx3d` | App kind |
| `print "..."` | Console write |
| `window "Title" WxH` | Create GUI window |
| `text "..."` | Add text line to current window |
| `wait` | Wait until window dismissed |
| `sleep N` | Yield for ~N ms (`goober.auto` stub) |
| `clear 0xRRGGBBAA` | Clear 3D canvas (`goober.gfx3d` stub / no-op) |
| `exit` | Terminate |
| `# comment` | Comment |

Future versions may add expressions, variables, and `fn main()`.

## `.gob` file format

```
offset  size  field
0       4     magic = 'GOB\0' (0x00424F47 LE)
4       2     version = 1
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
| 0 NOP | — | — |
| 1 EXIT | u32 code | Terminate |
| 2 WRITE | u32 off, u32 len | Print rodata string |
| 3 YIELD | — | Yield to desktop |
| 4 GUI_CREATE | u32 title_off, u16 w, u16 h | Create window |
| 5 GUI_TEXT | u32 slot, u16 x, u16 y, u32 str_off | Add text |
| 6 GUI_WAIT | u32 slot | Wait for dismiss |
| 7 GUI_CLOSE | u32 slot | Close window |
| 8 SLEEP_MS | u32 ms | Stub sleep (frame yields) |
| 9 GFX3D_CLEAR | u32 rgba | Stub clear (reserved) |

## Syscall ABI (`int 0x80`)

| # | Name | Args |
|---|------|------|
| 1 | EXIT | code |
| 2 | WRITE | fd, buf, len |
| 3 | YIELD | — |
| 20 | GUI_WIN_CREATE | title, w, h |
| 21 | GUI_WIN_TEXT | win, x, y, text |
| 22 | GUI_WIN_WAIT | win |
| 23 | GUI_WIN_CLOSE | win |

Registers (x86_64): `rax`=num, `rdi`/`rsi`/`rdx`/`r10`=args, return in `rax`.

v0 apps typically use bytecode interpreted by the kernel loader; native
ring-3 entry + `int 0x80` is available for later GooberC backends.
