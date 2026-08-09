#!/usr/bin/env python3
"""Host-side GooberC compiler (v2). Mirrors in-OS gooberc — easy syntax, not C."""
import argparse
import struct
import sys

GOB_MAGIC = 0x00424F47
GOB_VERSION = 2
GOB_ARCH_X86_64 = 2
GOB_KIND_CONSOLE = 1
GOB_KIND_GUI = 2
GOB_KIND_AUTO = 3
GOB_KIND_GFX3D = 4
GOB_FLAG_BYTECODE = 1

GBC_EXIT = 1
GBC_WRITE = 2
GBC_GUI_CREATE = 4
GBC_GUI_TEXT = 5
GBC_GUI_WAIT = 6
GBC_SLEEP_MS = 8
GBC_GFX3D_CLEAR = 9
GBC_PUSH_I = 10
GBC_LOAD = 11
GBC_STORE = 12
GBC_ADD = 13
GBC_SUB = 14
GBC_MUL = 15
GBC_DIV = 16
GBC_CMP_EQ = 17
GBC_CMP_NE = 18
GBC_CMP_LT = 19
GBC_CMP_LE = 20
GBC_CMP_GT = 21
GBC_CMP_GE = 22
GBC_JMP = 23
GBC_JZ = 24
GBC_CALL = 25
GBC_RET = 26
GBC_PRINT_I = 27
GBC_LOAD_LOCAL = 28
GBC_STORE_LOCAL = 29
GBC_CALL_N = 30
GBC_RET_V = 31
GBC_PUSH_STR = 32
GBC_LEN = 33
GBC_LIST_NEW = 34
GBC_LIST_PUSH = 35
GBC_LIST_GET = 36
GBC_ALLOC = 37
GBC_FREE = 38
GBC_FS_EXISTS = 39
GBC_FS_READ = 40
GBC_FS_WRITE = 41
GBC_STR_JOIN = 42
GBC_DUP = 43
GBC_POP = 44
GBC_PRINT_RAW = 45
GBC_STR_SLICE = 46
GBC_STR_FIND = 47
GBC_PATH_JOIN = 48
GBC_PATH_DIR = 49
GBC_PATH_BASE = 50
GBC_FS_LIST = 51
GBC_SET = 52
GBC_TYPEOF = 53
GBC_LAST_ERR = 54
GBC_MAP_NEW = 55
GBC_DOS_RUN = 56
GBC_KEY_POLL = 57
GBC_GUI_CLEAR = 58
GBC_GUI_TEXT_S = 59
GBC_STR_I = 60
GBC_GUI_CLOSED = 61
GBC_GFX_FILL = 62
GBC_GFX_RECT = 63
GBC_GFX_LABEL = 64
GBC_GFX_PRESENT = 65
GBC_NUM = 66

MAX_LOCALS = 32
MAX_GLOBALS = 128

# Named RGB colors (0xRRGGBB). User vars with the same name take precedence.
COLORS = {
    "BLACK": 0x000000,
    "WHITE": 0xFFFFFF,
    "GRAY": 0x808080,
    "GREY": 0x808080,
    "SILVER": 0xC0C0C0,
    "LIGHTGRAY": 0xD3D3D3,
    "LIGHTGREY": 0xD3D3D3,
    "DARKGRAY": 0x404040,
    "DARKGREY": 0x404040,
    "RED": 0xE74C3C,
    "DARKRED": 0x8B0000,
    "GREEN": 0x2ECC71,
    "DARKGREEN": 0x196F3D,
    "LIME": 0x4ADE80,
    "BLUE": 0x3498DB,
    "DARKBLUE": 0x1A5276,
    "NAVY": 0x1B2838,
    "SKY": 0x87CEEB,
    "CYAN": 0x1ABC9C,
    "TEAL": 0x148F77,
    "AQUA": 0x00FFFF,
    "YELLOW": 0xF1C40F,
    "GOLD": 0xFFD700,
    "ORANGE": 0xE67E22,
    "BROWN": 0x8B4513,
    "PURPLE": 0x9B59B6,
    "INDIGO": 0x6D28D9,
    "VIOLET": 0x8E44AD,
    "MAGENTA": 0xFF00FF,
    "PINK": 0xFF69B4,
    "CORAL": 0xFF6B6B,
    "MAROON": 0x800000,
    "OLIVE": 0x808000,
    "PANEL": 0x243447,
    "INK": 0xEEF2F7,
    "MUTED": 0x9FB3C8,
    "TRANSPARENT": 0x000000,
}

