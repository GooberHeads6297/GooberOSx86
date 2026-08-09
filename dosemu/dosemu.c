#include "dosemu.h"
#include "dosemu_priv.h"
#include "../lib/memory.h"
#include "../lib/string.h"
#include "../taskmgr/process.h"
#include "../fs/filesystem.h"
#include "../drivers/keyboard/keyboard.h"

extern void print(const char*);

dos_session_t g_dos_session;

/* Embedded HELLO.COM smoke fixture (org 100h). */
static const uint8_t k_hello_com[] = {
    0xB4, 0x09,             /* mov ah, 9 */
    0xBA, 0x0B, 0x01,       /* mov dx, 10Bh */
    0xCD, 0x21,             /* int 21h */
    0xB4, 0x4C,             /* mov ah, 4Ch */
    0xCD, 0x21,             /* int 21h */
    /* 10B: "Hello from GooberDOS!$" */
    'H','e','l','l','o',' ','f','r','o','m',' ',
    'G','o','o','b','e','r','D','O','S','!','$'
};

/* Tiny VER.COM: AH=30h then print major as digit + CR/LF via AH=02 */
static const uint8_t k_ver_com[] = {
    0xB4, 0x30,             /* mov ah, 30h */
    0xCD, 0x21,             /* int 21h */
    0x04, 0x30,             /* add al, '0' */
    0x8A, 0xD0,             /* mov dl, al */
    0xB4, 0x02,             /* mov ah, 2 */
    0xCD, 0x21,             /* int 21h */
    0xB2, 0x0D,             /* mov dl, 0Dh */
    0xB4, 0x02,
    0xCD, 0x21,
    0xB2, 0x0A,             /* mov dl, 0Ah */
    0xB4, 0x02,
    0xCD, 0x21,
    0xB4, 0x4C,             /* mov ah, 4Ch */
    0xCD, 0x21
};

const uint8_t* dos_hello_com_bytes(size_t* out_len) {
    if (out_len) *out_len = sizeof(k_hello_com);
    return k_hello_com;
}

FileHandle* dos_open_host_path(const char* path) {
    Directory* restore;
    FileHandle* fh;
    char dir[96];
    char base[48];
    const char* slash = NULL;
    const char* p;
    size_t di = 0, bi = 0;

    if (!path || !path[0]) return NULL;
    restore = fs_get_cwd_dir();
    p = path;
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
        {
            Directory* root = fs_get_cwd_dir();
            while (root && root->parent) root = root->parent;
            if (root) fs_set_current_dir(root);
        }
        if (dir[0]) {
            const char* seg = dir;
            char part[32];
            while (*seg == '/') seg++;
            while (*seg) {
                size_t n = 0;
                while (seg[n] && seg[n] != '/' && n + 1 < sizeof(part)) {
                    part[n] = seg[n];
                    n++;
                }
                part[n] = '\0';
                if (part[0] && fs_change_dir(part) != 0) {
                    if (restore) fs_set_current_dir(restore);
                    return NULL;
                }
                seg += n;
                while (*seg == '/') seg++;
            }
        }
        fh = fs_open(base);
    } else {
        fh = fs_open(path);
    }
    if (restore) fs_set_current_dir(restore);
    return fh;
}

static void dos_session_close(dos_session_t* s) {
    int i;
    int pid;
    if (!s || !s->used) return;
    for (i = 0; i < DOS_MAX_FILES; i++) {
        if (s->files[i].used && s->files[i].fh) {
            fs_close(s->files[i].fh);
            s->files[i].fh = NULL;
        }
        s->files[i].used = 0;
    }
    pid = s->pid;
    s->pid = -1;
    if (s->win) {
        s->win->user_data = NULL;
        s->win->tick_handler = NULL;
        s->win->key_handler = NULL;
        s->win->render = NULL;
        s->win->process_pid = -1;
        s->win = NULL;
    }
    dos_mem_free(s);
    s->used = 0;
    s->halted = 1;
    if (pid > 0) terminate_process(pid);
}

