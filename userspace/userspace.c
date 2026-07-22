#include "userspace.h"
#include "syscall.h"
#include "../fs/filesystem.h"
#include "../lib/memory.h"
#include "../lib/string.h"
#include "../taskmgr/process.h"
#include "../gui/vesa_window.h"

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
typedef struct {
    int used;
    VWindow* win;
    char lines[8][64];
    int line_count;
    int closed;
} gob_win_t;

static gob_win_t g_wins[GOB_MAX_WINS];

static void gob_win_render(VWindow* win, int cx, int cy, int cw, int ch) {
    gob_win_t* gw = (gob_win_t*)win->user_data;
    int i;
    vdesk_draw_rect(cx, cy, cw, ch, vdesk_shell_bg_color());
    if (!gw) return;
    for (i = 0; i < gw->line_count; i++) {
        vdesk_draw_text(cx + 12, cy + 12 + i * 18, gw->lines[i],
                        vdesk_shell_output_color(), vdesk_shell_bg_color());
    }
    vdesk_draw_text(cx + 12, cy + ch - 28, "[Click to close]",
                    vdesk_shell_muted_color(), vdesk_shell_bg_color());
}

static void gob_win_click(VWindow* win, int lx, int ly) {
    gob_win_t* gw = (gob_win_t*)win->user_data;
    (void)lx; (void)ly;
    if (!gw) return;
    gw->closed = 1;
    g_gui_wait_done = 1;
    vdesk_close_window(win);
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

static int gob_run_bytecode(const uint8_t* code, uint32_t code_size,
                            const uint8_t* rodata, uint32_t rodata_size,
                            uint32_t entry, int pid) {
    uint32_t ip = entry;
    int i;

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
            for (i = 0; i < GOB_MAX_WINS; i++) {
                if (!g_wins[i].used) { slot = i; break; }
            }
            if (slot >= 0) {
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
                g_wins[slot].win = vdesk_create_window(title, 120, 100,
                                                       w ? w : 420, h ? h : 240);
                if (g_wins[slot].win) {
                    g_wins[slot].win->user_data = &g_wins[slot];
                    g_wins[slot].win->render = gob_win_render;
                    g_wins[slot].win->click_handler = gob_win_click;
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
                g_wins[slot].line_count < 8 && str_off < rodata_size) {
                size_t n = 0;
                char* dst = g_wins[slot].lines[g_wins[slot].line_count];
                while (str_off + n < rodata_size && rodata[str_off + n] && n + 1 < 64) {
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
        break; /* unknown opcode */
    }

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

    if (!path || !path[0]) return -1;
    if (!g_userspace_ready) userspace_init();

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
        return -1;
    }

    hdr = (gob_header_t*)file;
    if (hdr->magic != GOB_MAGIC || hdr->version != GOB_VERSION) {
        print("gob: bad magic/version\n");
        kfree(file);
        return -1;
    }
    if (!(hdr->flags & GOB_FLAG_BYTECODE)) {
        print("gob: only bytecode .gob supported in v1\n");
        kfree(file);
        return -1;
    }
    if (sizeof(gob_header_t) + hdr->code_size + hdr->rodata_size > total) {
        print("gob: size mismatch\n");
        kfree(file);
        return -1;
    }

    basename_copy(name, sizeof(name), path);
    pid = create_process_ex(name, (total + 1023) / 1024, PROC_KIND_GOB);
    if (pid < 0) {
        kfree(file);
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
    /* Return to volume root after Apps/… exec */
    while (fs_cd_up() == 0) { }
    return 0;
}