# Key codes from the OS keyboard driver (unsigned char values).
KEYS = {
    "KEY_ESC": 27,
    "KEY_ENTER": 13,
    "KEY_SPACE": 32,
    "KEY_UP": 128,
    "KEY_DOWN": 129,
    "KEY_LEFT": 130,
    "KEY_RIGHT": 131,
}


class Compiler:
    def __init__(self):
        self.code = bytearray()
        self.rodata = bytearray()
        self.globals = {}
        self.fns = {}  # name -> {entry, arity, args}
        self.blocks = []  # (kind, ...)
        self.pend = []  # (patch_at, name, arity)
        self.kind = GOB_KIND_CONSOLE
        self.has_exit = False
        self.in_fn = None  # current fn dict or None
        self.locals = {}  # name -> slot when in_fn

    def emit(self, *bs):
        self.code.extend(bs)

    def emit_u32(self, op, v):
        self.emit(op)
        self.code.extend(struct.pack("<I", v & 0xFFFFFFFF))

    def emit_i32(self, op, v):
        self.emit_u32(op, v)

    def emit_u8op(self, op, v):
        self.emit(op, v & 0xFF)

    def add_str(self, s):
        off = len(self.rodata)
        self.rodata.extend(s.encode("utf-8") + b"\0")
        return off

    def ensure_global(self, name):
        if name not in self.globals:
            if len(self.globals) >= MAX_GLOBALS:
                raise ValueError("too many globals")
            self.globals[name] = len(self.globals)
        return self.globals[name]

    def ensure_local(self, name):
        if name not in self.locals:
            if len(self.locals) >= MAX_LOCALS:
                raise ValueError("too many locals")
            self.locals[name] = len(self.locals)
        return self.locals[name]

    def resolve_var(self, name):
        """Return ('local'|'global', slot) or None."""
        if self.in_fn is not None and name in self.locals:
            return ("local", self.locals[name])
        if name in self.globals:
            return ("global", self.globals[name])
        return None

    def emit_load_name(self, name):
        r = self.resolve_var(name)
        if r is None:
            raise ValueError(f"unknown var {name}")
        kind, slot = r
        if kind == "local":
            self.emit_u8op(GBC_LOAD_LOCAL, slot)
        else:
            self.emit_u8op(GBC_LOAD, slot)

    def emit_store_name(self, name):
        r = self.resolve_var(name)
        if r is None:
            if self.in_fn is not None:
                slot = self.ensure_local(name)
                self.emit_u8op(GBC_STORE_LOCAL, slot)
            else:
                slot = self.ensure_global(name)
                self.emit_u8op(GBC_STORE, slot)
            return
        kind, slot = r
        if kind == "local":
            self.emit_u8op(GBC_STORE_LOCAL, slot)
        else:
            self.emit_u8op(GBC_STORE, slot)

    def skip_ws(self, s, i):
        while i < len(s) and s[i] in " \t\r":
            i += 1
        return i

    def parse_ident(self, s, i):
        i = self.skip_ws(s, i)
        if i >= len(s) or not (s[i].isalpha() or s[i] == "_"):
            return None, i
        j = i
        while j < len(s) and (s[j].isalnum() or s[j] == "_"):
            j += 1
        return s[i:j], j

    def parse_int(self, s, i):
        i = self.skip_ws(s, i)
        neg = False
        if i < len(s) and s[i] == "-":
            neg = True
            i += 1
        if i + 1 < len(s) and s[i] == "0" and s[i + 1] in "xX":
            i += 2
            if i >= len(s) or s[i] not in "0123456789abcdefABCDEF":
                return None, i
            v = 0
            while i < len(s) and s[i] in "0123456789abcdefABCDEF":
                dig = int(s[i], 16)
                v = (v << 4) | dig
                i += 1
            if v >= 0x80000000:
                v -= 0x100000000
            return (-v if neg else v), i
        if i >= len(s) or not s[i].isdigit():
            return None, i
        v = 0
        while i < len(s) and s[i].isdigit():
            v = v * 10 + int(s[i])
            i += 1
        return (-v if neg else v), i

    def parse_string(self, s, i):
        i = self.skip_ws(s, i)
        if i >= len(s) or s[i] != '"':
            return None, i
        i += 1
        j = i
        while j < len(s) and s[j] != '"':
            j += 1
        if j >= len(s):
            raise ValueError("unterminated string")
        return s[i:j], j + 1

    def emit_factor(self, s, i):
        i = self.skip_ws(s, i)
        if i < len(s) and s[i] == "(":
            i = self.emit_expr(s, i + 1)
            i = self.skip_ws(s, i)
            if i >= len(s) or s[i] != ")":
                raise ValueError("expected )")
            return i + 1
        if i < len(s) and s[i] == "[":
            i += 1
            items = []
            while True:
                i = self.skip_ws(s, i)
                if i < len(s) and s[i] == "]":
                    i += 1
                    break
                start = len(self.code)
                i = self.emit_expr(s, i)
                items.append(True)
                i = self.skip_ws(s, i)
                if i < len(s) and s[i] == ",":
                    i += 1
                    continue
                if i < len(s) and s[i] == "]":
                    i += 1
                    break
                raise ValueError("bad list")
            self.emit(GBC_LIST_NEW, len(items))
            return i
        if i < len(s) and s[i] == '"':
            text, i = self.parse_string(s, i)
            off = self.add_str(text)
            self.emit_u32(GBC_PUSH_STR, off)
            return i
        if i < len(s) and (s[i].isdigit() or (s[i] == "-" and i + 1 < len(s) and s[i + 1].isdigit())):
            v, i = self.parse_int(s, i)
            self.emit_i32(GBC_PUSH_I, v)
            return i
        name, i = self.parse_ident(s, i)
        if name is None:
            raise ValueError(f"bad factor near: {s[i:i+24]!r}")
        if name in ("map", "errmsg", "getkey", "winclosed"):
            if name == "map":
                self.emit(GBC_MAP_NEW)
            elif name == "errmsg":
                self.emit(GBC_LAST_ERR)
            elif name == "getkey":
                self.emit(GBC_KEY_POLL)
            else:
                self.emit(GBC_GUI_CLOSED)
            return i
        # unary prefix builtins
        if name in (
            "len",
            "alloc",
            "free",
            "exists",
            "read",
            "listdir",
            "dirname",
            "basename",
            "typeof",
            "dos_run",
            "str",
            "num",
        ):
            # Args are sums so trailing == / != bind outside the call.
            i = self.emit_sum(s, i)
            op = {
                "len": GBC_LEN,
                "alloc": GBC_ALLOC,
                "free": GBC_FREE,
                "exists": GBC_FS_EXISTS,
                "read": GBC_FS_READ,
                "listdir": GBC_FS_LIST,
                "dirname": GBC_PATH_DIR,
                "basename": GBC_PATH_BASE,
                "typeof": GBC_TYPEOF,
                "dos_run": GBC_DOS_RUN,
                "str": GBC_STR_I,
                "num": GBC_NUM,
            }[name]
            self.emit(op)
            return i
        if name in ("get", "find", "path_join", "push"):
            i = self.emit_sum(s, i)
            i = self.emit_sum(s, i)
            op = {
                "get": GBC_LIST_GET,
                "find": GBC_STR_FIND,
                "path_join": GBC_PATH_JOIN,
                "push": GBC_LIST_PUSH,
            }[name]
            self.emit(op)
            return i
        if name in ("slice", "set"):
            i = self.emit_sum(s, i)
            i = self.emit_sum(s, i)
            i = self.emit_sum(s, i)
            self.emit(GBC_STR_SLICE if name == "slice" else GBC_SET)
            return i
        # User fn call in expression (fn must be defined earlier): name arg…
        if name in self.fns:
            arity = self.fns[name]["arity"]
            entry = self.fns[name]["entry"]
            for _ in range(arity):
                i = self.skip_ws(s, i)
                i = self.emit_sum(s, i)
            if arity == 0:
                self.emit_u32(GBC_CALL, entry)
            else:
                self.emit(GBC_CALL_N)
                self.code.extend(struct.pack("<I", entry))
                self.emit(arity)
            return i
        # Named colors / keys (vars with the same name win).
        if self.resolve_var(name) is None:
            if name in COLORS:
                self.emit_i32(GBC_PUSH_I, COLORS[name])
                return i
            if name in KEYS:
                self.emit_i32(GBC_PUSH_I, KEYS[name])
                return i
        self.emit_load_name(name)
        return i

    def emit_term(self, s, i):
        i = self.emit_factor(s, i)
        while True:
            j = self.skip_ws(s, i)
            if j >= len(s) or s[j] not in "*/":
                return i
            op = s[j]
            i = self.emit_factor(s, j + 1)
            self.emit(GBC_MUL if op == "*" else GBC_DIV)

    def emit_sum(self, s, i):
        i = self.emit_term(s, i)
        while True:
            j = self.skip_ws(s, i)
            if j >= len(s) or s[j] not in "+-":
                return i
            op = s[j]
            i = self.emit_term(s, j + 1)
            self.emit(GBC_ADD if op == "+" else GBC_SUB)

    def emit_expr(self, s, i):
        i = self.emit_sum(s, i)
        j = self.skip_ws(s, i)
        for tok, op in (
            ("==", GBC_CMP_EQ),
            ("!=", GBC_CMP_NE),
            ("<=", GBC_CMP_LE),
            (">=", GBC_CMP_GE),
            ("<", GBC_CMP_LT),
            (">", GBC_CMP_GT),
        ):
            if s.startswith(tok, j):
                i = self.emit_sum(s, j + len(tok))
                self.emit(op)
                return i
        return i

    def starts(self, line, kw):
        if not line.startswith(kw):
            return False
        rest = line[len(kw) :]
        return not rest or not (rest[0].isalnum() or rest[0] == "_")

    def patch_breaks(self, end_ip, cont_ip):
        """Patch break/continue markers in current loop block."""
        # handled via block records
        pass

    def compile_line(self, line):
        line = line.strip()
        if not line or line.startswith("#"):
            return
        if self.starts(line, "use"):
            if "gfx3d" in line:
                self.kind = GOB_KIND_GFX3D
            elif "auto" in line:
                self.kind = GOB_KIND_AUTO
            elif "gui" in line:
                self.kind = GOB_KIND_GUI
            elif "fs" in line:
                pass
            return
        if self.starts(line, "app"):
            if "gfx3d" in line:
                self.kind = GOB_KIND_GFX3D
            elif "auto" in line:
                self.kind = GOB_KIND_AUTO
            elif "gui" in line:
                self.kind = GOB_KIND_GUI
            else:
                self.kind = GOB_KIND_CONSOLE
            return
        if self.starts(line, "var"):
            name, i = self.parse_ident(line, 3)
            if self.in_fn is not None:
                self.ensure_local(name)
            else:
                self.ensure_global(name)
            i = self.skip_ws(line, i)
            if i < len(line) and line[i] == "=":
                self.emit_expr(line, i + 1)
            else:
                self.emit_i32(GBC_PUSH_I, 0)
            self.emit_store_name(name)
            return
        if self.starts(line, "print"):
            rest = line[5:].lstrip()
            if rest.startswith('"'):
                text, _ = self.parse_string(rest, 0)
                off = self.add_str(text)
                self.emit(GBC_WRITE)
                self.code.extend(struct.pack("<II", off, len(text)))
            else:
                self.emit_expr(line, 5)
                self.emit(GBC_PRINT_I)
            return
        if self.starts(line, "if"):
            self.emit_expr(line, 2)
            jz_at = len(self.code) + 1
            self.emit_u32(GBC_JZ, 0)
            self.blocks.append(["if", jz_at, None, [], []])  # jz, else_jmp, breaks, conts
            return
        if self.starts(line, "else"):
            if not self.blocks or self.blocks[-1][0] != "if":
                raise ValueError("else without if")
            blk = self.blocks[-1]
            if blk[2] is not None:
                raise ValueError("duplicate else")
            jmp_at = len(self.code) + 1
            self.emit_u32(GBC_JMP, 0)
            # false-branch lands here
            struct.pack_into("<I", self.code, blk[1], len(self.code))
            blk[2] = jmp_at
            return
        if self.starts(line, "while"):
            loop = len(self.code)
            self.emit_expr(line, 5)
            jz_at = len(self.code) + 1
            self.emit_u32(GBC_JZ, 0)
            self.blocks.append(("while", jz_at, loop, [], []))
            return
        if self.starts(line, "for"):
            # for name = a to b
            name, i = self.parse_ident(line, 3)
            i = self.skip_ws(line, i)
            if i >= len(line) or line[i] != "=":
                raise ValueError("for needs =")
            if self.in_fn is not None:
                self.ensure_local(name)
            else:
                self.ensure_global(name)
            i = self.emit_expr(line, i + 1)
            self.emit_store_name(name)
            i = self.skip_ws(line, i)
            if not line.startswith("to", i):
                raise ValueError("for needs to")
            i += 2
            # save end expr into a temp global
            end_name = f"__for_end_{len(self.blocks)}"
            self.ensure_global(end_name)
            i = self.emit_expr(line, i)
            self.emit_u8op(GBC_STORE, self.globals[end_name])
            loop = len(self.code)
            self.emit_load_name(name)
            self.emit_u8op(GBC_LOAD, self.globals[end_name])
            self.emit(GBC_CMP_LE)
            jz_at = len(self.code) + 1
            self.emit_u32(GBC_JZ, 0)
            self.blocks.append(("for", jz_at, loop, name, end_name, [], []))
            return
        if self.starts(line, "fn"):
            name, i = self.parse_ident(line, 2)
            args = []
            while True:
                arg, i = self.parse_ident(line, i)
                if arg is None:
                    break
                args.append(arg)
            jmp_at = len(self.code) + 1
            self.emit_u32(GBC_JMP, 0)
            entry = len(self.code)
            self.fns[name] = {"entry": entry, "arity": len(args), "args": args}
            for patch, n, arity in self.pend:
                if n == name:
                    struct.pack_into("<I", self.code, patch, entry)
                    if arity is not None:
                        pass
            self.locals = {a: idx for idx, a in enumerate(args)}
            self.in_fn = self.fns[name]
            self.blocks.append(("fn", jmp_at, name))
            return
        if self.starts(line, "end"):
            if not self.blocks:
                raise ValueError("end without block")
            blk = self.blocks.pop()
            kind = blk[0]
            if kind == "if":
                _, jz_at, else_jmp, breaks, conts = blk
                end = len(self.code)
                if else_jmp is None:
                    struct.pack_into("<I", self.code, jz_at, end)
                else:
                    struct.pack_into("<I", self.code, else_jmp, end)
                for p in breaks:
                    struct.pack_into("<I", self.code, p, end)
            elif kind == "while":
                _, jz_at, loop, breaks, conts = blk
                cont = len(self.code)
                for p in conts:
                    struct.pack_into("<I", self.code, p, loop)
                self.emit_u32(GBC_JMP, loop)
                end = len(self.code)
                struct.pack_into("<I", self.code, jz_at, end)
                for p in breaks:
                    struct.pack_into("<I", self.code, p, end)
            elif kind == "for":
                _, jz_at, loop, name, end_name, breaks, conts = blk
                cont = len(self.code)
                for p in conts:
                    struct.pack_into("<I", self.code, p, cont)
                self.emit_load_name(name)
                self.emit_i32(GBC_PUSH_I, 1)
                self.emit(GBC_ADD)
                self.emit_store_name(name)
                self.emit_u32(GBC_JMP, loop)
                end = len(self.code)
                struct.pack_into("<I", self.code, jz_at, end)
                for p in breaks:
                    struct.pack_into("<I", self.code, p, end)
            elif kind == "fn":
                _, jmp_at, _name = blk
                self.emit(GBC_RET)
                struct.pack_into("<I", self.code, jmp_at, len(self.code))
                self.in_fn = None
                self.locals = {}
            return
        if self.starts(line, "break"):
            for blk in reversed(self.blocks):
                if blk[0] in ("while", "for"):
                    patch = len(self.code) + 1
                    self.emit_u32(GBC_JMP, 0)
                    if blk[0] == "while":
                        blk[3].append(patch)
                    else:
                        blk[5].append(patch)
                    return
            raise ValueError("break outside loop")
        if self.starts(line, "continue"):
            for blk in reversed(self.blocks):
                if blk[0] == "while":
                    patch = len(self.code) + 1
                    self.emit_u32(GBC_JMP, 0)
                    blk[4].append(patch)
                    return
                if blk[0] == "for":
                    patch = len(self.code) + 1
                    self.emit_u32(GBC_JMP, 0)
                    blk[6].append(patch)
                    return
            raise ValueError("continue outside loop")
        if self.starts(line, "call"):
            name, i = self.parse_ident(line, 4)
            args = []
            while True:
                i = self.skip_ws(line, i)
                if i >= len(line):
                    break
                before = len(self.code)
                try:
                    i = self.emit_expr(line, i)
                except ValueError:
                    break
                if len(self.code) == before:
                    break
                args.append(True)
            arity = len(args)
            if name in self.fns:
                entry = self.fns[name]["entry"]
                if arity == 0:
                    self.emit_u32(GBC_CALL, entry)
                else:
                    self.emit(GBC_CALL_N)
                    self.code.extend(struct.pack("<I", entry))
                    self.emit(arity)
            else:
                patch = len(self.code) + 1
                self.pend.append((patch, name, arity))
                if arity == 0:
                    self.emit_u32(GBC_CALL, 0)
                else:
                    self.emit(GBC_CALL_N)
                    self.code.extend(struct.pack("<I", 0))
                    self.emit(arity)
            return
        if self.starts(line, "return"):
            rest = line[6:].strip()
            if rest:
                self.emit_expr(line, 6)
                self.emit(GBC_RET_V)
            else:
                self.emit(GBC_RET)
            return
        if self.starts(line, "write"):
            # write path data
            i = self.emit_expr(line, 5)
            i = self.emit_expr(line, i)
            self.emit(GBC_FS_WRITE)
            self.emit(GBC_POP)
            return
        if self.starts(line, "push"):
            i = self.emit_expr(line, 4)
            i = self.emit_expr(line, i)
            self.emit(GBC_LIST_PUSH)
            self.emit(GBC_POP)
            return
        if self.starts(line, "set"):
            # set container key/idx value
            i = self.emit_expr(line, 3)
            i = self.emit_expr(line, i)
            i = self.emit_expr(line, i)
            self.emit(GBC_SET)
            self.emit(GBC_POP)
            return
        if self.starts(line, "dos_run"):
            self.emit_expr(line, 7)
            self.emit(GBC_DOS_RUN)
            self.emit(GBC_POP)
            return
        if self.starts(line, "window"):
            parts = line.split('"')
            if len(parts) >= 3:
                title = parts[1]
                rest = parts[2].strip()
                w, h = 420, 240
                if "x" in rest.lower():
                    a, b = rest.lower().split("x", 1)
                    try:
                        w, h = int(a.strip()), int(b.strip().split()[0])
                    except ValueError:
                        pass
                off = self.add_str(title)
                self.emit(GBC_GUI_CREATE)
                self.code.extend(struct.pack("<IHH", off, w, h))
                self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "text"):
            rest = line[4:].strip()
            if rest.startswith('"'):
                parts = line.split('"')
                if len(parts) >= 3:
                    off = self.add_str(parts[1])
                    self.emit(GBC_GUI_TEXT)
                    self.code.extend(struct.pack("<IHHI", 0, 0, 0, off))
            else:
                self.emit_expr(rest, 0)
                self.emit(GBC_GUI_TEXT_S)
            return
        if self.starts(line, "cleargui"):
            self.emit(GBC_GUI_CLEAR)
            self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "fill"):
            self.emit_expr(line, 4)
            self.emit(GBC_GFX_FILL)
            self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "rect"):
            i = self.emit_expr(line, 4)
            i = self.emit_expr(line, i)
            i = self.emit_expr(line, i)
            i = self.emit_expr(line, i)
            self.emit_expr(line, i)
            self.emit(GBC_GFX_RECT)
            self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "label"):
            # label x y str fg bg  → push order; runtime pops bg,fg,str,y,x
            i = self.emit_expr(line, 5)
            i = self.emit_expr(line, i)
            i = self.emit_expr(line, i)
            i = self.emit_expr(line, i)
            self.emit_expr(line, i)
            self.emit(GBC_GFX_LABEL)
            self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "present"):
            self.emit(GBC_GFX_PRESENT)
            self.kind = GOB_KIND_GUI
            return
        if self.starts(line, "wait"):
            self.emit_u32(GBC_GUI_WAIT, 0)
            return
        if self.starts(line, "sleep"):
            parts = line.split()
            ms = int(parts[1]) if len(parts) > 1 else 0
            self.emit_u32(GBC_SLEEP_MS, ms)
            if self.kind == GOB_KIND_CONSOLE:
                self.kind = GOB_KIND_AUTO
            return
        if self.starts(line, "clear"):
            parts = line.split()
            rgba = int(parts[1], 16) if len(parts) > 1 else 0
            self.emit_u32(GBC_GFX3D_CLEAR, rgba)
            self.kind = GOB_KIND_GFX3D
            return
        if self.starts(line, "exit"):
            self.emit_u32(GBC_EXIT, 0)
            self.has_exit = True
            return
        name, i = self.parse_ident(line, 0)
        if name:
            i = self.skip_ws(line, i)
            if i < len(line) and line[i] == "=":
                if self.resolve_var(name) is None:
                    if self.in_fn is not None:
                        self.ensure_local(name)
                    else:
                        self.ensure_global(name)
                self.emit_expr(line, i + 1)
                self.emit_store_name(name)
                return
            if name in self.fns:
                arity = self.fns[name]["arity"]
                entry = self.fns[name]["entry"]
                for _ in range(arity):
                    i = self.skip_ws(line, i)
                    i = self.emit_expr(line, i)
                if arity == 0:
                    self.emit_u32(GBC_CALL, entry)
                else:
                    self.emit(GBC_CALL_N)
                    self.code.extend(struct.pack("<I", entry))
                    self.emit(arity)
                return
        raise ValueError(f"bad line: {line}")

    def compile(self, src: str) -> bytes:
        for lineno, raw in enumerate(src.splitlines(), 1):
            try:
                self.compile_line(raw)
            except ValueError as e:
                raise ValueError(f"line {lineno}: {e}") from e
        if self.blocks:
            kinds = ", ".join(b[0] for b in self.blocks)
            raise ValueError(f"unclosed block(s): {kinds}")
        for patch, name, arity in self.pend:
            if name not in self.fns:
                raise ValueError(f"undefined fn '{name}' (called but never defined)")
            struct.pack_into("<I", self.code, patch, self.fns[name]["entry"])
        if not self.has_exit:
            self.emit_u32(GBC_EXIT, 0)
        hdr = struct.pack(
            "<IHBBIIIII",
            GOB_MAGIC,
            GOB_VERSION,
            GOB_ARCH_X86_64,
            self.kind,
            GOB_FLAG_BYTECODE,
            0,
            len(self.code),
            len(self.rodata),
            0,
        )
        return hdr + bytes(self.code) + bytes(self.rodata)


def main():
    ap = argparse.ArgumentParser(description="GooberC host compiler v2")
    ap.add_argument("src")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    src = open(args.src, "r", encoding="utf-8").read()
    blob = Compiler().compile(src)
    open(args.output, "wb").write(blob)
    print(f"[+] wrote {args.output} ({len(blob)} bytes)", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"gooberc: {e}", file=sys.stderr)
        sys.exit(1)
