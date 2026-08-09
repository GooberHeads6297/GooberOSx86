#include "userspace.h"
#include "syscall.h"
#include "../fs/filesystem.h"
#include "../lib/memory.h"
#include "../lib/string.h"

extern void print(const char*);

/*
 * GooberC compiler (v2): line-oriented -> bytecode .gob
 * Mirrors gooberc/gooberc.py — easy syntax, not C.
 */

#define GC_NAME_MAX   24
#define GC_MAX_GLOBALS 128
#define GC_MAX_LOCALS  32
#define GC_MAX_FNS    24
#define GC_MAX_BLOCKS 32
#define GC_MAX_PEND   32
#define GC_MAX_PATCH  16

typedef struct {
    char name[GC_NAME_MAX];
    uint8_t slot;
} gc_var_t;

typedef struct {
    char name[GC_NAME_MAX];
    uint32_t entry;
    uint8_t arity;
    int defined;
} gc_fn_t;

typedef struct {
    int kind; /* 0=if 1=while 2=for 3=fn */
    uint32_t jz_at;
    uint32_t else_jmp; /* if/else: JMP over else body; 0 = no else */
    uint32_t loop_start;
    uint32_t jmp_over_at;
    char for_var[GC_NAME_MAX];
    uint8_t for_end_slot;
    uint32_t breaks[GC_MAX_PATCH];
    int nbreak;
    uint32_t conts[GC_MAX_PATCH];
    int ncont;
} gc_block_t;

typedef struct {
    char name[GC_NAME_MAX];
    uint32_t patch_at;
    uint8_t arity;
} gc_pend_call_t;

static uint8_t* g_code;
static uint32_t g_code_cap;
static uint32_t g_code_len;
static uint8_t* g_ro;
static uint32_t g_ro_cap;
static uint32_t g_ro_len;

static gc_var_t g_globals[GC_MAX_GLOBALS];
static int g_nglobal;
static gc_var_t g_locals[GC_MAX_LOCALS];
static int g_nlocal;
static int g_in_fn;
static gc_fn_t g_fns[GC_MAX_FNS];
static int g_nfn;
static gc_block_t g_blocks[GC_MAX_BLOCKS];
static int g_nblock;
static gc_pend_call_t g_pend[GC_MAX_PEND];
static int g_npend;

static const char* gc_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static int emit_byte(uint8_t b) {
    if (g_code_len + 1 > g_code_cap) return -1;
    g_code[g_code_len++] = b;
    return 0;
}

static int emit_op32(uint8_t op, uint32_t v) {
    if (g_code_len + 5 > g_code_cap) return -1;
    g_code[g_code_len++] = op;
    wr32(g_code + g_code_len, v);
    g_code_len += 4;
    return 0;
}

static int emit_op_i32(uint8_t op, int32_t v) {
    return emit_op32(op, (uint32_t)v);
}

static int emit_op_u8(uint8_t op, uint8_t v) {
    if (g_code_len + 2 > g_code_cap) return -1;
    g_code[g_code_len++] = op;
    g_code[g_code_len++] = v;
    return 0;
}

static int emit_call(uint32_t entry, uint8_t arity) {
    if (arity == 0)
        return emit_op32(GBC_CALL, entry);
    if (g_code_len + 6 > g_code_cap) return -1;
    g_code[g_code_len++] = GBC_CALL_N;
    wr32(g_code + g_code_len, entry);
    g_code_len += 4;
    g_code[g_code_len++] = arity;
    return 0;
}

static int line_has(const char* line, const char* needle) {
    size_t nlen = 0;
    const char* q;
    while (needle[nlen]) nlen++;
    for (q = line; *q && *q != '\n'; q++) {
        size_t i = 0;
        while (i < nlen && q[i] == needle[i]) i++;
        if (i == nlen) return 1;
    }
    return 0;
}

static int gc_is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int gc_is_ident(char c) {
    return gc_is_ident_start(c) || (c >= '0' && c <= '9');
}

static int gc_parse_ident(const char** pp, char* out, int out_sz) {
    const char* p = gc_skip_ws(*pp);
    int n = 0;
    if (!gc_is_ident_start(*p)) return -1;
    while (gc_is_ident(*p) && n + 1 < out_sz) out[n++] = *p++;
    out[n] = '\0';
    *pp = p;
    return 0;
}

static int gc_parse_i32(const char** pp, int32_t* out) {
    const char* p = gc_skip_ws(*pp);
    int neg = 0;
    uint32_t v = 0;
    if (*p == '-') { neg = 1; p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return -1;
        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            uint32_t dig;
            if (*p <= '9') dig = (uint32_t)(*p - '0');
            else if (*p <= 'F') dig = (uint32_t)(*p - 'A') + 10u;
            else dig = (uint32_t)(*p - 'a') + 10u;
            v = (v << 4) | dig;
            p++;
        }
        *out = neg ? -(int32_t)v : (int32_t)v;
        *pp = p;
        return 0;
    }
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    *out = neg ? -(int32_t)v : (int32_t)v;
    *pp = p;
    return 0;
}

static int gc_find_global(const char* name) {
    int i;
    for (i = 0; i < g_nglobal; i++)
        if (strcmp(g_globals[i].name, name) == 0) return i;
    return -1;
}

