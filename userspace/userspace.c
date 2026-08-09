#include "userspace.h"
#include "syscall.h"
#include "../fs/filesystem.h"
#include "../lib/memory.h"
#include "../lib/string.h"
#include "../taskmgr/process.h"
#include "../gui/vesa_window.h"
#include "../dosemu/dosemu.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/timer.h"

extern void print(const char*);
extern void gdt64_reload(void);
extern void gdt64_set_tss_rsp0(uint64_t rsp0);
extern void enter_user64(uint64_t rip, uint64_t rsp);
extern void syscall80_stub(void);

#ifdef __x86_64__
extern void x64_idt_set_user_gate(int vec, void (*handler)(void));
#endif

static int g_current_gob_pid = -1;
static int g_gui_wait_done = 0;
static int g_userspace_ready = 0;

/* Simple GUI window slots for Goober apps */
#define GOB_MAX_WINS 4
#define GOB_WIN_LINES 28
#define GOB_WIN_COLS  72
#define GOB_GFX_MAX_CMDS 384
#define GOB_GFX_OP_RECT  1
#define GOB_GFX_OP_LABEL 2
typedef struct {
    uint8_t op;
    int16_t x, y, w, h;
    uint32_t c0; /* rect fill / label fg */
    uint32_t c1; /* label bg */
    char text[40];
} gob_gfx_cmd_t;

typedef struct {
    int used;
    VWindow* win;
    char lines[GOB_WIN_LINES][GOB_WIN_COLS];
    int line_count;
    int closed;
    int interactive;
    int pending_key;
    /* 2D canvas mode (fill/rect/label) */
    int canvas;
    uint32_t canvas_bg;
    gob_gfx_cmd_t cmds[GOB_GFX_MAX_CMDS];
    int cmd_count;
} gob_win_t;

static gob_win_t g_wins[GOB_MAX_WINS];
/* Slot used by fill/rect/label/getkey — always the most recent window. */
static int g_gob_active_slot = -1;

static void gob_wins_cleanup(void) {
    int i;
    for (i = 0; i < GOB_MAX_WINS; i++) {
        if (!g_wins[i].used) continue;
        if (g_wins[i].win) {
            g_wins[i].win->user_data = NULL;
            g_wins[i].win->render = NULL;
            g_wins[i].win->click_handler = NULL;
            g_wins[i].win->key_handler = NULL;
            vdesk_close_window(g_wins[i].win);
        }
        memset(&g_wins[i], 0, sizeof(g_wins[i]));
    }
    g_gob_active_slot = -1;
}

static gob_win_t* gob_active_win(void) {
    if (g_gob_active_slot < 0 || g_gob_active_slot >= GOB_MAX_WINS)
        return NULL;
    if (!g_wins[g_gob_active_slot].used)
        return NULL;
    return &g_wins[g_gob_active_slot];
}

static void gob_win_render(VWindow* win, int cx, int cy, int cw, int ch) {
    gob_win_t* gw = (gob_win_t*)win->user_data;
    int i;
    int line_h = 14;
    if (!gw) {
        vdesk_draw_rect(cx, cy, cw, ch, vdesk_shell_bg_color());
        return;
    }
    if (gw->canvas) {
        vdesk_draw_rect(cx, cy, cw, ch, gw->canvas_bg);
        for (i = 0; i < gw->cmd_count; i++) {
            gob_gfx_cmd_t* c = &gw->cmds[i];
            int x = cx + c->x;
            int y = cy + c->y;
            if (c->op == GOB_GFX_OP_RECT) {
                int w = c->w;
                int h = c->h;
                if (w < 1) w = 1;
                if (h < 1) h = 1;
                if (x + w > cx + cw) w = cx + cw - x;
                if (y + h > cy + ch) h = cy + ch - y;
                if (w > 0 && h > 0)
                    vdesk_draw_rect(x, y, w, h, c->c0);
            } else if (c->op == GOB_GFX_OP_LABEL) {
                if (x >= cx && y >= cy && x < cx + cw && y < cy + ch)
                    vdesk_draw_text(x, y, c->text, c->c0, c->c1);
            }
        }
        return;
    }
    vdesk_draw_rect(cx, cy, cw, ch, vdesk_shell_bg_color());
    for (i = 0; i < gw->line_count; i++) {
        vdesk_draw_text(cx + 10, cy + 8 + i * line_h, gw->lines[i],
                        vdesk_shell_output_color(), vdesk_shell_bg_color());
    }
    if (gw->interactive) {
        vdesk_draw_text(cx + 10, cy + ch - 22,
                        "Arrows move  Space/Enter act  ESC quit",
                        vdesk_shell_muted_color(), vdesk_shell_bg_color());
    } else {
        vdesk_draw_text(cx + 10, cy + ch - 22, "[Click to close]",
                        vdesk_shell_muted_color(), vdesk_shell_bg_color());
    }
}

static void gob_win_dirty(gob_win_t* gw) {
    if (gw && gw->win)
        vdesk_mark_dirty(gw->win->x, gw->win->y, gw->win->width, gw->win->height);
}

static void gob_win_click(VWindow* win, int lx, int ly) {
    gob_win_t* gw = (gob_win_t*)win->user_data;
    (void)lx; (void)ly;
    if (!gw) return;
    if (gw->interactive) return; /* games use ESC / window chrome */
    gw->closed = 1;
    g_gui_wait_done = 1;
    vdesk_close_window(win);
    gw->win = NULL;
    gw->used = 0;
    if (g_gob_active_slot >= 0 && &g_wins[g_gob_active_slot] == gw)
        g_gob_active_slot = -1;
}

