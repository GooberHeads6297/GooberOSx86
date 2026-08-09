# GooberDOS

BSD soft MS-DOS guest for GooberOS — **DOSBox-class goals**, **not** GPL DOSBox code.

Sound I/O is ignored (no SB/AdLib yet).

## Session (like DOSBox)

Open **GooberDOS** from the desktop / Start menu, or:

```
rundos
rundos Dos/Apps/HELLO.COM
```

You get a live `C:\>` prompt (guest `C:\` → host `/Dos`):

| Command | Action |
|---------|--------|
| `DIR` | List directory |
| `CD path` | Change directory |
| `TYPE file` | Print file |
| `CLS` | Clear screen |
| `VER` / `MEM` / `HELP` | Info |
| `CYCLES [n\|UP\|DOWN\|MAX]` | Speed control |
| `EXIT` | Close session |
| `HELLO` / `APPS\FOO.COM` | Run a program |

After a program exits (`AH=4Ch`), you return to the prompt (session stays open).

**Cycles:** `F11` slower / `F12` faster (also while a guest is running). Status bar shows cycles, `S`hell/`R`unning, video mode, `PE` if protected, and `CS:IP`. `Ctrl+C` aborts a guest back to the prompt.

## Roadmap (DOSBox-class, clean-room)

Building a full DOSBox replacement inside GooberOS is the right long-term goal; it is a **multi-phase** project:

1. **Real-mode DOS** — prompt, INT 21 files, MCB, simple `.COM`/MZ (largely here)
2. **386 + raw PM** — CR0/GDT, INT 15 himem, DOS/16M bring-up (in progress)
3. **DPMI / VCPI / XMS** — real hosts (DPMI stub removed; re-add when solid)
4. **VGA** — Mode 13h/X, DAC, vsync waits (partial Mode 13h)
5. **Timers / IRQ** — PIT, IRQ0, keyboard IRQ1 (PIT ports stubbed; more to do)
6. **Sound** — SB16/AdLib (outs ignored today)
7. **Games** — Doom-class titles once 2–6 hold

Until then, desktop **GooberDoom** is the playable Doom path.

## Protected mode / DOS extenders

Experimental path for DOS/4GW-style programs:

- 8 MiB guest arena (1 MiB conventional + extended; `INT 15h` AH=87h/88h/E801h)
- Operand/address-size prefixes (`66h`/`67h`), `0Fh` escapes, soft x87 probe ops, `LOCK` (`F0h`)
- AT identity: model `F000:FFFE=FCh`, `INT 15h AH=C0h`, CMOS 70h/71h
- EXE layout `MCB+PSP+image` so `AH=4A`/`AH=48` (tstack) work
- CR0.PE, GDT caches, far jumps (paging ignored — identity map)
- DPMI **not advertised** (fake host caused error [32])

Blank window + status `R` usually means the guest is still executing (often Mode 13h black or a wait loop). Watch `m13` / `PE` / whether `CS:IP` moves; try `F12` for more cycles; `Ctrl+C` returns to the shell.

## DOOM shareware (optional bundle)

```bash
bash scripts/fetch-doom-shareware.sh   # DOOM.EXE + DOOM1.WAD → fixtures/doom/
bash scripts/build-x64.sh              # seeds into /Dos/Apps on the ISO
```

Under GooberDOS:

```
CD APPS
DOOM
```

id’s shareware is freely redistributable; binaries are gitignored and fetched at build time (`GOOBER_FETCH_DOOM=0` skips the fetch).

## Runtime

8086/186 + 386/PM CPU, PSP/IVT/MCB, INT 21 files + cwd, INT 10 text/Mode 13h, INT 16 scancodes, INT 33 mouse, B800/A000 VRAM, PIT port stubs.

## License

BSD 3-Clause with GooberOS. Do not vendor GPL DOSBox code here.
id Software retains copyright on DOOM.EXE / DOOM1.WAD.