static int gc_ensure_global(const char* name) {
    int i = gc_find_global(name);
    if (i >= 0) return i;
    if (g_nglobal >= GC_MAX_GLOBALS) return -1;
    strncpy(g_globals[g_nglobal].name, name, GC_NAME_MAX - 1);
    g_globals[g_nglobal].name[GC_NAME_MAX - 1] = '\0';
    g_globals[g_nglobal].slot = (uint8_t)g_nglobal;
    g_nglobal++;
    return g_nglobal - 1;
}

static int gc_find_local(const char* name) {
    int i;
    for (i = 0; i < g_nlocal; i++)
        if (strcmp(g_locals[i].name, name) == 0) return i;
    return -1;
}

static int gc_ensure_local(const char* name) {
    int i = gc_find_local(name);
    if (i >= 0) return i;
    if (g_nlocal >= GC_MAX_LOCALS) return -1;
    strncpy(g_locals[g_nlocal].name, name, GC_NAME_MAX - 1);
    g_locals[g_nlocal].name[GC_NAME_MAX - 1] = '\0';
    g_locals[g_nlocal].slot = (uint8_t)g_nlocal;
    g_nlocal++;
    return g_nlocal - 1;
}

static int gc_resolve_var(const char* name, int* is_local, uint8_t* slot) {
    if (g_in_fn) {
        int li = gc_find_local(name);
        if (li >= 0) {
            *is_local = 1;
            *slot = g_locals[li].slot;
            return 0;
        }
    }
    {
        int gi = gc_find_global(name);
        if (gi >= 0) {
            *is_local = 0;
            *slot = g_globals[gi].slot;
            return 0;
        }
    }
    return -1;
}

/* Named RGB (0xRRGGBB) and key codes — mirrors gooberc/gooberc.py. */
static int gc_named_const(const char* name, int32_t* out) {
    static const struct { const char* n; int32_t v; } tab[] = {
        {"BLACK", 0x000000}, {"WHITE", 0xFFFFFF},
        {"GRAY", 0x808080}, {"GREY", 0x808080},
        {"SILVER", 0xC0C0C0},
        {"LIGHTGRAY", 0xD3D3D3}, {"LIGHTGREY", 0xD3D3D3},
        {"DARKGRAY", 0x404040}, {"DARKGREY", 0x404040},
        {"RED", 0xE74C3C}, {"DARKRED", 0x8B0000},
        {"GREEN", 0x2ECC71}, {"DARKGREEN", 0x196F3D}, {"LIME", 0x4ADE80},
        {"BLUE", 0x3498DB}, {"DARKBLUE", 0x1A5276}, {"NAVY", 0x1B2838},
        {"SKY", 0x87CEEB}, {"CYAN", 0x1ABC9C}, {"TEAL", 0x148F77},
        {"AQUA", 0x00FFFF},
        {"YELLOW", 0xF1C40F}, {"GOLD", 0xFFD700}, {"ORANGE", 0xE67E22},
        {"BROWN", 0x8B4513},
        {"PURPLE", 0x9B59B6}, {"INDIGO", 0x6D28D9}, {"VIOLET", 0x8E44AD},
        {"MAGENTA", 0xFF00FF}, {"PINK", 0xFF69B4}, {"CORAL", 0xFF6B6B},
        {"MAROON", 0x800000}, {"OLIVE", 0x808000},
        {"PANEL", 0x243447}, {"INK", 0xEEF2F7}, {"MUTED", 0x9FB3C8},
        {"TRANSPARENT", 0x000000},
        {"KEY_ESC", 27}, {"KEY_ENTER", 13}, {"KEY_SPACE", 32},
        {"KEY_UP", 128}, {"KEY_DOWN", 129}, {"KEY_LEFT", 130}, {"KEY_RIGHT", 131},
    };
    size_t i;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(name, tab[i].n) == 0) {
            *out = tab[i].v;
            return 0;
        }
    }
    return -1;
}

static int gc_emit_load_name(const char* name) {
    int is_local;
    uint8_t slot;
    if (gc_resolve_var(name, &is_local, &slot) != 0) return -1;
    return emit_op_u8(is_local ? GBC_LOAD_LOCAL : GBC_LOAD, slot);
}

static int gc_emit_store_name(const char* name) {
    int is_local = 0;
    uint8_t slot = 0;
    if (gc_resolve_var(name, &is_local, &slot) == 0)
        return emit_op_u8(is_local ? GBC_STORE_LOCAL : GBC_STORE, slot);
    if (g_in_fn) {
        int li = gc_ensure_local(name);
        if (li < 0) return -1;
        return emit_op_u8(GBC_STORE_LOCAL, g_locals[li].slot);
    }
    {
        int gi = gc_ensure_global(name);
        if (gi < 0) return -1;
        return emit_op_u8(GBC_STORE, g_globals[gi].slot);
    }
}

static int gc_find_fn(const char* name) {
    int i;
    for (i = 0; i < g_nfn; i++)
        if (strcmp(g_fns[i].name, name) == 0) return i;
    return -1;
}