static void dos_win_render(VWindow* win, int cx, int cy, int cw, int ch) {
    dos_session_t* s = (dos_session_t*)win->user_data;
    char status[64];
    const char* cyc;
    int i = 0;
    if (!s || !s->used) {
        vdesk_draw_rect(cx, cy, cw, ch, 0x000000);
        vdesk_draw_text(cx + 8, cy + 8, "GooberDOS (idle)", 0xAAAAAA, 0x000000);
        return;
    }
    dos_video_render(s, cx, cy, cw, ch);
    /* Status: Cycles / S|R / vmode / PE / CS:IP — so blank ≠ mystery hang */
    cyc = dos_cycles_label(s);
    status[i++] = 'C'; status[i++] = ':';
    while (*cyc && i + 1 < (int)sizeof(status) - 28) status[i++] = *cyc++;
    status[i++] = ' ';
    status[i++] = s->at_shell ? 'S' : 'R';
    status[i++] = ' ';
    status[i++] = 'm';
    {
        const char* hex = "0123456789ABCDEF";
        uint8_t m = (uint8_t)(s->video_mode & 0xFF);
        status[i++] = hex[(m >> 4) & 0xF];
        status[i++] = hex[m & 0xF];
    }
    if (s->pe) { status[i++] = ' '; status[i++] = 'P'; status[i++] = 'E'; }
    if (!s->at_shell) {
        const char* hex = "0123456789ABCDEF";
        uint16_t cs = s->cpu.cs, ip = s->cpu.ip;
        int b;
        status[i++] = ' ';
        for (b = 12; b >= 0; b -= 4) status[i++] = hex[(cs >> b) & 0xF];
        status[i++] = ':';
        for (b = 12; b >= 0; b -= 4) status[i++] = hex[(ip >> b) & 0xF];
    }
    status[i] = '\0';
    vdesk_draw_rect(cx, cy + ch - 16, cw, 16, 0x202020);
    vdesk_draw_text(cx + 4, cy + ch - 14, status, 0x00FFAA, 0x202020);
}

static void dos_win_key(VWindow* win, char key) {
    dos_session_t* s = (dos_session_t*)win->user_data;
    if (!s || !s->used) return;
    if (s->at_shell) {
        (void)dos_shell_on_key(s, key);
        if (s->shell_want_close && s->win) {
            VWindow* w = s->win;
            vdesk_close_window(w);
        }
        return;
    }
    /* Ctrl+C aborts guest back to shell */
    if ((unsigned char)key == 0x03) {
        s->return_code = 0xFF;
        s->halted = 1;
        s->shell_reentry = 1;
        return;
    }
    if ((unsigned char)key == KEY_F11) { dos_cycles_adjust(s, -1); return; }
    if ((unsigned char)key == KEY_F12) { dos_cycles_adjust(s, 1); return; }
    dos_key_push(s, key);
}

static void dos_win_tick(VWindow* win) {
    dos_session_t* s = (dos_session_t*)win->user_data;
    int n;
    uint32_t budget;
    if (!s || !s->used) return;

    if (s->shell_reentry || (s->halted && !s->at_shell)) {
        dos_shell_on_guest_exit(s);
        vdesk_mark_dirty(win->x, win->y, win->width, win->height);
        return;
    }

    dos_mouse_update(s);
    s->steps_since_tick++;
    if (s->steps_since_tick >= 2) {
        s->steps_since_tick = 0;
        dos_timer_tick(s);
        if (!s->at_shell) {
            /* IRQ0 → INT 08 (default IRET) then user INT 1Ch hook if any */
            if (dos_ivt_is_default(s, 0x08)) {
                /* no guest handler — still fire 1Ch for DOS-style timers */
            } else {
                dos_soft_int(s, 0x08);
            }
            dos_soft_int(s, 0x1C);
        }
    }

    if (s->at_shell) {
        vdesk_mark_dirty(win->x, win->y, win->width, win->height);
        return;
    }

    budget = s->cycles_per_tick ? s->cycles_per_tick : 8000u;
    for (n = 0; n < (int)budget && !s->halted && !s->at_shell; n++) {
        int ok;
        if (s->pe && s->cpu32) ok = cpu386_step(s);
        else ok = cpu8086_step(s);
        if (!ok) break;
        s->steps_total++;
    }
    vdesk_mark_dirty(win->x, win->y, win->width, win->height);
}