static void gob_win_key(VWindow* win, char key) {
    gob_win_t* gw = (gob_win_t*)win->user_data;
    if (!gw) return;
    gw->pending_key = (unsigned char)key;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void basename_copy(char* out, size_t out_sz, const char* path) {
    const char* s = path;
    const char* last = path;
    size_t i = 0;
    while (*s) {
        if (*s == '/' || *s == '\\') last = s + 1;
        s++;
    }
    while (last[i] && last[i] != '.' && i + 1 < out_sz) {
        out[i] = last[i];
        i++;
    }
    out[i] = '\0';
    if (i == 0 && out_sz > 0) {
        out[0] = '?';
        out[1] = '\0';
    }
}

static int32_t rd32s(const uint8_t* p) {
    return (int32_t)rd32(p);
}

static void gob_print_i32(int32_t v) {
    char buf[16];
    char rev[16];
    int n = 0, i = 0, neg = 0;
    uint32_t u;
    if (v == (int32_t)0x80000000) {
        print("-2147483648\n");
        return;
    }
    if (v < 0) {
        neg = 1;
        u = (uint32_t)(-v);
    } else {
        u = (uint32_t)v;
    }
    if (u == 0) rev[n++] = '0';
    while (u > 0 && n < 14) {
        rev[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    if (neg) buf[i++] = '-';
    while (n > 0) buf[i++] = rev[--n];
    buf[i++] = '\n';
    buf[i] = '\0';
    print(buf);
}

/* ---- GooberC v2 heap / frames ---- */
typedef struct {
    uint8_t used;
    uint8_t type;
    uint16_t len;
    uint16_t cap;
    uint32_t off; /* heap: str/blob bytes; list int32[]; map key/val pairs */
} gob_obj_t;

typedef struct {
    uint32_t ret_ip;
    int32_t locals[GBC_MAX_LOCALS];
} gob_frame_t;

static uint8_t g_heap[GBC_HEAP_BYTES];
static uint32_t g_heap_top;
static gob_obj_t g_objs[GBC_MAX_OBJECTS];
static char g_gob_err[96];

static void gob_set_err(const char* msg) {
    size_t i = 0;
    if (!msg) msg = "error";
    while (msg[i] && i + 1 < sizeof(g_gob_err)) {
        g_gob_err[i] = msg[i];
        i++;
    }
    g_gob_err[i] = '\0';
}

static void gob_heap_reset(void) {
    int i;
    g_heap_top = 0;
    g_gob_err[0] = '\0';
    for (i = 0; i < GBC_MAX_OBJECTS; i++)
        g_objs[i].used = 0;
}

static int32_t* gob_list_items(gob_obj_t* o);
static int32_t* gob_map_slot(gob_obj_t* o, uint16_t i);

static void gob_mark_obj(int32_t v, uint8_t* mark) {
    uint32_t id;
    gob_obj_t* o;
    uint16_t i;
    if (!GBC_IS_OBJ(v)) return;
    id = GBC_OBJ_ID(v);
    if (id >= GBC_MAX_OBJECTS || !g_objs[id].used || mark[id]) return;
    mark[id] = 1;
    o = &g_objs[id];
    if (o->type == GBC_OBJ_LIST) {
        int32_t* items = gob_list_items(o);
        for (i = 0; i < o->len; i++)
            gob_mark_obj(items[i], mark);
    } else if (o->type == GBC_OBJ_MAP) {
        for (i = 0; i < o->len; i++) {
            int32_t* slot = gob_map_slot(o, i);
            gob_mark_obj(slot[0], mark);
            gob_mark_obj(slot[1], mark);
        }
    }
}

/* Free unreachable objects and compact the heap (game redraws allocate a lot). */
static void gob_gc(int32_t* globals, int32_t* stack, int sp) {
    uint8_t mark[GBC_MAX_OBJECTS];
    uint8_t* tmp;
    uint32_t top = 0;
    int i;
    memset(mark, 0, sizeof(mark));
    if (globals) {
        for (i = 0; i < GBC_MAX_GLOBALS; i++)
            gob_mark_obj(globals[i], mark);
    }
    if (stack) {
        for (i = 0; i < sp; i++)
            gob_mark_obj(stack[i], mark);
    }
    tmp = (uint8_t*)kmalloc(GBC_HEAP_BYTES);
    if (!tmp) {
        for (i = 1; i < GBC_MAX_OBJECTS; i++)
            if (g_objs[i].used && !mark[i])
                g_objs[i].used = 0;
        return;
    }
    for (i = 1; i < GBC_MAX_OBJECTS; i++) {
        gob_obj_t* o;
        uint32_t nbytes;
        if (!mark[i] || !g_objs[i].used) {
            if (g_objs[i].used && !mark[i])
                g_objs[i].used = 0;
            continue;
        }
        o = &g_objs[i];
        if (o->type == GBC_OBJ_STR || o->type == GBC_OBJ_BLOB) {
            nbytes = (uint32_t)o->len + 1u;
            if (top + nbytes > GBC_HEAP_BYTES) break;
            memcpy(tmp + top, g_heap + o->off, o->len);
            tmp[top + o->len] = 0;
            o->off = top;
            top += nbytes;
        } else if (o->type == GBC_OBJ_LIST) {
            nbytes = (uint32_t)o->cap * 4u;
            if (top + nbytes > GBC_HEAP_BYTES) break;
            memcpy(tmp + top, g_heap + o->off, nbytes);
            o->off = top;
            top += nbytes;
        } else if (o->type == GBC_OBJ_MAP) {
            nbytes = (uint32_t)o->cap * 8u;
            if (top + nbytes > GBC_HEAP_BYTES) break;
            memcpy(tmp + top, g_heap + o->off, nbytes);
            o->off = top;
            top += nbytes;
        }
    }
    memcpy(g_heap, tmp, top);
    g_heap_top = top;
    kfree(tmp);
}

static int gob_obj_alloc(uint8_t type) {
    int i;
    for (i = 1; i < GBC_MAX_OBJECTS; i++) {
        if (!g_objs[i].used) {
            memset(&g_objs[i], 0, sizeof(g_objs[i]));
            g_objs[i].used = 1;
            g_objs[i].type = type;
            return i;
        }
    }
    gob_set_err("out of objects");
    return -1;
}

static int32_t gob_make_str_from(const uint8_t* data, uint32_t len) {
    int id;
    if (len > GBC_STR_MAX) len = GBC_STR_MAX;
    if (g_heap_top + len + 1 > GBC_HEAP_BYTES) {
        gob_set_err("heap full");
        return 0;
    }
    id = gob_obj_alloc(GBC_OBJ_STR);
    if (id < 0) return 0;
    g_objs[id].off = g_heap_top;
    g_objs[id].len = (uint16_t)len;
    if (len) memcpy(g_heap + g_heap_top, data, len);
    g_heap[g_heap_top + len] = 0;
    g_heap_top += len + 1;
    return GBC_MAKE_OBJ(id);
}

static int32_t gob_make_str_ro(const uint8_t* rodata, uint32_t rodata_size,
                               uint32_t off) {
    uint32_t len = 0;
    if (off >= rodata_size) return 0;
    while (off + len < rodata_size && rodata[off + len]) len++;
    return gob_make_str_from(rodata + off, len);
}

static const char* gob_str_cstr(int32_t v, uint32_t* out_len) {
    uint32_t id;
    if (!GBC_IS_OBJ(v)) return NULL;
    id = GBC_OBJ_ID(v);
    if (id >= GBC_MAX_OBJECTS || !g_objs[id].used ||
        g_objs[id].type != GBC_OBJ_STR)
        return NULL;
    if (out_len) *out_len = g_objs[id].len;
    return (const char*)(g_heap + g_objs[id].off);
}

static int32_t* gob_list_items(gob_obj_t* o) {
    return (int32_t*)(g_heap + o->off);
}

/* Map layout: int32_t pairs [key, val] * cap at heap off */
static int32_t* gob_map_slot(gob_obj_t* o, uint16_t i) {
    return (int32_t*)(g_heap + o->off) + (size_t)i * 2u;
}

static int gob_heap_alloc_words(uint32_t nwords, uint32_t* out_off) {
    uint32_t nbytes = nwords * 4u;
    if (g_heap_top + nbytes > GBC_HEAP_BYTES) {
        gob_set_err("heap full");
        return -1;
    }
    *out_off = g_heap_top;
    memset(g_heap + g_heap_top, 0, nbytes);
    g_heap_top += nbytes;
    return 0;
}

static int gob_list_grow(gob_obj_t* o, uint16_t need) {
    uint16_t ncap = o->cap ? o->cap : 8;
    uint32_t off;
    int32_t* src;
    int32_t* dst;
    uint16_t i;
    while (ncap < need) {
        if (ncap >= GBC_LIST_MAX) {
            gob_set_err("list too large");
            return -1;
        }
        ncap = (uint16_t)(ncap * 2u);
        if (ncap > GBC_LIST_MAX) ncap = GBC_LIST_MAX;
    }
    if (gob_heap_alloc_words(ncap, &off) != 0) return -1;
    src = gob_list_items(o);
    dst = (int32_t*)(g_heap + off);
    for (i = 0; i < o->len; i++) dst[i] = src[i];
    o->off = off;
    o->cap = ncap;
    return 0;
}

static Directory* gob_resolve_dir(const char* path) {
    Directory* dir;
    const char* p;
    char seg[MAX_NAME_LEN];
    size_t n;

    if (!path || !path[0] || (path[0] == '.' && path[1] == '\0'))
        return fs_get_cwd_dir();

    if (path[0] == '/') {
        dir = fs_get_cwd_dir();
        while (dir && dir->parent) dir = dir->parent;
        p = path + 1;
        while (*p == '/') p++;
        if (!*p) return dir;
    } else {
        dir = fs_get_cwd_dir();
        p = path;
    }

    while (dir && *p) {
        n = 0;
        while (p[n] && p[n] != '/' && n + 1 < sizeof(seg)) {
            seg[n] = p[n];
            n++;
        }
        seg[n] = '\0';
        p += n;
        while (*p == '/') p++;
        if (!seg[0]) continue;
        if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            if (!dir->parent) return NULL;
            dir = dir->parent;
        } else if (!(seg[0] == '.' && seg[1] == '\0')) {
            dir = fs_dir_find_child(dir, seg);
            if (!dir) return NULL;
        }
    }
    if (dir) fs_dir_refresh(dir);
    return dir;
}

static int gob_str_eq(int32_t a, int32_t b) {
    uint32_t la = 0, lb = 0;
    const char* sa = gob_str_cstr(a, &la);
    const char* sb = gob_str_cstr(b, &lb);
    uint32_t i;
    if (!sa || !sb || la != lb) return 0;
    for (i = 0; i < la; i++)
        if (sa[i] != sb[i]) return 0;
    return 1;
}

static void gob_print_val(int32_t v) {
    uint32_t len = 0;
    const char* s;
    if (GBC_IS_OBJ(v)) {
        s = gob_str_cstr(v, &len);
        if (s) {
            char tmp[128];
            uint32_t n = len < sizeof(tmp) - 1 ? len : (uint32_t)sizeof(tmp) - 1;
            memcpy(tmp, s, n);
            tmp[n] = '\0';
            print(tmp);
            print("\n");
            return;
        }
        if (GBC_OBJ_ID(v) < GBC_MAX_OBJECTS && g_objs[GBC_OBJ_ID(v)].used) {
            if (g_objs[GBC_OBJ_ID(v)].type == GBC_OBJ_LIST) {
                print("[list]\n");
                return;
            }
            if (g_objs[GBC_OBJ_ID(v)].type == GBC_OBJ_MAP) {
                print("[map]\n");
                return;
            }
            if (g_objs[GBC_OBJ_ID(v)].type == GBC_OBJ_BLOB) {
                print("[blob]\n");
                return;
            }
        }
        print("[obj]\n");
        return;
    }
    gob_print_i32(v);
}

static int gob_run_bytecode(const uint8_t* code, uint32_t code_size,
                            const uint8_t* rodata, uint32_t rodata_size,
                            uint32_t entry, int pid) {
    uint32_t ip = entry;
    int i;
    int32_t stack[GBC_OP_STACK];
    int sp = 0;
    int32_t globals[GBC_MAX_GLOBALS];
    gob_frame_t frames[GBC_CALL_STACK];
    int fp = -1; /* current frame index; -1 = top-level (globals only) */

    memset(globals, 0, sizeof(globals));
    gob_heap_reset();
    process_set_state(pid, PROC_STATE_RUNNING);
    g_current_gob_pid = pid;

    while (ip < code_size) {
        uint8_t op = code[ip++];
        if (op == GBC_NOP) continue;
        if (op == GBC_EXIT) {
            if (ip + 4 > code_size) break;
            ip += 4;
            break;
        }
        if (op == GBC_WRITE) {
            uint32_t off, len;
            if (ip + 8 > code_size) break;
            off = rd32(code + ip); ip += 4;
            len = rd32(code + ip); ip += 4;
            if (off + len <= rodata_size) {
                char tmp[128];
                uint32_t n = len < sizeof(tmp) - 1 ? len : (uint32_t)sizeof(tmp) - 1;
                memcpy(tmp, rodata + off, n);
                tmp[n] = '\0';
                print(tmp);
            }
            continue;
        }
        if (op == GBC_YIELD) {
            process_set_state(pid, PROC_STATE_READY);
            vdesk_pump_one_frame();
            process_set_state(pid, PROC_STATE_RUNNING);
            continue;
        }
        if (op == GBC_GUI_CREATE) {
            uint32_t title_off;
            uint16_t w, h;
            int slot = -1;
            if (ip + 8 > code_size) break;
            title_off = rd32(code + ip); ip += 4;
            w = rd16(code + ip); ip += 2;
            h = rd16(code + ip); ip += 2;
            /* One interactive Gob app at a time — free prior slots so the
             * next game always owns slot 0 (fill/rect/getkey target it). */
            gob_wins_cleanup();
            slot = 0;
            {
                const char* title = "GooberApp";
                char tbuf[48];
                if (title_off < rodata_size) {
                    size_t n = 0;
                    while (title_off + n < rodata_size && rodata[title_off + n] &&
                           n + 1 < sizeof(tbuf)) {
                        tbuf[n] = (char)rodata[title_off + n];
                        n++;
                    }
                    tbuf[n] = '\0';
                    title = tbuf;
                }
                memset(&g_wins[slot], 0, sizeof(g_wins[slot]));
                g_wins[slot].used = 1;
                g_gob_active_slot = slot;
                g_wins[slot].win = vdesk_create_window(title, 120, 100,
                                                       w ? w : 420, h ? h : 240);
                if (g_wins[slot].win) {
                    g_wins[slot].win->user_data = &g_wins[slot];
                    g_wins[slot].win->render = gob_win_render;
                    g_wins[slot].win->click_handler = gob_win_click;
                    g_wins[slot].win->key_handler = gob_win_key;
                    g_wins[slot].win->process_pid = pid;
                }
            }
            continue;
        }
        if (op == GBC_GUI_TEXT) {
            uint32_t slot, str_off;
            uint16_t x, y;
            if (ip + 12 > code_size) break;
            slot = rd32(code + ip); ip += 4;
            x = rd16(code + ip); ip += 2;
            y = rd16(code + ip); ip += 2;
            str_off = rd32(code + ip); ip += 4;
            (void)x; (void)y;
            if (slot < GOB_MAX_WINS && g_wins[slot].used &&
                g_wins[slot].line_count < GOB_WIN_LINES && str_off < rodata_size) {
                size_t n = 0;
                char* dst = g_wins[slot].lines[g_wins[slot].line_count];
                while (str_off + n < rodata_size && rodata[str_off + n] &&
                       n + 1 < GOB_WIN_COLS) {
                    dst[n] = (char)rodata[str_off + n];
                    n++;
                }
                dst[n] = '\0';
                g_wins[slot].line_count++;
                if (g_wins[slot].win) vdesk_mark_full_dirty();
            }
            continue;
        }
        if (op == GBC_GUI_WAIT) {
            uint32_t slot;
            if (ip + 4 > code_size) break;
            slot = rd32(code + ip); ip += 4;
            if (slot < GOB_MAX_WINS && g_wins[slot].used) {
                process_set_state(pid, PROC_STATE_BLOCKED);
                g_gui_wait_done = 0;
                while (!g_wins[slot].closed && !g_gui_wait_done)
                    vdesk_pump_one_frame();
                process_set_state(pid, PROC_STATE_RUNNING);
            }
            continue;
        }
        if (op == GBC_GUI_CLOSE) {
            uint32_t slot;
            if (ip + 4 > code_size) break;
            slot = rd32(code + ip); ip += 4;
            if (slot < GOB_MAX_WINS && g_wins[slot].used) {
                if (g_wins[slot].win) vdesk_close_window(g_wins[slot].win);
                g_wins[slot].used = 0;
            }
            continue;
        }
        if (op == GBC_SLEEP_MS) {
            /* Wall-clock sleep while still pumping the desktop so windows
             * stay responsive. (Frame-count sleeps raced on fast hosts.) */
            uint32_t ms;
            uint64_t deadline;
            uint32_t pumps = 0;
            uint32_t max_pumps;
            if (ip + 4 > code_size) break;
            ms = rd32(code + ip); ip += 4;
            if (ms == 0) ms = 1;
            if (ms > 5000u) ms = 5000u;
            deadline = timer_deadline_ms(ms);
            /* Bound pumps so a stuck clock cannot hang forever; keep the
             * ceiling high enough that fast hosts still wait out `ms`. */
            max_pumps = ms * 100u + 64u;
            if (max_pumps > 500000u) max_pumps = 500000u;
            process_set_state(pid, PROC_STATE_READY);
            while (!timer_deadline_expired(deadline) && pumps < max_pumps) {
                vdesk_pump_one_frame();
                pumps++;
            }
            process_set_state(pid, PROC_STATE_RUNNING);
            continue;
        }
        if (op == GBC_GFX3D_CLEAR) {
            if (ip + 4 > code_size) break;
            ip += 4;
            continue;
        }
        if (op == GBC_PUSH_I) {
            if (ip + 4 > code_size || sp >= GBC_OP_STACK) break;
            stack[sp++] = rd32s(code + ip); ip += 4;
            continue;
        }
        if (op == GBC_LOAD) {
            uint8_t slot;
            if (ip + 1 > code_size || sp >= GBC_OP_STACK) break;
            slot = code[ip++];
            if (slot >= GBC_MAX_GLOBALS) break;
            stack[sp++] = globals[slot];
            continue;
        }
        if (op == GBC_STORE) {
            uint8_t slot;
            if (ip + 1 > code_size || sp < 1) break;
            slot = code[ip++];
            if (slot >= GBC_MAX_GLOBALS) break;
            globals[slot] = stack[--sp];
            continue;
        }
        if (op == GBC_LOAD_LOCAL) {
            uint8_t slot;
            if (ip + 1 > code_size || sp >= GBC_OP_STACK || fp < 0) break;
            slot = code[ip++];
            if (slot >= GBC_MAX_LOCALS) break;
            stack[sp++] = frames[fp].locals[slot];
            continue;
        }
        if (op == GBC_STORE_LOCAL) {
            uint8_t slot;
            if (ip + 1 > code_size || sp < 1 || fp < 0) break;
            slot = code[ip++];
            if (slot >= GBC_MAX_LOCALS) break;
            frames[fp].locals[slot] = stack[--sp];
            continue;
        }
        if (op == GBC_ADD || op == GBC_SUB || op == GBC_MUL || op == GBC_DIV ||
            op == GBC_CMP_EQ || op == GBC_CMP_NE || op == GBC_CMP_LT ||
            op == GBC_CMP_LE || op == GBC_CMP_GT || op == GBC_CMP_GE) {
            int32_t a, b, r = 0;
            if (sp < 2) break;
            b = stack[--sp];
            a = stack[--sp];
            if (op == GBC_ADD && (GBC_IS_OBJ(a) || GBC_IS_OBJ(b))) {
                uint32_t la = 0, lb = 0;
                const char* sa = gob_str_cstr(a, &la);
                const char* sb = gob_str_cstr(b, &lb);
                if (sa && sb && g_heap_top + la + lb + 1 <= GBC_HEAP_BYTES) {
                    int id = gob_obj_alloc(GBC_OBJ_STR);
                    if (id >= 0) {
                        g_objs[id].off = g_heap_top;
                        g_objs[id].len = (uint16_t)(la + lb);
                        memcpy(g_heap + g_heap_top, sa, la);
                        memcpy(g_heap + g_heap_top + la, sb, lb);
                        g_heap[g_heap_top + la + lb] = 0;
                        g_heap_top += la + lb + 1;
                        stack[sp++] = GBC_MAKE_OBJ(id);
                        continue;
                    }
                }
                stack[sp++] = 0;
                continue;
            }
            if (op == GBC_ADD) r = a + b;
            else if (op == GBC_SUB) r = a - b;
            else if (op == GBC_MUL) r = a * b;
            else if (op == GBC_DIV) r = (b != 0) ? (a / b) : 0;
            else if (op == GBC_CMP_EQ) r = (a == b);
            else if (op == GBC_CMP_NE) r = (a != b);
            else if (op == GBC_CMP_LT) r = (a < b);
            else if (op == GBC_CMP_LE) r = (a <= b);
            else if (op == GBC_CMP_GT) r = (a > b);
            else if (op == GBC_CMP_GE) r = (a >= b);
            stack[sp++] = r;
            continue;
        }
        if (op == GBC_JMP) {
            uint32_t tgt;
            if (ip + 4 > code_size) break;
            tgt = rd32(code + ip);
            if (tgt > code_size) break;
            ip = tgt;
            continue;
        }
        if (op == GBC_JZ) {
            uint32_t tgt;
            int32_t v;
            if (ip + 4 > code_size || sp < 1) break;
            tgt = rd32(code + ip); ip += 4;
            v = stack[--sp];
            if (v == 0) {
                if (tgt > code_size) break;
                ip = tgt;
            }
            continue;
        }
        if (op == GBC_CALL) {
            uint32_t tgt;
            if (ip + 4 > code_size || fp + 1 >= GBC_CALL_STACK) break;
            tgt = rd32(code + ip); ip += 4;
            fp++;
            memset(&frames[fp], 0, sizeof(frames[fp]));
            frames[fp].ret_ip = ip;
            if (tgt > code_size) break;
            ip = tgt;
            continue;
        }
        if (op == GBC_CALL_N) {
            uint32_t tgt;
            uint8_t arity;
            int a;
            if (ip + 5 > code_size || fp + 1 >= GBC_CALL_STACK) break;
            tgt = rd32(code + ip); ip += 4;
            arity = code[ip++];
            if (sp < (int)arity) break;
            fp++;
            memset(&frames[fp], 0, sizeof(frames[fp]));
            frames[fp].ret_ip = ip;
            /* args were pushed left-to-right; top is last arg */
            for (a = (int)arity - 1; a >= 0; a--)
                frames[fp].locals[a] = stack[--sp];
            if (tgt > code_size) break;
            ip = tgt;
            continue;
        }
        if (op == GBC_RET || op == GBC_RET_V) {
            int32_t retv = 0;
            int has = 0;
            if (op == GBC_RET_V) {
                if (sp < 1) break;
                retv = stack[--sp];
                has = 1;
            }
            if (fp < 0) break;
            ip = frames[fp].ret_ip;
            fp--;
            if (has) {
                if (sp >= GBC_OP_STACK) break;
                stack[sp++] = retv;
            }
            continue;
        }
        if (op == GBC_PRINT_I) {
            if (sp < 1) break;
            gob_print_val(stack[--sp]);
            continue;
        }
        if (op == GBC_PRINT_RAW) {
            uint32_t len = 0;
            const char* s;
            if (sp < 1) break;
            s = gob_str_cstr(stack[--sp], &len);
            if (s) {
                char tmp[128];
                uint32_t n = len < sizeof(tmp) - 1 ? len : (uint32_t)sizeof(tmp) - 1;
                memcpy(tmp, s, n);
                tmp[n] = '\0';
                print(tmp);
            }
            continue;
        }
        if (op == GBC_PUSH_STR) {
            uint32_t off;
            if (ip + 4 > code_size || sp >= GBC_OP_STACK) break;
            off = rd32(code + ip); ip += 4;
            stack[sp++] = gob_make_str_ro(rodata, rodata_size, off);
            continue;
        }
        if (op == GBC_LEN) {
            int32_t v;
            if (sp < 1) break;
            v = stack[--sp];
            if (GBC_IS_OBJ(v)) {
                uint32_t id = GBC_OBJ_ID(v);
                if (id < GBC_MAX_OBJECTS && g_objs[id].used) {
                    if (g_objs[id].type == GBC_OBJ_STR ||
                        g_objs[id].type == GBC_OBJ_BLOB ||
                        g_objs[id].type == GBC_OBJ_LIST ||
                        g_objs[id].type == GBC_OBJ_MAP)
                        stack[sp++] = (int32_t)g_objs[id].len;
                    else stack[sp++] = 0;
                    continue;
                }
            }
            stack[sp++] = 0;
            continue;
        }
        if (op == GBC_LIST_NEW) {
            uint8_t count;
            int id, j;
            uint16_t cap;
            uint32_t off;
            int32_t* items;
            if (ip + 1 > code_size) break;
            count = code[ip++];
            if (sp < (int)count) {
                gob_set_err("bad list literal");
                break;
            }
            cap = count ? (uint16_t)count : 8;
            if (cap < 8) cap = 8;
            id = gob_obj_alloc(GBC_OBJ_LIST);
            if (id < 0) { sp -= count; stack[sp++] = 0; continue; }
            if (gob_heap_alloc_words(cap, &off) != 0) {
                g_objs[id].used = 0;
                sp -= count;
                stack[sp++] = 0;
                continue;
            }
            g_objs[id].off = off;
            g_objs[id].cap = cap;
            g_objs[id].len = count;
            items = gob_list_items(&g_objs[id]);
            for (j = (int)count - 1; j >= 0; j--)
                items[j] = stack[--sp];
            stack[sp++] = GBC_MAKE_OBJ(id);
            continue;
        }
        if (op == GBC_LIST_PUSH) {
            int32_t val, lst;
            uint32_t id;
            if (sp < 2) break;
            val = stack[--sp];
            lst = stack[--sp];
            if (!GBC_IS_OBJ(lst)) { stack[sp++] = lst; continue; }
            id = GBC_OBJ_ID(lst);
            if (id >= GBC_MAX_OBJECTS || !g_objs[id].used ||
                g_objs[id].type != GBC_OBJ_LIST) {
                gob_set_err("push needs list");
                stack[sp++] = lst;
                continue;
            }
            if (g_objs[id].len >= g_objs[id].cap &&
                gob_list_grow(&g_objs[id], (uint16_t)(g_objs[id].len + 1)) != 0) {
                stack[sp++] = lst;
                continue;
            }
            gob_list_items(&g_objs[id])[g_objs[id].len++] = val;
            stack[sp++] = lst;
            continue;
        }
        if (op == GBC_LIST_GET) {
            int32_t key, cont;
            uint32_t id;
            if (sp < 2) break;
            key = stack[--sp];
            cont = stack[--sp];
            if (!GBC_IS_OBJ(cont)) { stack[sp++] = 0; continue; }
            id = GBC_OBJ_ID(cont);
            if (id >= GBC_MAX_OBJECTS || !g_objs[id].used) {
                stack[sp++] = 0;
                continue;
            }
            if (g_objs[id].type == GBC_OBJ_LIST) {
                if (key < 0 || key >= (int32_t)g_objs[id].len) {
                    gob_set_err("list index out of range");
                    stack[sp++] = 0;
                    continue;
                }
                stack[sp++] = gob_list_items(&g_objs[id])[key];
                continue;
            }
            if (g_objs[id].type == GBC_OBJ_MAP) {
                uint16_t j;
                int found = 0;
                for (j = 0; j < g_objs[id].len; j++) {
                    int32_t* slot = gob_map_slot(&g_objs[id], j);
                    if (gob_str_eq(slot[0], key)) {
                        stack[sp++] = slot[1];
                        found = 1;
                        break;
                    }
                }
                if (!found) stack[sp++] = 0;
                continue;
            }
            stack[sp++] = 0;
            continue;
        }
        if (op == GBC_ALLOC) {
            int32_t n;
            int id;
            if (sp < 1) break;
            n = stack[--sp];
            if (n < 0) n = 0;
            if (n > (int32_t)GBC_STR_MAX) n = GBC_STR_MAX;
            if (g_heap_top + (uint32_t)n > GBC_HEAP_BYTES) {
                stack[sp++] = 0;
                continue;
            }
            id = gob_obj_alloc(GBC_OBJ_BLOB);
            if (id < 0) { stack[sp++] = 0; continue; }
            g_objs[id].off = g_heap_top;
            g_objs[id].len = (uint16_t)n;
            memset(g_heap + g_heap_top, 0, (size_t)n);
            g_heap_top += (uint32_t)n;
            stack[sp++] = GBC_MAKE_OBJ(id);
            continue;
        }
        if (op == GBC_FREE) {
            int32_t v;
            uint32_t id;
            if (sp < 1) break;
            v = stack[--sp];
            if (GBC_IS_OBJ(v)) {
                id = GBC_OBJ_ID(v);
                if (id < GBC_MAX_OBJECTS) g_objs[id].used = 0;
            }
            continue;
        }
        if (op == GBC_STR_JOIN) {
            int32_t b, a;
            uint32_t la = 0, lb = 0;
            const char* sa;
            const char* sb;
            if (sp < 2) break;
            b = stack[--sp];
            a = stack[--sp];
            sa = gob_str_cstr(a, &la);
            sb = gob_str_cstr(b, &lb);
            if (!sa || !sb || g_heap_top + la + lb + 1 > GBC_HEAP_BYTES) {
                stack[sp++] = 0;
                continue;
            }
            {
                int id = gob_obj_alloc(GBC_OBJ_STR);
                if (id < 0) { stack[sp++] = 0; continue; }
                g_objs[id].off = g_heap_top;
                g_objs[id].len = (uint16_t)(la + lb);
                memcpy(g_heap + g_heap_top, sa, la);
                memcpy(g_heap + g_heap_top + la, sb, lb);
                g_heap[g_heap_top + la + lb] = 0;
                g_heap_top += la + lb + 1;
                stack[sp++] = GBC_MAKE_OBJ(id);
            }
            continue;
        }
        if (op == GBC_DUP) {
            if (sp < 1 || sp >= GBC_OP_STACK) break;
            stack[sp] = stack[sp - 1];
            sp++;
            continue;
        }
        if (op == GBC_POP) {
            if (sp < 1) break;
            sp--;
            continue;
        }
        if (op == GBC_FS_EXISTS) {
            int32_t pathv;
            uint32_t len = 0;
            const char* path;
            FileHandle* fh;
            if (sp < 1) break;
            pathv = stack[--sp];
            path = gob_str_cstr(pathv, &len);
            if (!path) { stack[sp++] = 0; continue; }
            fh = fs_open(path);
            if (fh) { fs_close(fh); stack[sp++] = 1; }
            else stack[sp++] = 0;
            continue;
        }
        if (op == GBC_FS_READ) {
            int32_t pathv;
            uint32_t len = 0;
            const char* path;
            FileHandle* fh;
            uint8_t buf[GBC_STR_MAX];
            size_t got = 0, n;
            if (sp < 1) break;
            pathv = stack[--sp];
            path = gob_str_cstr(pathv, &len);
            if (!path) { stack[sp++] = 0; continue; }
            fh = fs_open(path);
            if (!fh) { stack[sp++] = 0; continue; }
            while (got + 1 < sizeof(buf) &&
                   (n = fs_read(fh, buf + got, sizeof(buf) - 1 - got)) > 0)
                got += n;
            fs_close(fh);
            buf[got] = 0;
            stack[sp++] = gob_make_str_from(buf, (uint32_t)got);
            continue;
        }
        if (op == GBC_FS_WRITE) {
            int32_t pathv, datav;
            uint32_t plen = 0, dlen = 0;
            const char* path;
            const char* data;
            int rc;
            if (sp < 2) break;
            datav = stack[--sp];
            pathv = stack[--sp];
            path = gob_str_cstr(pathv, &plen);
            data = gob_str_cstr(datav, &dlen);
            if (!path || !data) {
                gob_set_err("write needs string path/data");
                stack[sp++] = 0;
                continue;
            }
            rc = fs_write(path, (const uint8_t*)data, dlen);
            if (rc != 0) gob_set_err("write failed");
            stack[sp++] = (rc == 0) ? 1 : 0;
            continue;
        }
        if (op == GBC_STR_SLICE) {
            int32_t end, start, sv;
            uint32_t len = 0;
            const char* s;
            int32_t a, b;
            if (sp < 3) break;
            end = stack[--sp];
            start = stack[--sp];
            sv = stack[--sp];
            s = gob_str_cstr(sv, &len);
            if (!s) { gob_set_err("slice needs string"); stack[sp++] = 0; continue; }
            a = start < 0 ? 0 : start;
            b = end < 0 ? 0 : end;
            if (a > (int32_t)len) a = (int32_t)len;
            if (b > (int32_t)len) b = (int32_t)len;
            if (b < a) b = a;
            stack[sp++] = gob_make_str_from((const uint8_t*)s + (uint32_t)a,
                                            (uint32_t)(b - a));
            continue;
        }
        if (op == GBC_STR_FIND) {
            int32_t needle, hay;
            uint32_t nl = 0, hl = 0, i, j;
            const char* n;
            const char* h;
            int found = -1;
            if (sp < 2) break;
            needle = stack[--sp];
            hay = stack[--sp];
            h = gob_str_cstr(hay, &hl);
            n = gob_str_cstr(needle, &nl);
            if (!h || !n) { gob_set_err("find needs strings"); stack[sp++] = -1; continue; }
            if (nl == 0) { stack[sp++] = 0; continue; }
            if (nl <= hl) {
                for (i = 0; i + nl <= hl; i++) {
                    for (j = 0; j < nl; j++)
                        if (h[i + j] != n[j]) break;
                    if (j == nl) { found = (int)i; break; }
                }
            }
            stack[sp++] = found;
            continue;
        }
        if (op == GBC_PATH_JOIN) {
            int32_t b, a;
            uint32_t la = 0, lb = 0;
            const char* sa;
            const char* sb;
            char buf[GBC_STR_MAX];
            uint32_t n = 0;
            if (sp < 2) break;
            b = stack[--sp];
            a = stack[--sp];
            sa = gob_str_cstr(a, &la);
            sb = gob_str_cstr(b, &lb);
            if (!sa || !sb) { gob_set_err("path_join needs strings"); stack[sp++] = 0; continue; }
            if (la + lb + 2 > sizeof(buf)) {
                gob_set_err("path too long");
                stack[sp++] = 0;
                continue;
            }
            if (la) { memcpy(buf, sa, la); n = la; }
            if (n && buf[n - 1] != '/') buf[n++] = '/';
            while (lb && sb[0] == '/') { sb++; lb--; }
            if (lb) { memcpy(buf + n, sb, lb); n += lb; }
            buf[n] = 0;
            stack[sp++] = gob_make_str_from((const uint8_t*)buf, n);
            continue;
        }
        if (op == GBC_PATH_DIR || op == GBC_PATH_BASE) {
            int32_t pv;
            uint32_t len = 0, i, cut = 0;
            const char* path;
            int has_slash = 0;
            if (sp < 1) break;
            pv = stack[--sp];
            path = gob_str_cstr(pv, &len);
            if (!path) { gob_set_err("path needs string"); stack[sp++] = 0; continue; }
            for (i = 0; i < len; i++) {
                if (path[i] == '/' || path[i] == '\\') {
                    cut = i;
                    has_slash = 1;
                }
            }
            if (op == GBC_PATH_BASE) {
                if (!has_slash)
                    stack[sp++] = gob_make_str_from((const uint8_t*)path, len);
                else
                    stack[sp++] = gob_make_str_from((const uint8_t*)path + cut + 1,
                                                    len - cut - 1);
            } else {
                if (!has_slash)
                    stack[sp++] = gob_make_str_from((const uint8_t*)".", 1);
                else if (cut == 0)
                    stack[sp++] = gob_make_str_from((const uint8_t*)"/", 1);
                else
                    stack[sp++] = gob_make_str_from((const uint8_t*)path, cut);
            }
            continue;
        }
        if (op == GBC_FS_LIST) {
            int32_t pathv;
            uint32_t plen = 0;
            const char* path;
            Directory* dir;
            int id;
            uint16_t cap, n = 0, j;
            uint32_t off;
            int32_t* items;
            if (sp < 1) break;
            pathv = stack[--sp];
            path = gob_str_cstr(pathv, &plen);
            if (!path) { gob_set_err("listdir needs string"); stack[sp++] = 0; continue; }
            dir = gob_resolve_dir(path);
            if (!dir) { gob_set_err("listdir: path not found"); stack[sp++] = 0; continue; }
            cap = (uint16_t)(dir->file_count + dir->child_count);
            if (cap < 8) cap = 8;
            if (cap > GBC_LIST_MAX) cap = GBC_LIST_MAX;
            id = gob_obj_alloc(GBC_OBJ_LIST);
            if (id < 0) { stack[sp++] = 0; continue; }
            if (gob_heap_alloc_words(cap, &off) != 0) {
                g_objs[id].used = 0;
                stack[sp++] = 0;
                continue;
            }
            g_objs[id].off = off;
            g_objs[id].cap = cap;
            items = gob_list_items(&g_objs[id]);
            for (j = 0; j < dir->child_count && n < cap; j++) {
                const char* nm = dir->children[j].name;
                size_t nl = 0;
                while (nm[nl]) nl++;
                items[n++] = gob_make_str_from((const uint8_t*)nm, (uint32_t)nl);
            }
            for (j = 0; j < dir->file_count && n < cap; j++) {
                const char* nm = dir->files[j].name;
                size_t nl = 0;
                while (nm[nl]) nl++;
                items[n++] = gob_make_str_from((const uint8_t*)nm, (uint32_t)nl);
            }
            g_objs[id].len = n;
            stack[sp++] = GBC_MAKE_OBJ(id);
            continue;
        }
        if (op == GBC_SET) {
            int32_t val, key, cont;
            uint32_t id;
            if (sp < 3) break;
            val = stack[--sp];
            key = stack[--sp];
            cont = stack[--sp];
            if (!GBC_IS_OBJ(cont)) { stack[sp++] = 0; continue; }
            id = GBC_OBJ_ID(cont);
            if (id >= GBC_MAX_OBJECTS || !g_objs[id].used) {
                stack[sp++] = 0;
                continue;
            }
            if (g_objs[id].type == GBC_OBJ_LIST) {
                if (key < 0 || key >= (int32_t)g_objs[id].len) {
                    gob_set_err("set: list index out of range");
                    stack[sp++] = cont;
                    continue;
                }
                gob_list_items(&g_objs[id])[key] = val;
                stack[sp++] = cont;
                continue;
            }
            if (g_objs[id].type == GBC_OBJ_MAP) {
                uint16_t j;
                int found = 0;
                if (!GBC_IS_OBJ(key) ||
                    GBC_OBJ_ID(key) >= GBC_MAX_OBJECTS ||
                    !g_objs[GBC_OBJ_ID(key)].used ||
                    g_objs[GBC_OBJ_ID(key)].type != GBC_OBJ_STR) {
                    gob_set_err("set: map key must be string");
                    stack[sp++] = cont;
                    continue;
                }
                for (j = 0; j < g_objs[id].len; j++) {
                    int32_t* slot = gob_map_slot(&g_objs[id], j);
                    if (gob_str_eq(slot[0], key)) {
                        slot[1] = val;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    uint32_t words, noff;
                    int32_t* src;
                    int32_t* dst;
                    uint16_t k;
                    if (g_objs[id].len >= GBC_MAP_MAX) {
                        gob_set_err("map full");
                        stack[sp++] = cont;
                        continue;
                    }
                    if (g_objs[id].len >= g_objs[id].cap) {
                        uint16_t ncap = g_objs[id].cap ? (uint16_t)(g_objs[id].cap * 2u) : 8;
                        if (ncap > GBC_MAP_MAX) ncap = GBC_MAP_MAX;
                        words = (uint32_t)ncap * 2u;
                        if (gob_heap_alloc_words(words, &noff) != 0) {
                            stack[sp++] = cont;
                            continue;
                        }
                        src = (int32_t*)(g_heap + g_objs[id].off);
                        dst = (int32_t*)(g_heap + noff);
                        for (k = 0; k < g_objs[id].len * 2u; k++) dst[k] = src[k];
                        g_objs[id].off = noff;
                        g_objs[id].cap = ncap;
                    }
                    {
                        int32_t* slot = gob_map_slot(&g_objs[id], g_objs[id].len);
                        slot[0] = key;
                        slot[1] = val;
                        g_objs[id].len++;
                    }
                }
                stack[sp++] = cont;
                continue;
            }
            gob_set_err("set needs list or map");
            stack[sp++] = cont;
            continue;
        }
        if (op == GBC_TYPEOF) {
            int32_t v;
            if (sp < 1) break;
            v = stack[--sp];
            if (!GBC_IS_OBJ(v)) { stack[sp++] = 0; continue; }
            {
                uint32_t id = GBC_OBJ_ID(v);
                if (id >= GBC_MAX_OBJECTS || !g_objs[id].used) {
                    stack[sp++] = 0;
                    continue;
                }
                switch (g_objs[id].type) {
                case GBC_OBJ_STR:  stack[sp++] = 1; break;
                case GBC_OBJ_LIST: stack[sp++] = 2; break;
                case GBC_OBJ_BLOB: stack[sp++] = 3; break;
                case GBC_OBJ_MAP:  stack[sp++] = 4; break;
                default: stack[sp++] = 0; break;
                }
            }
            continue;
        }
        if (op == GBC_LAST_ERR) {
            size_t n = 0;
            while (g_gob_err[n]) n++;
            stack[sp++] = gob_make_str_from((const uint8_t*)g_gob_err, (uint32_t)n);
            continue;
        }
        if (op == GBC_MAP_NEW) {
            int id;
            uint32_t off;
            id = gob_obj_alloc(GBC_OBJ_MAP);
            if (id < 0) { stack[sp++] = 0; continue; }
            if (gob_heap_alloc_words(16, &off) != 0) { /* 8 pairs */
                g_objs[id].used = 0;
                stack[sp++] = 0;
                continue;
            }
            g_objs[id].off = off;
            g_objs[id].cap = 8;
            g_objs[id].len = 0;
            stack[sp++] = GBC_MAKE_OBJ(id);
            continue;
        }
        if (op == GBC_DOS_RUN) {
            int32_t pathv;
            uint32_t plen = 0;
            const char* path;
            char buf[96];
            uint32_t i;
            if (sp < 1) break;
            pathv = stack[--sp];
            path = gob_str_cstr(pathv, &plen);
            if (!path || plen == 0) {
                gob_set_err("dos_run needs path string");
                stack[sp++] = 0;
                continue;
            }
            if (plen >= sizeof(buf)) plen = sizeof(buf) - 1;
            for (i = 0; i < plen; i++) buf[i] = path[i];
            buf[plen] = '\0';
            stack[sp++] = (dos_exec(buf) == 0) ? 1 : 0;
            continue;
        }
        if (op == GBC_KEY_POLL) {
            int k = 0;
            gob_win_t* gw = gob_active_win();
            process_set_state(pid, PROC_STATE_READY);
            vdesk_pump_one_frame();
            process_set_state(pid, PROC_STATE_RUNNING);
            if (gw && gw->pending_key) {
                k = gw->pending_key;
                gw->pending_key = 0;
            } else if (keyboard_has_char()) {
                k = (unsigned char)keyboard_read_char();
            }
            if (sp >= GBC_OP_STACK) break;
            stack[sp++] = k;
            continue;
        }
        if (op == GBC_GUI_CLEAR) {
            /* Drop ephemeral strings from the last frame so board redraws
             * do not exhaust the object table (looks like a hard freeze). */
            gob_win_t* gw = gob_active_win();
            gob_gc(globals, stack, sp);
            if (gw) {
                gw->line_count = 0;
                gw->cmd_count = 0;
                gw->canvas = 0;
                gw->interactive = 1;
                gob_win_dirty(gw);
            }
            continue;
        }
        if (op == GBC_GUI_TEXT_S) {
            int32_t sv;
            uint32_t slen = 0;
            const char* s;
            gob_win_t* gw = gob_active_win();
            if (sp < 1) break;
            sv = stack[--sp];
            s = gob_str_cstr(sv, &slen);
            if (gw && gw->line_count < GOB_WIN_LINES && s) {
                size_t n = 0;
                char* dst = gw->lines[gw->line_count];
                gw->canvas = 0;
                if (slen >= GOB_WIN_COLS) slen = GOB_WIN_COLS - 1;
                while (n < slen) {
                    dst[n] = s[n];
                    n++;
                }
                dst[n] = '\0';
                gw->line_count++;
                gw->interactive = 1;
                gob_win_dirty(gw);
            }
            continue;
        }
        if (op == GBC_GFX_FILL) {
            int32_t rgb;
            gob_win_t* gw = gob_active_win();
            if (sp < 1) break;
            rgb = stack[--sp];
            gob_gc(globals, stack, sp);
            if (gw) {
                gw->canvas = 1;
                gw->canvas_bg = (uint32_t)rgb & 0x00FFFFFFu;
                gw->cmd_count = 0;
                gw->line_count = 0;
                gw->interactive = 1;
                gob_win_dirty(gw);
            }
            continue;
        }
        if (op == GBC_GFX_RECT) {
            int32_t rgb, h, w, y, x;
            gob_win_t* gw = gob_active_win();
            if (sp < 5) break;
            rgb = stack[--sp];
            h = stack[--sp];
            w = stack[--sp];
            y = stack[--sp];
            x = stack[--sp];
            if (gw && gw->cmd_count < GOB_GFX_MAX_CMDS) {
                gob_gfx_cmd_t* c = &gw->cmds[gw->cmd_count++];
                gw->canvas = 1;
                gw->interactive = 1;
                c->op = GOB_GFX_OP_RECT;
                c->x = (int16_t)x;
                c->y = (int16_t)y;
                c->w = (int16_t)w;
                c->h = (int16_t)h;
                c->c0 = (uint32_t)rgb & 0x00FFFFFFu;
                c->c1 = 0;
                c->text[0] = '\0';
            }
            continue;
        }
        if (op == GBC_GFX_LABEL) {
            int32_t bg, fg, y, x, sv;
            uint32_t slen = 0;
            const char* s;
            gob_win_t* gw = gob_active_win();
            if (sp < 5) break;
            /* Push order is x,y,str,fg,bg */
            bg = stack[--sp];
            fg = stack[--sp];
            sv = stack[--sp];
            y = stack[--sp];
            x = stack[--sp];
            s = gob_str_cstr(sv, &slen);
            if (gw && gw->cmd_count < GOB_GFX_MAX_CMDS && s) {
                gob_gfx_cmd_t* c = &gw->cmds[gw->cmd_count++];
                size_t n = 0;
                gw->canvas = 1;
                gw->interactive = 1;
                c->op = GOB_GFX_OP_LABEL;
                c->x = (int16_t)x;
                c->y = (int16_t)y;
                c->w = 0;
                c->h = 0;
                c->c0 = (uint32_t)fg & 0x00FFFFFFu;
                c->c1 = (uint32_t)bg & 0x00FFFFFFu;
                if (slen >= sizeof(c->text)) slen = (uint32_t)sizeof(c->text) - 1u;
                while (n < slen) {
                    c->text[n] = s[n];
                    n++;
                }
                c->text[n] = '\0';
            }
            continue;
        }
        if (op == GBC_GFX_PRESENT) {
            gob_win_t* gw = gob_active_win();
            if (gw) {
                gw->canvas = 1;
                gw->interactive = 1;
                gob_win_dirty(gw);
            }
            continue;
        }
        if (op == GBC_NUM) {
            int32_t sv;
            uint32_t slen = 0;
            const char* s;
            int32_t n = 0;
            int neg = 0;
            uint32_t i = 0;
            if (sp < 1) break;
            sv = stack[--sp];
            s = gob_str_cstr(sv, &slen);
            if (!s) { stack[sp++] = 0; continue; }
            if (i < slen && s[i] == '-') { neg = 1; i++; }
            while (i < slen && s[i] >= '0' && s[i] <= '9') {
                n = n * 10 + (int32_t)(s[i] - '0');
                i++;
            }
            stack[sp++] = neg ? -n : n;
            continue;
        }
        if (op == GBC_STR_I) {
            int32_t v;
            char buf[16];
            char rev[16];
            int n = 0, i = 0, neg = 0;
            uint32_t u;
            if (sp < 1) break;
            v = stack[--sp];
            if (v == (int32_t)0x80000000) {
                stack[sp++] = gob_make_str_from((const uint8_t*)"-2147483648", 11);
                continue;
            }
            if (v < 0) { neg = 1; u = (uint32_t)(-v); }
            else u = (uint32_t)v;
            if (u == 0) rev[n++] = '0';
            while (u > 0 && n < 14) {
                rev[n++] = (char)('0' + (u % 10u));
                u /= 10u;
            }
            if (neg) buf[i++] = '-';
            while (n > 0) buf[i++] = rev[--n];
            buf[i] = '\0';
            stack[sp++] = gob_make_str_from((const uint8_t*)buf, (uint32_t)i);
            continue;
        }
        if (op == GBC_GUI_CLOSED) {
            int closed = 0;
            gob_win_t* gw = gob_active_win();
            if (!gw || gw->closed) closed = 1;
            else if (gw->win && !gw->win->visible) closed = 1;
            if (sp >= GBC_OP_STACK) break;
            stack[sp++] = closed;
            continue;
        }
        gob_set_err("unknown opcode");
        print("gob: runtime error: ");
        print(g_gob_err[0] ? g_gob_err : "unknown opcode");
        print("\n");
        break;
    }

    /* Close Gob windows so the next game can own slot 0 cleanly. */
    gob_wins_cleanup();
    g_current_gob_pid = -1;
    process_set_state(pid, PROC_STATE_ZOMBIE);
    return 0;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    (void)a3;
    switch ((uint32_t)num) {
    case SYS_EXIT:
        if (g_current_gob_pid > 0)
            terminate_process(g_current_gob_pid);
        return 0;
    case SYS_WRITE:
        if (a0 == 1 && a1 && a2) {
            char tmp[128];
            size_t n = (size_t)a2 < sizeof(tmp) - 1 ? (size_t)a2 : sizeof(tmp) - 1;
            memcpy(tmp, (const void*)(uintptr_t)a1, n);
            tmp[n] = '\0';
            print(tmp);
            return (uint64_t)n;
        }
        return (uint64_t)-1;
    case SYS_YIELD:
        vdesk_pump_one_frame();
        return 0;
    default:
        return (uint64_t)-1;
    }
}

void userspace_init(void) {
    static uint8_t kstack[8192];
#ifdef __x86_64__
    gdt64_reload();
    gdt64_set_tss_rsp0((uint64_t)(uintptr_t)(kstack + sizeof(kstack)));
    x64_idt_set_user_gate(0x80, syscall80_stub);
#endif
    g_userspace_ready = 1;
    gob_seed_game_apps();
    print("userspace: ring-3 GDT/TSS + int 0x80 ready\n");
}

void userspace_on_timer(void) {
    /* Reserved for preemptive scheduling. */
}

int gob_exec(const char* path) {
    FileHandle* fh;
    uint8_t* file;
    size_t cap = 65536;
    size_t total = 0;
    size_t n;
    gob_header_t* hdr;
    char name[16];
    int pid;
    const uint8_t* code;
    const uint8_t* rodata;
    Directory* restore_dir;

    if (!path || !path[0]) return -1;
    if (!g_userspace_ready) userspace_init();

    restore_dir = fs_get_cwd_dir();

    /* Support Apps/Welcome.gob style paths via cd */
    {
        char dir[64];
        char base[32];
        const char* slash = 0;
        const char* p = path;
        size_t di = 0, bi = 0;
        while (*p) {
            if (*p == '/') slash = p;
            p++;
        }
        if (slash) {
            while (path + di < slash && di + 1 < sizeof(dir)) {
                dir[di] = path[di];
                di++;
            }
            dir[di] = '\0';
            slash++;
            while (slash[bi] && bi + 1 < sizeof(base)) {
                base[bi] = slash[bi];
                bi++;
            }
            base[bi] = '\0';
            if (dir[0] && fs_change_dir(dir) != 0) {
                /* try from root */
                while (fs_cd_up() == 0) { }
                if (fs_change_dir(dir) != 0) {
                    print("gob: cannot open directory\n");
                    if (restore_dir) fs_set_current_dir(restore_dir);
                    return -1;
                }
            }
            fh = fs_open(base);
        } else {
            fh = fs_open(path);
        }
    }

    if (!fh) {
        print("gob: file not found: ");
        print(path);
        print("\n");
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }

    file = (uint8_t*)kmalloc(cap);
    if (!file) {
        fs_close(fh);
        return -1;
    }
    while ((n = fs_read(fh, file + total, cap - total)) > 0) {
        total += n;
        if (total >= cap) break;
    }
    fs_close(fh);

    if (total < sizeof(gob_header_t)) {
        print("gob: truncated header\n");
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }

    hdr = (gob_header_t*)file;
    if (hdr->magic != GOB_MAGIC) {
        print("gob: bad magic (not a .gob file)\n");
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }
    if (hdr->version != GOB_VERSION) {
        print("gob: unsupported version (need ");
        {
            char vb[8];
            vb[0] = (char)('0' + (GOB_VERSION % 10));
            vb[1] = '\0';
            print(vb);
        }
        print(", got ");
        {
            char vb[8];
            uint16_t v = hdr->version;
            if (v >= 10) {
                vb[0] = (char)('0' + (v / 10) % 10);
                vb[1] = (char)('0' + (v % 10));
                vb[2] = '\0';
            } else {
                vb[0] = (char)('0' + (v % 10));
                vb[1] = '\0';
            }
            print(vb);
        }
        print("). Recompile with gooberc.\n");
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }
    if (!(hdr->flags & GOB_FLAG_BYTECODE)) {
        print("gob: only bytecode .gob supported\n");
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }
    if (sizeof(gob_header_t) + hdr->code_size + hdr->rodata_size > total) {
        print("gob: size mismatch\n");
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }

    basename_copy(name, sizeof(name), path);
    pid = create_process_ex(name, (total + 1023) / 1024, PROC_KIND_GOB);
    if (pid < 0) {
        kfree(file);
        if (restore_dir) fs_set_current_dir(restore_dir);
        return -1;
    }

    code = file + sizeof(gob_header_t);
    rodata = code + hdr->code_size;
    print("gob: running ");
    print(name);
    print("\n");
    gob_run_bytecode(code, hdr->code_size, rodata, hdr->rodata_size, hdr->entry, pid);
    terminate_process(pid);
    kfree(file);
    /* Restore caller's cwd (do not walk to volume root). */
    if (restore_dir) fs_set_current_dir(restore_dir);
    return 0;
}