static int gc_add_fn(const char* name) {
    int i = gc_find_fn(name);
    if (i >= 0) return i;
    if (g_nfn >= GC_MAX_FNS) return -1;
    strncpy(g_fns[g_nfn].name, name, GC_NAME_MAX - 1);
    g_fns[g_nfn].name[GC_NAME_MAX - 1] = '\0';
    g_fns[g_nfn].entry = 0;
    g_fns[g_nfn].arity = 0;
    g_fns[g_nfn].defined = 0;
    g_nfn++;
    return g_nfn - 1;
}

static int gc_add_str_ro(const char* s, size_t len, uint32_t* off) {
    if (g_ro_len + len + 1 > g_ro_cap) return -1;
    *off = g_ro_len;
    memcpy(g_ro + g_ro_len, s, len);
    g_ro[g_ro_len + len] = '\0';
    g_ro_len += (uint32_t)len + 1;
    return 0;
}

static int gc_block_add_break(gc_block_t* b) {
    if (b->nbreak >= GC_MAX_PATCH) return -1;
    b->breaks[b->nbreak++] = g_code_len + 1;
    return emit_op32(GBC_JMP, 0);
}

static int gc_block_add_cont(gc_block_t* b) {
    if (b->ncont >= GC_MAX_PATCH) return -1;
    b->conts[b->ncont++] = g_code_len + 1;
    return emit_op32(GBC_JMP, 0);
}

static void gc_patch_breaks(gc_block_t* b, uint32_t target) {
    int i;
    for (i = 0; i < b->nbreak; i++)
        wr32(g_code + b->breaks[i], target);
}

static int emit_expr(const char** pp);
static int emit_sum(const char** pp);

static int emit_factor(const char** pp) {
    const char* p = gc_skip_ws(*pp);
    char name[GC_NAME_MAX];
    int32_t lit;
    uint32_t off;
    int item_count;

    if (*p == '(') {
        p++;
        *pp = p;
        if (emit_expr(pp) != 0) return -1;
        p = gc_skip_ws(*pp);
        if (*p != ')') return -1;
        *pp = p + 1;
        return 0;
    }
    if (*p == '[') {
        p++;
        item_count = 0;
        for (;;) {
            p = gc_skip_ws(p);
            if (*p == ']') {
                p++;
                break;
            }
            *pp = p;
            if (emit_expr(pp) != 0) return -1;
            item_count++;
            p = gc_skip_ws(*pp);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == ']') {
                p++;
                break;
            }
            return -1;
        }
        *pp = p;
        if (item_count > 255) return -1;
        if (emit_byte(GBC_LIST_NEW) != 0) return -1;
        return emit_byte((uint8_t)item_count);
    }
    if (*p == '"') {
        char lit[256];
        size_t li = 0;
        p++;
        while (*p && *p != '"' && li + 1 < sizeof(lit)) lit[li++] = *p++;
        if (*p != '"') return -1;
        lit[li] = '\0';
        p++;
        if (gc_add_str_ro(lit, li, &off) != 0) return -1;
        *pp = p;
        return emit_op32(GBC_PUSH_STR, off);
    }
    if ((*p >= '0' && *p <= '9') || (*p == '-' && p[1] >= '0' && p[1] <= '9')) {
        if (gc_parse_i32(&p, &lit) != 0) return -1;
        *pp = p;
        return emit_op_i32(GBC_PUSH_I, lit);
    }
    if (gc_parse_ident(&p, name, sizeof(name)) != 0) return -1;
    if (strcmp(name, "map") == 0) {
        *pp = p;
        return emit_byte(GBC_MAP_NEW);
    }
    if (strcmp(name, "errmsg") == 0) {
        *pp = p;
        return emit_byte(GBC_LAST_ERR);
    }
    if (strcmp(name, "getkey") == 0) {
        *pp = p;
        return emit_byte(GBC_KEY_POLL);
    }
    if (strcmp(name, "winclosed") == 0) {
        *pp = p;
        return emit_byte(GBC_GUI_CLOSED);
    }
    if (strcmp(name, "len") == 0 || strcmp(name, "alloc") == 0 ||
        strcmp(name, "free") == 0 || strcmp(name, "exists") == 0 ||
        strcmp(name, "read") == 0 || strcmp(name, "listdir") == 0 ||
        strcmp(name, "dirname") == 0 || strcmp(name, "basename") == 0 ||
        strcmp(name, "typeof") == 0 || strcmp(name, "dos_run") == 0 ||
        strcmp(name, "str") == 0 || strcmp(name, "num") == 0) {
        uint8_t op;
        *pp = p;
        /* Args are sums so trailing == / != bind outside the call. */
        if (emit_sum(pp) != 0) return -1;
        if (strcmp(name, "len") == 0) op = GBC_LEN;
        else if (strcmp(name, "alloc") == 0) op = GBC_ALLOC;
        else if (strcmp(name, "free") == 0) op = GBC_FREE;
        else if (strcmp(name, "exists") == 0) op = GBC_FS_EXISTS;
        else if (strcmp(name, "read") == 0) op = GBC_FS_READ;
        else if (strcmp(name, "listdir") == 0) op = GBC_FS_LIST;
        else if (strcmp(name, "dirname") == 0) op = GBC_PATH_DIR;
        else if (strcmp(name, "basename") == 0) op = GBC_PATH_BASE;
        else if (strcmp(name, "dos_run") == 0) op = GBC_DOS_RUN;
        else if (strcmp(name, "str") == 0) op = GBC_STR_I;
        else if (strcmp(name, "num") == 0) op = GBC_NUM;
        else op = GBC_TYPEOF;
        return emit_byte(op);
    }
    if (strcmp(name, "get") == 0 || strcmp(name, "find") == 0 ||
        strcmp(name, "path_join") == 0 || strcmp(name, "push") == 0) {
        uint8_t op;
        *pp = p;
        if (emit_sum(pp) != 0) return -1;
        if (emit_sum(pp) != 0) return -1;
        if (strcmp(name, "get") == 0) op = GBC_LIST_GET;
        else if (strcmp(name, "find") == 0) op = GBC_STR_FIND;
        else if (strcmp(name, "path_join") == 0) op = GBC_PATH_JOIN;
        else op = GBC_LIST_PUSH;
        return emit_byte(op);
    }
    if (strcmp(name, "slice") == 0 || strcmp(name, "set") == 0) {
        *pp = p;
        if (emit_sum(pp) != 0) return -1;
        if (emit_sum(pp) != 0) return -1;
        if (emit_sum(pp) != 0) return -1;
        return emit_byte(strcmp(name, "slice") == 0 ? GBC_STR_SLICE : GBC_SET);
    }
    {
        int fi = gc_find_fn(name);
        if (fi >= 0 && g_fns[fi].defined) {
            uint8_t a;
            *pp = p;
            for (a = 0; a < g_fns[fi].arity; a++) {
                if (emit_sum(pp) != 0) return -1;
            }
            return emit_call(g_fns[fi].entry, g_fns[fi].arity);
        }
    }
    {
        int is_local = 0;
        uint8_t slot = 0;
        int32_t cv = 0;
        if (gc_resolve_var(name, &is_local, &slot) != 0 &&
            gc_named_const(name, &cv) == 0) {
            *pp = p;
            return emit_op_i32(GBC_PUSH_I, cv);
        }
    }
    *pp = p;
    return gc_emit_load_name(name);
}