static int dos_load_com(dos_session_t* s, const uint8_t* data, size_t len,
                        const char* cmdline) {
    uint32_t base;
    size_t i;
    if (!s || !data || len == 0 || len > 0xFE00) return -1;
    dos_setup_ivt(s);
    s->psp_seg = DOS_PSP_SEG;
    s->env_seg = DOS_ENV_SEG;
    dos_setup_psp(s, cmdline);
    base = dos_seg_off(s->psp_seg, DOS_COM_IP);
    for (i = 0; i < len; i++)
        dos_write8(s, base + (uint32_t)i, data[i]);
    cpu8086_reset(&s->cpu, s->psp_seg, DOS_COM_IP, s->psp_seg, 0xFFFE);
    s->cpu.ds = s->psp_seg;
    s->cpu.es = s->psp_seg;
    dos_write16(s, dos_seg_off(s->cpu.ss, s->cpu.sp), 0);
    /* Free conventional memory from DOS_LOAD_SEG upward */
    dos_mcb_init(s, DOS_LOAD_SEG, DOS_MEM_TOP_SEG);
    return 0;
}

static int dos_load_exe(dos_session_t* s, const uint8_t* data, size_t len,
                        const char* cmdline) {
    uint16_t header_para, pages, bytes_last;
    uint16_t ss, sp, cs, ip, reloc_items, reloc_tbl;
    uint16_t min_alloc, max_alloc;
    uint32_t image_size, i;
    uint16_t psp, load_seg, block_paras, img_paras, min_need;
    uint16_t mcb, free_sz;
    uint32_t hdr, copy, dst;
    if (!s || len < 28) return -1;
    if (!(data[0] == 'M' || data[0] == 'Z') || !(data[1] == 'Z' || data[1] == 'M'))
        return -1;
    bytes_last = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    pages = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    reloc_items = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    header_para = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
    min_alloc = (uint16_t)data[10] | ((uint16_t)data[11] << 8);
    max_alloc = (uint16_t)data[12] | ((uint16_t)data[13] << 8);
    ss = (uint16_t)data[14] | ((uint16_t)data[15] << 8);
    sp = (uint16_t)data[16] | ((uint16_t)data[17] << 8);
    ip = (uint16_t)data[20] | ((uint16_t)data[21] << 8);
    cs = (uint16_t)data[22] | ((uint16_t)data[23] << 8);
    reloc_tbl = (uint16_t)data[24] | ((uint16_t)data[25] << 8);

    image_size = (uint32_t)pages * 512u;
    if (bytes_last) image_size = image_size - 512u + bytes_last;
    if (image_size > len) image_size = (uint32_t)len;

    hdr = (uint32_t)header_para * 16u;
    copy = image_size > hdr ? image_size - hdr : 0;
    if (hdr + copy > len) return -1;
    img_paras = (uint16_t)((copy + 15u) >> 4);
    /* PSP (16 paras) + image + e_minalloc — DOS then often AH=4A-shrinks */
    min_need = (uint16_t)(0x10u + img_paras + min_alloc);
    if (min_need < 0x11u) return -1;

    dos_setup_ivt(s);
    /* Conventional arena: [MCB][PSP|image|…]  (env stays below at DOS_ENV_SEG) */
    dos_mcb_init(s, DOS_LOAD_SEG, DOS_MEM_TOP_SEG);
    mcb = s->mcb_first;
    free_sz = dos_read16(s, dos_seg_off(mcb, 3));
    if (free_sz < min_need) return -1;

    block_paras = free_sz;
    if (max_alloc != 0xFFFFu) {
        uint32_t want = 0x10u + (uint32_t)img_paras + (uint32_t)max_alloc;
        if (want < min_need) want = min_need;
        if (want < block_paras) block_paras = (uint16_t)want;
    }

    if (block_paras < free_sz) {
        uint16_t rem = (uint16_t)(free_sz - block_paras - 1u);
        uint16_t nseg = (uint16_t)(mcb + block_paras + 1u);
        dos_write8(s, dos_seg_off(mcb, 0), 'M');
        dos_write16(s, dos_seg_off(mcb, 1), 0); /* owner patched after PSP known */
        dos_write16(s, dos_seg_off(mcb, 3), block_paras);
        dos_write8(s, dos_seg_off(nseg, 0), 'Z');
        dos_write16(s, dos_seg_off(nseg, 1), 0);
        dos_write16(s, dos_seg_off(nseg, 3), rem);
    } else {
        dos_write8(s, dos_seg_off(mcb, 0), 'Z');
        dos_write16(s, dos_seg_off(mcb, 3), block_paras);
    }

    psp = (uint16_t)(mcb + 1u);
    load_seg = (uint16_t)(psp + 0x10u);
    s->psp_seg = psp;
    s->env_seg = DOS_ENV_SEG;
    dos_setup_psp(s, cmdline);
    dos_write16(s, dos_seg_off(mcb, 1), psp);
    /* First paragraph past the allocation (exclusive) */
    dos_write16(s, dos_seg_off(psp, 2), (uint16_t)(psp + block_paras));

    dst = (uint32_t)load_seg << 4;
    if (dst + copy >= DOS_MEM_SIZE) return -1;
    for (i = 0; i < copy; i++)
        dos_write8(s, dst + i, data[hdr + i]);
    for (i = 0; i < reloc_items; i++) {
        uint32_t off = (uint32_t)reloc_tbl + i * 4u;
        uint16_t roff, rseg;
        uint32_t patch;
        uint16_t val;
        if (off + 4 > len) break;
        roff = (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
        rseg = (uint16_t)data[off + 2] | ((uint16_t)data[off + 3] << 8);
        patch = ((uint32_t)(load_seg + rseg) << 4) + roff;
        if (patch + 1 >= DOS_MEM_SIZE) break;
        val = dos_read16(s, patch);
        dos_write16(s, patch, (uint16_t)(val + load_seg));
    }

    cpu8086_reset(&s->cpu,
                  (uint16_t)(load_seg + cs), ip,
                  (uint16_t)(load_seg + ss), sp ? sp : 0xFFFE);
    s->cpu.ds = psp;
    s->cpu.es = psp;
    return 0;
}

/* DOS/16M probes: CPU class at DS:[2F], AT/PS2 check if class==2.
 * Patch hard-fail paths so our soft 386/AT identity is accepted. */
static void dos_patch_dos16m_cpu_check(dos_session_t* s) {
    uint32_t a;
    if (!s || !s->mem) return;
    for (a = 0; a + 10 < 0xA0000u; a++) {
        /* cmp [2F],3 / jge → jmp : skip error [15] "not a 386" */
        if (dos_read8(s, a) == 0x80 && dos_read8(s, a + 1) == 0x3E &&
            dos_read8(s, a + 2) == 0x2F && dos_read8(s, a + 3) == 0x00 &&
            dos_read8(s, a + 4) == 0x03 && dos_read8(s, a + 5) == 0x7D &&
            dos_read8(s, a + 7) == 0xB8 && dos_read8(s, a + 8) == 0x0F &&
            dos_read8(s, a + 9) == 0x00) {
            dos_write8(s, a + 5, 0xEB);
        }
        /* cmp [2F],2 / jne → jmp : skip error [19] AT/PS2 check */
        if (dos_read8(s, a) == 0x80 && dos_read8(s, a + 1) == 0x3E &&
            dos_read8(s, a + 2) == 0x2F && dos_read8(s, a + 3) == 0x00 &&
            dos_read8(s, a + 4) == 0x02 && dos_read8(s, a + 5) == 0x75 &&
            dos_read8(s, a + 7) == 0xE8) {
            dos_write8(s, a + 5, 0xEB);
        }
        /* CPU identify prologue → always return 3 (386) */
        if (dos_read8(s, a) == 0x9C && dos_read8(s, a + 1) == 0x33 &&
            dos_read8(s, a + 2) == 0xC0 && dos_read8(s, a + 3) == 0x50 &&
            dos_read8(s, a + 4) == 0x9D && dos_read8(s, a + 5) == 0x9C &&
            dos_read8(s, a + 6) == 0x58 && dos_read8(s, a + 7) == 0x80 &&
            dos_read8(s, a + 8) == 0xE4 && dos_read8(s, a + 9) == 0xF0) {
            dos_write8(s, a + 0, 0xB8); /* MOV AX, 3 */
            dos_write8(s, a + 1, 0x03);
            dos_write8(s, a + 2, 0x00);
            dos_write8(s, a + 3, 0xC3); /* RET */
        }
        /* 486/CPUID probe entry (pushfd; pushfd; pop edx…) → return 3 */
        if (dos_read8(s, a) == 0x66 && dos_read8(s, a + 1) == 0x9C &&
            dos_read8(s, a + 2) == 0x66 && dos_read8(s, a + 3) == 0x9C &&
            dos_read8(s, a + 4) == 0x66 && dos_read8(s, a + 5) == 0x5A &&
            dos_read8(s, a + 6) == 0x66 && dos_read8(s, a + 7) == 0x8B &&
            dos_read8(s, a + 8) == 0xCA) {
            dos_write8(s, a + 0, 0xB8);
            dos_write8(s, a + 1, 0x03);
            dos_write8(s, a + 2, 0x00);
            dos_write8(s, a + 3, 0xC3);
        }
    }
}

int dos_load_program_bytes(dos_session_t* s, const uint8_t* data, size_t len,
                           const char* cmdline, int is_exe) {
    int i;
    char saved_cwd[96];
    if (!s || !data || !len) return -1;
    /* preserve shell cwd across load */
    strncpy(saved_cwd, s->guest_cwd, sizeof(saved_cwd) - 1);
    saved_cwd[sizeof(saved_cwd) - 1] = '\0';
    for (i = 3; i < DOS_MAX_FILES; i++) {
        if (s->files[i].used && s->files[i].fh) {
            fs_close(s->files[i].fh);
            s->files[i].fh = NULL;
        }
        s->files[i].used = 0;
    }
    if (is_exe) {
        if (dos_load_exe(s, data, len, cmdline ? cmdline : "") != 0) return -1;
    } else {
        if (dos_load_com(s, data, len, cmdline ? cmdline : "") != 0) return -1;
    }
    strncpy(s->guest_cwd, saved_cwd, sizeof(s->guest_cwd) - 1);
    s->guest_cwd[sizeof(s->guest_cwd) - 1] = '\0';
    s->halted = 0;
    s->shell_reentry = 0;
    s->pe = 0;
    s->cpu32 = 0;
    pm_set_cr0(s, 0x10);
    pm_sync_real_segs(s);
    /* Advertise IOPL=3 in FLAGS so DOS/16M's 286-vs-386 probe sees a 386. */
    s->cpu.flags = (uint16_t)((s->cpu.flags & 0x0FFFu) | 0x3002u);
    s->eflags = (s->eflags & 0xFFFF0000u) | s->cpu.flags;
    if (is_exe) dos_patch_dos16m_cpu_check(s);
    return 0;
}

int dos_session_active(void) {
    return g_dos_session.used;
}

static int dos_session_open_window(dos_session_t* s) {
    VWindow* win;
    int pid;
    pid = create_process_ex("GooberDOS", 1024, PROC_KIND_DOS);
    if (pid < 0) return -1;
    s->pid = pid;
    process_set_state(pid, PROC_STATE_RUNNING);
    win = vdesk_create_window("GooberDOS", 64, 48, 720, 480);
    if (!win) {
        terminate_process(pid);
        s->pid = -1;
        return -1;
    }
    win->user_data = s;
    win->render = dos_win_render;
    win->key_handler = dos_win_key;
    win->tick_handler = dos_win_tick;
    win->process_pid = pid;
    s->win = win;
    return 0;
}

int dos_exec(const char* path) {
    dos_session_t* s = &g_dos_session;
    int auto_run = (path && path[0]);

    if (s->used) {
        if (s->win) vdesk_close_window(s->win);
        dos_session_close(s);
    }
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->pid = -1;
    if (auto_run) strncpy(s->path, path, sizeof(s->path) - 1);

    if (dos_mem_alloc(s) != 0) {
        print("dos: out of memory\n");
        s->used = 0;
        return -1;
    }
    dos_video_init(s);
    dos_setup_ivt(s);
    dos_setup_psp(s, "");
    dos_mcb_init(s, DOS_LOAD_SEG, DOS_MEM_TOP_SEG);
    s->files[0].used = 1; s->files[0].is_std = 1;
    s->files[1].used = 1; s->files[1].is_std = 1;
    s->files[2].used = 1; s->files[2].is_std = 1;
    s->bios_ticks = 0;
    s->find_index = -1;
    s->mouse_min_x = 0; s->mouse_max_x = 639;
    s->mouse_min_y = 0; s->mouse_max_y = 199;
    s->mouse_x = 320; s->mouse_y = 100;
    s->cycles_preset = 3;
    s->cycles_per_tick = 8000;
    pm_init_session(s);
    dpmi_init(s);
    s->fs = 0;
    s->gs = 0;

    if (dos_session_open_window(s) != 0) {
        dos_session_close(s);
        return -1;
    }

    dos_shell_boot(s);

    if (auto_run) {
        /* Map host path Dos/Apps/FOO.COM -> guest Apps\FOO.COM */
        char guest[96];
        const char* p = path;
        size_t i = 0;
        if ((p[0] == 'D' || p[0] == 'd') &&
            (p[1] == 'o' || p[1] == 'O') &&
            (p[2] == 's' || p[2] == 'S') && p[3] == '/') {
            p += 4;
        }
        while (*p && i + 1 < sizeof(guest)) {
            guest[i++] = (*p == '/') ? '\\' : *p;
            p++;
        }
        guest[i] = '\0';
        dos_video_puts(s, "Launching ");
        dos_video_puts(s, guest);
        dos_video_puts(s, "\r\n");
        if (dos_shell_launch(s, guest, "") != 0) {
            /* stay at shell with error already printed */
        }
    }

    vdesk_notify("GooberDOS", auto_run ? "DOS program" : "DOS session");
    vdesk_mark_full_dirty();
    return 0;
}

void dos_on_window_closed(struct VWindow* win) {
    dos_session_t* s = &g_dos_session;
    if (s->used && (VWindow*)win == s->win) {
        s->win = NULL;
        dos_session_close(s);
    }
}

void dos_seed_share(void) {
    Directory* restore = fs_get_cwd_dir();
    Directory* root = restore;
    Directory* dos;
    Directory* apps;
    FileHandle* fh;
    size_t n = 0;
    const uint8_t* hello = dos_hello_com_bytes(&n);

    while (root && root->parent) root = root->parent;
    if (!root) return;
    fs_set_current_dir(root);
    dos = fs_dir_find_child(root, "Dos");
    if (!dos) {
        if (fs_dir_create_dir(root, "Dos") != 0) {
            if (restore) fs_set_current_dir(restore);
            return;
        }
        dos = fs_dir_find_child(root, "Dos");
    }
    if (!dos) {
        if (restore) fs_set_current_dir(restore);
        return;
    }
    apps = fs_dir_find_child(dos, "Apps");
    if (!apps) {
        (void)fs_dir_create_dir(dos, "Apps");
        apps = fs_dir_find_child(dos, "Apps");
    }
    if (apps) {
        fh = fs_dir_open(apps, "HELLO.COM");
        if (fh) fs_close(fh);
        else (void)fs_dir_write(apps, "HELLO.COM", hello, n);
        fh = fs_dir_open(apps, "VER.COM");
        if (fh) fs_close(fh);
        else (void)fs_dir_write(apps, "VER.COM", k_ver_com, sizeof(k_ver_com));
    }
    if (restore) fs_set_current_dir(restore);
}