static int emit_term(const char** pp) {
    char op;
    if (emit_factor(pp) != 0) return -1;
    for (;;) {
        const char* p = gc_skip_ws(*pp);
        if (*p != '*' && *p != '/') break;
        op = *p++;
        *pp = p;
        if (emit_factor(pp) != 0) return -1;
        if (emit_byte(op == '*' ? GBC_MUL : GBC_DIV) != 0) return -1;
    }
    return 0;
}

static int emit_sum(const char** pp) {
    char op;
    if (emit_term(pp) != 0) return -1;
    for (;;) {
        const char* p = gc_skip_ws(*pp);
        if (*p != '+' && *p != '-') break;
        op = *p++;
        *pp = p;
        if (emit_term(pp) != 0) return -1;
        if (emit_byte(op == '+' ? GBC_ADD : GBC_SUB) != 0) return -1;
    }
    return 0;
}

static int emit_expr(const char** pp) {
    const char* p;
    uint8_t cmp;

    if (emit_sum(pp) != 0) return -1;
    p = gc_skip_ws(*pp);
    if (p[0] == '=' && p[1] == '=') { cmp = GBC_CMP_EQ; p += 2; }
    else if (p[0] == '!' && p[1] == '=') { cmp = GBC_CMP_NE; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { cmp = GBC_CMP_LE; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { cmp = GBC_CMP_GE; p += 2; }
    else if (p[0] == '<') { cmp = GBC_CMP_LT; p += 1; }
    else if (p[0] == '>') { cmp = GBC_CMP_GT; p += 1; }
    else return 0;
    *pp = p;
    if (emit_sum(pp) != 0) return -1;
    return emit_byte(cmp);
}

static int gc_parse_u16(const char* p, uint16_t* out) {
    uint32_t v = 0;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 65535u) return -1;
        p++;
    }
    *out = (uint16_t)v;
    return 0;
}

static int starts_with(const char* line, const char* kw) {
    int i = 0;
    while (kw[i]) {
        if (line[i] != kw[i]) return 0;
        i++;
    }
    if (gc_is_ident(line[i])) return 0;
    return 1;
}

static int gc_starts_to(const char* line, const char* p) {
    if (p[0] == 't' && p[1] == 'o' &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t'))
        return 1;
    return 0;
}

static int gc_compile_line(const char* line, int* kind_out, int* has_exit) {
    char lit[128];
    char name[GC_NAME_MAX];
    char end_name[GC_NAME_MAX];
    const char* q;
    size_t li;
    int i;

    if (starts_with(line, "use")) {
        if (line_has(line, "gfx3d")) *kind_out = GOB_KIND_GFX3D;
        else if (line_has(line, "auto")) *kind_out = GOB_KIND_AUTO;
        else if (line_has(line, "gui")) *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "app")) {
        if (line_has(line, "gfx3d")) *kind_out = GOB_KIND_GFX3D;
        else if (line_has(line, "auto")) *kind_out = GOB_KIND_AUTO;
        else if (line_has(line, "gui")) *kind_out = GOB_KIND_GUI;
        else *kind_out = GOB_KIND_CONSOLE;
    } else if (starts_with(line, "var")) {
        q = line + 3;
        if (gc_parse_ident(&q, name, sizeof(name)) != 0) return -1;
        if (g_in_fn) {
            if (gc_ensure_local(name) < 0) return -1;
        } else {
            if (gc_ensure_global(name) < 0) return -1;
        }
        q = gc_skip_ws(q);
        if (*q == '=') {
            q++;
            if (emit_expr(&q) != 0) return -1;
        } else {
            if (emit_op_i32(GBC_PUSH_I, 0) != 0) return -1;
        }
        if (gc_emit_store_name(name) != 0) return -1;
    } else if (starts_with(line, "print")) {
        q = gc_skip_ws(line + 5);
        if (*q == '"') {
            q++;
            li = 0;
            while (*q && *q != '"' && li + 1 < sizeof(lit)) lit[li++] = *q++;
            lit[li] = '\0';
            if (g_ro_len + li + 1 < g_ro_cap && g_code_len + 9 < g_code_cap) {
                uint32_t off = g_ro_len;
                memcpy(g_ro + g_ro_len, lit, li + 1);
                g_ro_len += (uint32_t)li + 1;
                g_code[g_code_len++] = GBC_WRITE;
                wr32(g_code + g_code_len, off); g_code_len += 4;
                wr32(g_code + g_code_len, (uint32_t)li); g_code_len += 4;
            } else return -1;
        } else {
            if (emit_expr(&q) != 0) return -1;
            if (emit_byte(GBC_PRINT_I) != 0) return -1;
        }
    } else if (starts_with(line, "if")) {
        q = line + 2;
        if (g_nblock >= GC_MAX_BLOCKS) return -1;
        if (emit_expr(&q) != 0) return -1;
        g_blocks[g_nblock].kind = 0;
        g_blocks[g_nblock].jz_at = g_code_len + 1;
        g_blocks[g_nblock].else_jmp = 0;
        g_blocks[g_nblock].nbreak = 0;
        g_blocks[g_nblock].ncont = 0;
        if (emit_op32(GBC_JZ, 0) != 0) return -1;
        g_nblock++;
    } else if (starts_with(line, "else")) {
        gc_block_t* b;
        if (g_nblock <= 0) return -1;
        b = &g_blocks[g_nblock - 1];
        if (b->kind != 0 || b->else_jmp) return -1;
        b->else_jmp = g_code_len + 1;
        if (emit_op32(GBC_JMP, 0) != 0) return -1;
        wr32(g_code + b->jz_at, g_code_len);
    } else if (starts_with(line, "while")) {
        q = line + 5;
        if (g_nblock >= GC_MAX_BLOCKS) return -1;
        g_blocks[g_nblock].kind = 1;
        g_blocks[g_nblock].loop_start = g_code_len;
        g_blocks[g_nblock].nbreak = 0;
        g_blocks[g_nblock].ncont = 0;
        if (emit_expr(&q) != 0) return -1;
        g_blocks[g_nblock].jz_at = g_code_len + 1;
        if (emit_op32(GBC_JZ, 0) != 0) return -1;
        g_nblock++;
    } else if (starts_with(line, "for")) {
        char num[12];
        q = line + 3;
        if (g_nblock >= GC_MAX_BLOCKS) return -1;
        if (gc_parse_ident(&q, name, sizeof(name)) != 0) return -1;
        q = gc_skip_ws(q);
        if (*q != '=') return -1;
        q++;
        if (g_in_fn) {
            if (gc_ensure_local(name) < 0) return -1;
        } else {
            if (gc_ensure_global(name) < 0) return -1;
        }
        if (emit_expr(&q) != 0) return -1;
        if (gc_emit_store_name(name) != 0) return -1;
        q = gc_skip_ws(q);
        if (!gc_starts_to(line, q)) return -1;
        q += 2;
        strcpy(end_name, "__for_end_");
        itoa(g_nblock, num, 10);
        strcat(end_name, num);
        {
            int end_gi = gc_ensure_global(end_name);
            if (end_gi < 0) return -1;
            if (emit_expr(&q) != 0) return -1;
            if (emit_op_u8(GBC_STORE, g_globals[end_gi].slot) != 0) return -1;
            g_blocks[g_nblock].kind = 2;
            g_blocks[g_nblock].loop_start = g_code_len;
            g_blocks[g_nblock].nbreak = 0;
            g_blocks[g_nblock].ncont = 0;
            strncpy(g_blocks[g_nblock].for_var, name, GC_NAME_MAX - 1);
            g_blocks[g_nblock].for_var[GC_NAME_MAX - 1] = '\0';
            g_blocks[g_nblock].for_end_slot = g_globals[end_gi].slot;
            if (gc_emit_load_name(name) != 0) return -1;
            if (emit_op_u8(GBC_LOAD, g_globals[end_gi].slot) != 0) return -1;
            if (emit_byte(GBC_CMP_LE) != 0) return -1;
            g_blocks[g_nblock].jz_at = g_code_len + 1;
            if (emit_op32(GBC_JZ, 0) != 0) return -1;
            g_nblock++;
        }
    } else if (starts_with(line, "fn")) {
        int fi;
        uint8_t argc = 0;
        char fn_name[GC_NAME_MAX];
        char arg_name[GC_NAME_MAX];
        q = line + 2;
        if (gc_parse_ident(&q, fn_name, sizeof(fn_name)) != 0) return -1;
        fi = gc_add_fn(fn_name);
        if (fi < 0 || g_nblock >= GC_MAX_BLOCKS) return -1;
        g_nlocal = 0;
        while (gc_parse_ident(&q, arg_name, sizeof(arg_name)) == 0) {
            if (gc_ensure_local(arg_name) < 0) return -1;
            argc++;
        }
        g_blocks[g_nblock].kind = 3;
        g_blocks[g_nblock].jmp_over_at = g_code_len + 1;
        if (emit_op32(GBC_JMP, 0) != 0) return -1;
        g_fns[fi].entry = g_code_len;
        g_fns[fi].defined = 1;
        g_fns[fi].arity = argc;
        for (i = 0; i < g_npend; i++) {
            if (strcmp(g_pend[i].name, g_fns[fi].name) == 0)
                wr32(g_code + g_pend[i].patch_at, g_fns[fi].entry);
        }
        g_in_fn = 1;
        g_nblock++;
    } else if (starts_with(line, "end")) {
        gc_block_t* b;
        if (g_nblock <= 0) return -1;
        g_nblock--;
        b = &g_blocks[g_nblock];
        if (b->kind == 0) {
            if (b->else_jmp)
                wr32(g_code + b->else_jmp, g_code_len);
            else
                wr32(g_code + b->jz_at, g_code_len);
            gc_patch_breaks(b, g_code_len);
        } else if (b->kind == 1) {
            int j;
            for (j = 0; j < b->ncont; j++)
                wr32(g_code + b->conts[j], b->loop_start);
            if (emit_op32(GBC_JMP, b->loop_start) != 0) return -1;
            wr32(g_code + b->jz_at, g_code_len);
            gc_patch_breaks(b, g_code_len);
        } else if (b->kind == 2) {
            uint32_t cont_ip;
            int j;
            cont_ip = g_code_len;
            for (j = 0; j < b->ncont; j++)
                wr32(g_code + b->conts[j], cont_ip);
            if (gc_emit_load_name(b->for_var) != 0) return -1;
            if (emit_op_i32(GBC_PUSH_I, 1) != 0) return -1;
            if (emit_byte(GBC_ADD) != 0) return -1;
            if (gc_emit_store_name(b->for_var) != 0) return -1;
            if (emit_op32(GBC_JMP, b->loop_start) != 0) return -1;
            wr32(g_code + b->jz_at, g_code_len);
            gc_patch_breaks(b, g_code_len);
        } else if (b->kind == 3) {
            if (emit_byte(GBC_RET) != 0) return -1;
            wr32(g_code + b->jmp_over_at, g_code_len);
            g_in_fn = 0;
            g_nlocal = 0;
        }
    } else if (starts_with(line, "break")) {
        int j;
        for (j = g_nblock - 1; j >= 0; j--) {
            if (g_blocks[j].kind == 1 || g_blocks[j].kind == 2) {
                if (gc_block_add_break(&g_blocks[j]) != 0) return -1;
                return 0;
            }
        }
        return -1;
    } else if (starts_with(line, "continue")) {
        int j;
        for (j = g_nblock - 1; j >= 0; j--) {
            if (g_blocks[j].kind == 1) {
                if (gc_block_add_cont(&g_blocks[j]) != 0) return -1;
                return 0;
            }
            if (g_blocks[j].kind == 2) {
                if (gc_block_add_cont(&g_blocks[j]) != 0) return -1;
                return 0;
            }
        }
        return -1;
    } else if (starts_with(line, "call")) {
        int fi;
        uint8_t argc = 0;
        uint32_t before;
        q = line + 4;
        if (gc_parse_ident(&q, name, sizeof(name)) != 0) return -1;
        for (;;) {
            q = gc_skip_ws(q);
            if (*q == '\0') break;
            before = g_code_len;
            if (emit_expr(&q) != 0) break;
            if (g_code_len == before) break;
            argc++;
        }
        fi = gc_find_fn(name);
        if (fi >= 0 && g_fns[fi].defined) {
            if (emit_call(g_fns[fi].entry, argc) != 0) return -1;
        } else {
            if (fi < 0) fi = gc_add_fn(name);
            if (fi < 0 || g_npend >= GC_MAX_PEND) return -1;
            g_pend[g_npend].patch_at = g_code_len + 1;
            strncpy(g_pend[g_npend].name, name, GC_NAME_MAX - 1);
            g_pend[g_npend].name[GC_NAME_MAX - 1] = '\0';
            g_pend[g_npend].arity = argc;
            g_npend++;
            if (emit_call(0, argc) != 0) return -1;
        }
    } else if (starts_with(line, "return")) {
        q = gc_skip_ws(line + 6);
        if (*q != '\0') {
            if (emit_expr(&q) != 0) return -1;
            if (emit_byte(GBC_RET_V) != 0) return -1;
        } else {
            if (emit_byte(GBC_RET) != 0) return -1;
        }
    } else if (starts_with(line, "write")) {
        q = line + 5;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_FS_WRITE) != 0) return -1;
        if (emit_byte(GBC_POP) != 0) return -1;
    } else if (starts_with(line, "push")) {
        q = line + 4;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_LIST_PUSH) != 0) return -1;
        if (emit_byte(GBC_POP) != 0) return -1;
    } else if (starts_with(line, "set")) {
        q = line + 3;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_SET) != 0) return -1;
        if (emit_byte(GBC_POP) != 0) return -1;
    } else if (starts_with(line, "dos_run")) {
        q = line + 7;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_DOS_RUN) != 0) return -1;
        if (emit_byte(GBC_POP) != 0) return -1;
    } else if (starts_with(line, "window")) {
        uint16_t w = 420, h = 240;
        q = gc_skip_ws(line + 6);
        if (*q == '"') {
            q++;
            li = 0;
            while (*q && *q != '"' && li + 1 < sizeof(lit)) lit[li++] = *q++;
            lit[li] = '\0';
            if (*q == '"') q++;
            q = gc_skip_ws(q);
            if (*q) {
                uint16_t tw = 0, th = 0;
                if (gc_parse_u16(q, &tw) == 0) {
                    while (*q >= '0' && *q <= '9') q++;
                    if (*q == 'x' || *q == 'X') {
                        q++;
                        if (gc_parse_u16(q, &th) == 0) { w = tw; h = th; }
                    }
                }
            }
            if (g_ro_len + li + 1 < g_ro_cap && g_code_len + 9 < g_code_cap) {
                uint32_t off = g_ro_len;
                memcpy(g_ro + g_ro_len, lit, li + 1);
                g_ro_len += (uint32_t)li + 1;
                g_code[g_code_len++] = GBC_GUI_CREATE;
                wr32(g_code + g_code_len, off); g_code_len += 4;
                wr16(g_code + g_code_len, w); g_code_len += 2;
                wr16(g_code + g_code_len, h); g_code_len += 2;
                *kind_out = GOB_KIND_GUI;
            } else return -1;
        }
    } else if (starts_with(line, "text")) {
        q = gc_skip_ws(line + 4);
        if (*q == '"') {
            q++;
            li = 0;
            while (*q && *q != '"' && li + 1 < sizeof(lit)) lit[li++] = *q++;
            lit[li] = '\0';
            if (g_ro_len + li + 1 < g_ro_cap && g_code_len + 13 < g_code_cap) {
                uint32_t off = g_ro_len;
                memcpy(g_ro + g_ro_len, lit, li + 1);
                g_ro_len += (uint32_t)li + 1;
                g_code[g_code_len++] = GBC_GUI_TEXT;
                wr32(g_code + g_code_len, 0); g_code_len += 4;
                wr16(g_code + g_code_len, 0); g_code_len += 2;
                wr16(g_code + g_code_len, 0); g_code_len += 2;
                wr32(g_code + g_code_len, off); g_code_len += 4;
            } else return -1;
        } else {
            if (emit_expr(&q) != 0) return -1;
            if (emit_byte(GBC_GUI_TEXT_S) != 0) return -1;
        }
    } else if (starts_with(line, "cleargui")) {
        if (emit_byte(GBC_GUI_CLEAR) != 0) return -1;
        *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "fill")) {
        q = line + 4;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_GFX_FILL) != 0) return -1;
        *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "rect")) {
        q = line + 4;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_GFX_RECT) != 0) return -1;
        *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "label")) {
        q = line + 5;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_expr(&q) != 0) return -1;
        if (emit_byte(GBC_GFX_LABEL) != 0) return -1;
        *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "present")) {
        if (emit_byte(GBC_GFX_PRESENT) != 0) return -1;
        *kind_out = GOB_KIND_GUI;
    } else if (starts_with(line, "wait")) {
        if (emit_op32(GBC_GUI_WAIT, 0) != 0) return -1;
    } else if (starts_with(line, "sleep")) {
        int32_t ms = 0;
        q = line + 5;
        if (gc_parse_i32(&q, &ms) != 0) return -1;
        if (emit_op32(GBC_SLEEP_MS, (uint32_t)ms) != 0) return -1;
        if (*kind_out == GOB_KIND_CONSOLE) *kind_out = GOB_KIND_AUTO;
    } else if (starts_with(line, "clear")) {
        uint32_t rgba = 0;
        q = gc_skip_ws(line + 5);
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
        while ((*q >= '0' && *q <= '9') ||
               (*q >= 'a' && *q <= 'f') ||
               (*q >= 'A' && *q <= 'F')) {
            uint32_t dig;
            if (*q <= '9') dig = (uint32_t)(*q - '0');
            else if (*q <= 'F') dig = (uint32_t)(*q - 'A') + 10u;
            else dig = (uint32_t)(*q - 'a') + 10u;
            rgba = (rgba << 4) | dig;
            q++;
        }
        if (emit_op32(GBC_GFX3D_CLEAR, rgba) != 0) return -1;
        *kind_out = GOB_KIND_GFX3D;
    } else if (starts_with(line, "exit")) {
        if (emit_op32(GBC_EXIT, 0) != 0) return -1;
        *has_exit = 1;
    } else if (gc_parse_ident(&line, name, sizeof(name)) == 0) {
        int is_local;
        uint8_t slot;
        q = gc_skip_ws(line);
        if (*q == '=') {
            q++;
            if (gc_resolve_var(name, &is_local, &slot) != 0) {
                if (g_in_fn) {
                    if (gc_ensure_local(name) < 0) return -1;
                } else {
                    if (gc_ensure_global(name) < 0) return -1;
                }
            }
            if (emit_expr(&q) != 0) return -1;
            if (gc_emit_store_name(name) != 0) return -1;
        } else {
            int fi = gc_find_fn(name);
            uint8_t a;
            if (fi < 0 || !g_fns[fi].defined) return -1;
            for (a = 0; a < g_fns[fi].arity; a++) {
                if (emit_expr(&q) != 0) return -1;
            }
            if (emit_call(g_fns[fi].entry, g_fns[fi].arity) != 0) return -1;
        }
    } else return -1;

    return 0;
}

int gooberc_compile(const char* src_path, const char* out_path) {
    FileHandle* fh;
    uint8_t* src;
    size_t src_cap = 32768;
    size_t src_len = 0;
    size_t n;
    uint8_t code_buf[16384];
    uint8_t ro_buf[8192];
    int kind = GOB_KIND_CONSOLE;
    const char* p;
    gob_header_t hdr;
    uint8_t* outbuf;
    size_t out_total;
    int has_exit = 0;
    char line_buf[512];
    int i;

    if (!src_path || !out_path) return -1;

    g_code = code_buf; g_code_cap = sizeof(code_buf); g_code_len = 0;
    g_ro = ro_buf; g_ro_cap = sizeof(ro_buf); g_ro_len = 0;
    g_nglobal = g_nlocal = g_nfn = g_nblock = g_npend = 0;
    g_in_fn = 0;

    fh = fs_open(src_path);
    if (!fh) {
        print("gooberc: cannot open source\n");
        return -1;
    }
    src = (uint8_t*)kmalloc(src_cap);
    if (!src) { fs_close(fh); return -1; }
    while ((n = fs_read(fh, src + src_len, src_cap - src_len - 1)) > 0)
        src_len += n;
    fs_close(fh);
    src[src_len] = '\0';

    p = (const char*)src;
    while (*p) {
        const char* line_start = p;
        const char* line = gc_skip_ws(p);
        size_t lb = 0;

        if (*line == '\0' || *line == '\n' || *line == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        while (*p && *p != '\n' && lb + 1 < sizeof(line_buf))
            line_buf[lb++] = *p++;
        line_buf[lb] = '\0';
        (void)line_start;

        if (gc_compile_line(line_buf, &kind, &has_exit) != 0) {
            print("gooberc: bad line: ");
            print(line_buf);
            print("\n");
            kfree(src);
            return -1;
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (g_nblock != 0) {
        print("gooberc: unclosed block (missing end)\n");
        kfree(src);
        return -1;
    }
    for (i = 0; i < g_nfn; i++) {
        if (!g_fns[i].defined) {
            print("gooberc: undefined fn: ");
            print(g_fns[i].name);
            print("\n");
            kfree(src);
            return -1;
        }
    }

    if (!has_exit) {
        if (emit_op32(GBC_EXIT, 0) != 0) {
            kfree(src);
            return -1;
        }
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GOB_MAGIC;
    hdr.version = GOB_VERSION;
#ifdef __x86_64__
    hdr.arch = GOB_ARCH_X86_64;
#else
    hdr.arch = GOB_ARCH_I386;
#endif
    hdr.kind = (uint8_t)kind;
    hdr.flags = GOB_FLAG_BYTECODE;
    hdr.entry = 0;
    hdr.code_size = g_code_len;
    hdr.rodata_size = g_ro_len;

    out_total = sizeof(hdr) + g_code_len + g_ro_len;
    outbuf = (uint8_t*)kmalloc(out_total);
    if (!outbuf) { kfree(src); return -1; }
    memcpy(outbuf, &hdr, sizeof(hdr));
    memcpy(outbuf + sizeof(hdr), g_code, g_code_len);
    memcpy(outbuf + sizeof(hdr) + g_code_len, g_ro, g_ro_len);

    {
        char out_dir[64];
        char out_base[32];
        const char* slash = 0;
        const char* s = out_path;
        size_t di = 0, bi = 0;
        while (*s) {
            if (*s == '/') slash = s;
            s++;
        }
        if (slash) {
            while (out_path + di < slash && di + 1 < sizeof(out_dir)) {
                out_dir[di] = out_path[di];
                di++;
            }
            out_dir[di] = '\0';
            slash++;
            while (slash[bi] && bi + 1 < sizeof(out_base)) {
                out_base[bi] = slash[bi];
                bi++;
            }
            out_base[bi] = '\0';
            (void)fs_create_dir(out_dir);
            if (fs_change_dir(out_dir) == 0) {
                if (fs_write(out_base, outbuf, out_total) != 0) {
                    print("gooberc: write failed\n");
                    fs_cd_up();
                    kfree(outbuf); kfree(src);
                    return -1;
                }
                fs_cd_up();
            } else {
                print("gooberc: cannot enter output dir\n");
                kfree(outbuf); kfree(src);
                return -1;
            }
        } else if (fs_write(out_path, outbuf, out_total) != 0) {
            print("gooberc: write failed\n");
            kfree(outbuf); kfree(src);
            return -1;
        }
    }

    kfree(outbuf);
    kfree(src);
    print("gooberc: wrote ");
    print(out_path);
    print("\n");
    return 0;
}
