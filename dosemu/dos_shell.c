#include "dosemu.h"
#include "dosemu_priv.h"
#include "../lib/string.h"
#include "../lib/memory.h"
#include "../drivers/keyboard/keyboard.h"

static const uint32_t k_cycle_presets[] = {
    500u, 1500u, 4000u, 8000u, 16000u, 32000u, 64000u, 120000u
};
#define DOS_CYCLE_PRESET_COUNT ((int)(sizeof(k_cycle_presets) / sizeof(k_cycle_presets[0])))

void dos_cycles_adjust(dos_session_t* s, int delta) {
    if (!s) return;
    s->cycles_preset += delta;
    if (s->cycles_preset < 0) s->cycles_preset = 0;
    if (s->cycles_preset >= DOS_CYCLE_PRESET_COUNT)
        s->cycles_preset = DOS_CYCLE_PRESET_COUNT - 1;
    s->cycles_per_tick = k_cycle_presets[s->cycles_preset];
}

const char* dos_cycles_label(dos_session_t* s) {
    static char buf[24];
    uint32_t c = s ? s->cycles_per_tick : 0;
    /* simple decimal */
    char tmp[16];
    int i = 0, j = 0;
    if (c == 0) return "0";
    while (c && i < 15) { tmp[i++] = (char)('0' + (c % 10)); c /= 10; }
    while (i > 0 && j < 23) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

static int eq_cmd(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static void skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static void dos_shell_print_cwd(dos_session_t* s) {
    dos_video_puts(s, "C:");
    if (s->guest_cwd[0]) {
        size_t i;
        dos_video_putc(s, '\\');
        for (i = 0; s->guest_cwd[i]; i++) {
            char ch = s->guest_cwd[i];
            if (ch == '/') ch = '\\';
            dos_video_putc(s, ch);
        }
    } else {
        dos_video_putc(s, '\\');
    }
}

void dos_shell_show_prompt(dos_session_t* s) {
    if (!s) return;
    dos_shell_print_cwd(s);
    dos_video_puts(s, ">");
    s->shell_len = 0;
    s->shell_line[0] = '\0';
    s->shell_echo = 1;
}

void dos_shell_boot(dos_session_t* s) {
    if (!s) return;
    s->at_shell = 1;
    s->shell_reentry = 0;
    s->shell_want_close = 0;
    s->halted = 0;
    if (s->cycles_per_tick == 0) {
        s->cycles_preset = 3; /* 8000 */
        s->cycles_per_tick = k_cycle_presets[s->cycles_preset];
    }
    dos_video_set_mode(s, 3);
    dos_video_puts(s, "GooberDOS 0.2  (DOSBox-style session)\r\n");
    dos_video_puts(s, "Type HELP.  F11/F12 = cycles.  Ctrl+C aborts guest.\r\n");
    dos_video_puts(s, "Guest C:\\ maps to /Dos\r\n\r\n");
    dos_shell_show_prompt(s);
}

void dos_shell_on_guest_exit(dos_session_t* s) {
    char msg[48];
    int n;
    uint16_t code;
    if (!s) return;
    code = s->return_code;
    s->shell_reentry = 0;
    s->halted = 0;
    s->at_shell = 1;
    /* Leave protected mode so the status bar is not sticky PE at the shell. */
    s->pe = 0;
    s->cpu32 = 0;
    pm_set_cr0(s, 0x10);
    pm_sync_real_segs(s);
    if (s->video_mode == 0x13) dos_video_set_mode(s, 3);
    dos_video_puts(s, "\r\n");
    /* "Exit code N" */
    dos_video_puts(s, "Program exited (code ");
    n = 0;
    if (code >= 100) msg[n++] = (char)('0' + (code / 100) % 10);
    if (code >= 10) msg[n++] = (char)('0' + (code / 10) % 10);
    msg[n++] = (char)('0' + (code % 10));
    msg[n] = '\0';
    dos_video_puts(s, msg);
    dos_video_puts(s, ")\r\n");
    dos_shell_show_prompt(s);
}

static int name_ok_dos(const char* nm) {
    size_t i;
    if (!nm || !nm[0]) return 0;
    for (i = 0; nm[i] && i < 32; i++) {
        unsigned char c = (unsigned char)nm[i];
        if (c < 32 || c > 126) return 0;
        /* reject strings that look like corrupt FAT LFN debris */
        if (c == '?' || c == 0x7F) return 0;
    }
    return i > 0 && i < 32;
}

static void cmd_dir(dos_session_t* s) {
    char gdir[100];
    char hdir[160];
    Directory* dir;
    size_t i, k = 0;

    gdir[0] = '\\';
    while (s->guest_cwd[k] && k + 2 < sizeof(gdir)) {
        gdir[k + 1] = s->guest_cwd[k];
        k++;
    }
    gdir[k + 1] = '\0';
    if (dos_guest_to_host(gdir, hdir, sizeof(hdir)) != 0) {
        dos_video_puts(s, "Invalid directory\r\n");
        return;
    }
    dir = dos_resolve_host_dir(hdir);
    if (!dir) {
        dos_video_puts(s, "Directory not found\r\n");
        return;
    }
    fs_dir_refresh(dir);
    dos_video_puts(s, " Directory of ");
    dos_shell_print_cwd(s);
    dos_video_puts(s, "\r\n\r\n");
    for (i = 0; i < dir->child_count; i++) {
        if (!name_ok_dos(dir->children[i].name)) continue;
        dos_video_puts(s, dir->children[i].name);
        dos_video_puts(s, "    <DIR>\r\n");
    }
    for (i = 0; i < dir->file_count; i++) {
        if (!name_ok_dos(dir->files[i].name)) continue;
        dos_video_puts(s, dir->files[i].name);
        dos_video_puts(s, "\r\n");
    }
}

static void cmd_cd(dos_session_t* s, const char* arg) {
    char hpath[160];
    Directory* d;
    skip_ws(&arg);
    if (!arg[0]) {
        dos_shell_print_cwd(s);
        dos_video_puts(s, "\r\n");
        return;
    }
    if (eq_cmd(arg, "\\") || eq_cmd(arg, "/")) {
        s->guest_cwd[0] = '\0';
        return;
    }
    if (eq_cmd(arg, "..")) {
        char* slash = NULL;
        size_t i = 0;
        while (s->guest_cwd[i]) {
            if (s->guest_cwd[i] == '/' || s->guest_cwd[i] == '\\') slash = &s->guest_cwd[i];
            i++;
        }
        if (slash) *slash = '\0';
        else s->guest_cwd[0] = '\0';
        return;
    }
    if (dos_guest_to_host(arg, hpath, sizeof(hpath)) != 0) {
        dos_video_puts(s, "Invalid path\r\n");
        return;
    }
    d = dos_resolve_host_dir(hpath);
    if (!d) {
        dos_video_puts(s, "Directory not found\r\n");
        return;
    }
    {
        const char* p = hpath;
        size_t n = 0;
        if (p[0] == 'D' && p[1] == 'o' && p[2] == 's') {
            p += 3;
            if (*p == '/') p++;
        }
        while (*p && n + 1 < sizeof(s->guest_cwd)) s->guest_cwd[n++] = *p++;
        s->guest_cwd[n] = '\0';
    }
}

static void cmd_type(dos_session_t* s, const char* arg) {
    char hpath[160];
    FileHandle* fh;
    uint8_t buf[128];
    size_t n, i;
    skip_ws(&arg);
    if (!arg[0]) {
        dos_video_puts(s, "Required parameter missing\r\n");
        return;
    }
    if (dos_guest_to_host(arg, hpath, sizeof(hpath)) != 0) {
        dos_video_puts(s, "File not found\r\n");
        return;
    }
    fh = dos_open_host_path(hpath);
    if (!fh) {
        dos_video_puts(s, "File not found\r\n");
        return;
    }
    while ((n = fs_read(fh, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            char ch = (char)buf[i];
            if (ch == '\n') dos_video_putc(s, '\r');
            dos_video_putc(s, ch);
        }
    }
    fs_close(fh);
    dos_video_puts(s, "\r\n");
}

static void cmd_cls(dos_session_t* s) {
    dos_video_set_mode(s, 3);
}

static void cmd_help(dos_session_t* s) {
    dos_video_puts(s,
        "DIR CD TYPE CLS VER MEM HELP EXIT CYCLES\r\n"
        "Run programs: HELLO  or  APPS\\HELLO.COM\r\n"
        "F11/F12 change cycles  |  Ctrl+C abort guest\r\n");
}

static void cmd_ver(dos_session_t* s) {
    dos_video_puts(s, "GooberDOS version 5.00 (soft MS-DOS)\r\n");
}

static void cmd_mem(dos_session_t* s) {
    dos_video_puts(s, "655360 bytes conventional memory\r\n");
    if (DOS_EXT_KB) {
        char buf[48];
        unsigned kb = (unsigned)DOS_EXT_KB;
        int i = 0;
        dos_video_puts(s, "Extended memory: ");
        if (kb == 0) buf[i++] = '0';
        else {
            char tmp[16];
            int n = 0;
            while (kb) { tmp[n++] = (char)('0' + (kb % 10)); kb /= 10; }
            while (n--) buf[i++] = tmp[n];
        }
        buf[i] = '\0';
        dos_video_puts(s, buf);
        dos_video_puts(s, " KB\r\n");
    }
    dos_video_puts(s, "Cycles: ");
    dos_video_puts(s, dos_cycles_label(s));
    dos_video_puts(s, " / tick\r\n");
}

static void cmd_cycles(dos_session_t* s, const char* arg) {
    skip_ws(&arg);
    if (!arg[0]) {
        dos_video_puts(s, "Cycles=");
        dos_video_puts(s, dos_cycles_label(s));
        dos_video_puts(s, " (F11 slower, F12 faster)\r\n");
        return;
    }
    if (eq_cmd(arg, "MAX")) {
        s->cycles_preset = DOS_CYCLE_PRESET_COUNT - 1;
        s->cycles_per_tick = k_cycle_presets[s->cycles_preset];
    } else if (eq_cmd(arg, "UP")) {
        dos_cycles_adjust(s, 1);
    } else if (eq_cmd(arg, "DOWN")) {
        dos_cycles_adjust(s, -1);
    } else {
        /* parse decimal */
        uint32_t v = 0;
        while (*arg >= '0' && *arg <= '9') {
            v = v * 10u + (uint32_t)(*arg - '0');
            arg++;
        }
        if (v < 100) v = 100;
        if (v > 250000) v = 250000;
        s->cycles_per_tick = v;
        s->cycles_preset = 3;
    }
    dos_video_puts(s, "Cycles set to ");
    dos_video_puts(s, dos_cycles_label(s));
    dos_video_puts(s, "\r\n");
}

static int has_suffix_ci(const char* name, const char* suf) {
    size_t n = 0, s = 0, i;
    while (name[n]) n++;
    while (suf[s]) s++;
    if (n < s) return 0;
    for (i = 0; i < s; i++) {
        char a = name[n - s + i], b = suf[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

int dos_shell_launch(dos_session_t* s, const char* name, const char* args) {
    char try_paths[4][160];
    char hpath[160];
    FileHandle* fh = NULL;
    uint8_t* file = NULL;
    size_t cap = 1024 * 1024, total = 0, n;
    int is_exe = 0;
    int pi = 0;
    char guest[128];
    size_t gi = 0;

    if (!s || !name || !name[0]) return -1;

    /* Build guest-relative candidates */
    while (name[gi] && gi + 1 < sizeof(guest)) {
        guest[gi] = name[gi];
        gi++;
    }
    guest[gi] = '\0';

    try_paths[0][0] = '\0';
    if (dos_guest_to_host(guest, try_paths[0], sizeof(try_paths[0])) != 0)
        try_paths[0][0] = '\0';
    pi = 1;
    if (!has_suffix_ci(guest, ".COM") && !has_suffix_ci(guest, ".EXE")) {
        char with[140];
        size_t i = 0;
        while (guest[i] && i + 1 < sizeof(with) - 4) { with[i] = guest[i]; i++; }
        with[i] = '.'; with[i+1] = 'C'; with[i+2] = 'O'; with[i+3] = 'M'; with[i+4] = '\0';
        if (dos_guest_to_host(with, try_paths[pi], sizeof(try_paths[pi])) == 0) pi++;
        with[i+1] = 'E'; with[i+2] = 'X'; with[i+3] = 'E';
        if (dos_guest_to_host(with, try_paths[pi], sizeof(try_paths[pi])) == 0) pi++;
    }
    /* Apps\name fallback from C:\ */
    {
        char apps[160];
        size_t i = 0;
        const char* p = "Apps\\";
        while (*p && i + 1 < sizeof(apps)) apps[i++] = *p++;
        p = guest;
        while (*p && i + 1 < sizeof(apps)) apps[i++] = *p++;
        apps[i] = '\0';
        if (dos_guest_to_host(apps, try_paths[pi], sizeof(try_paths[pi])) == 0) pi++;
    }

    for (n = 0; n < (size_t)pi; n++) {
        if (!try_paths[n][0]) continue;
        fh = dos_open_host_path(try_paths[n]);
        if (fh) {
            strncpy(hpath, try_paths[n], sizeof(hpath) - 1);
            hpath[sizeof(hpath) - 1] = '\0';
            break;
        }
    }
    if (!fh) {
        if (eq_cmd(guest, "HELLO") || eq_cmd(guest, "HELLO.COM") ||
            eq_cmd(guest, "APPS\\HELLO.COM") || eq_cmd(guest, "APPS/HELLO.COM")) {
            size_t hl = 0;
            const uint8_t* hb = dos_hello_com_bytes(&hl);
            if (dos_load_program_bytes(s, hb, hl, args ? args : "", 0) != 0) return -1;
            s->at_shell = 0;
            s->halted = 0;
            strncpy(s->path, "HELLO.COM", sizeof(s->path) - 1);
            return 0;
        }
        dos_video_puts(s, "Bad command or file name\r\n");
        return -1;
    }

    file = (uint8_t*)kmalloc(cap);
    if (!file) { fs_close(fh); return -1; }
    while ((n = fs_read(fh, file + total, cap - total)) > 0) {
        total += n;
        if (total >= cap) break;
    }
    fs_close(fh);
    if (total >= cap) {
        kfree(file);
        dos_video_puts(s, "Program too large (>1MB)\r\n");
        return -1;
    }
    if (total >= 2 && ((file[0] == 'M' && file[1] == 'Z') || (file[0] == 'Z' && file[1] == 'M')))
        is_exe = 1;
    /* Guest path in PSP env before load (not host Dos/Apps/…) */
    {
        char gpath[96];
        size_t i = 0;
        const char* base = guest;
        const char* slash = guest;
        while (*slash) {
            if (*slash == '\\' || *slash == '/') base = slash + 1;
            slash++;
        }
        /* Prefer cwd-relative name; if launched as bare DOOM.EXE under Apps\, keep that */
        if (s->guest_cwd[0]) {
            const char* c = s->guest_cwd;
            gpath[i++] = 'C'; gpath[i++] = ':';
            if (*c != '\\' && *c != '/') gpath[i++] = '\\';
            while (*c && i + 1 < sizeof(gpath)) {
                gpath[i++] = (*c == '/') ? '\\' : *c;
                c++;
            }
            if (i > 0 && gpath[i - 1] != '\\') gpath[i++] = '\\';
            while (*base && i + 1 < sizeof(gpath)) gpath[i++] = *base++;
            gpath[i] = '\0';
        } else {
            gpath[0] = 'C'; gpath[1] = ':'; gpath[2] = '\\';
            i = 3;
            while (*base && i + 1 < sizeof(gpath)) gpath[i++] = *base++;
            gpath[i] = '\0';
        }
        if (!has_suffix_ci(gpath, ".EXE") && !has_suffix_ci(gpath, ".COM") && is_exe) {
            if (i + 4 < sizeof(gpath)) {
                gpath[i++] = '.'; gpath[i++] = 'E'; gpath[i++] = 'X';
                gpath[i++] = 'E'; gpath[i] = '\0';
            }
        }
        strncpy(s->path, gpath, sizeof(s->path) - 1);
        s->path[sizeof(s->path) - 1] = '\0';
    }
    (void)hpath;
    if (dos_load_program_bytes(s, file, total, args ? args : "", is_exe) != 0) {
        kfree(file);
        dos_video_puts(s, "Error loading program\r\n");
        return -1;
    }
    kfree(file);
    s->at_shell = 0;
    s->halted = 0;
    return 0;
}

static void execute_line(dos_session_t* s) {
    char line[128];
    const char* p;
    char cmd[32];
    size_t i = 0;
    int n;

    for (n = 0; n < s->shell_len && n + 1 < (int)sizeof(line); n++)
        line[n] = s->shell_line[n];
    line[n] = '\0';
    s->shell_len = 0;
    s->shell_line[0] = '\0';

    p = line;
    skip_ws(&p);
    if (!*p) return;

    while (*p && *p != ' ' && *p != '\t' && i + 1 < sizeof(cmd)) {
        cmd[i++] = *p++;
    }
    cmd[i] = '\0';
    skip_ws(&p);

    if (eq_cmd(cmd, "DIR") || eq_cmd(cmd, "LS")) { cmd_dir(s); return; }
    if (eq_cmd(cmd, "CD") || eq_cmd(cmd, "CHDIR")) { cmd_cd(s, p); return; }
    if (eq_cmd(cmd, "TYPE") || eq_cmd(cmd, "CAT")) { cmd_type(s, p); return; }
    if (eq_cmd(cmd, "CLS") || eq_cmd(cmd, "CLEAR")) { cmd_cls(s); return; }
    if (eq_cmd(cmd, "HELP") || eq_cmd(cmd, "?")) { cmd_help(s); return; }
    if (eq_cmd(cmd, "VER")) { cmd_ver(s); return; }
    if (eq_cmd(cmd, "MEM")) { cmd_mem(s); return; }
    if (eq_cmd(cmd, "CYCLES")) { cmd_cycles(s, p); return; }
    if (eq_cmd(cmd, "EXIT") || eq_cmd(cmd, "QUIT")) {
        s->shell_want_close = 1;
        return;
    }

    /* Implicit run */
    (void)dos_shell_launch(s, cmd, p);
}

int dos_shell_on_key(dos_session_t* s, char key) {
    unsigned char k;
    if (!s || !s->at_shell) return 0;
    k = (unsigned char)key;

    if (k == KEY_F11) {
        dos_cycles_adjust(s, -1);
        dos_video_puts(s, "\r\n[cycles=");
        dos_video_puts(s, dos_cycles_label(s));
        dos_video_puts(s, "]\r\n");
        dos_shell_show_prompt(s);
        return 1;
    }
    if (k == KEY_F12) {
        dos_cycles_adjust(s, 1);
        dos_video_puts(s, "\r\n[cycles=");
        dos_video_puts(s, dos_cycles_label(s));
        dos_video_puts(s, "]\r\n");
        dos_shell_show_prompt(s);
        return 1;
    }
    if (k == '\r' || k == '\n') {
        dos_video_puts(s, "\r\n");
        execute_line(s);
        if (!s->at_shell || s->shell_want_close) return 1;
        dos_shell_show_prompt(s);
        return 1;
    }
    if (k == '\b' || k == 0x7F) {
        if (s->shell_len > 0) {
            s->shell_len--;
            s->shell_line[s->shell_len] = '\0';
            dos_video_putc(s, '\b');
        }
        return 1;
    }
    if (k == 0x03) { /* Ctrl+C */
        dos_video_puts(s, "^C\r\n");
        dos_shell_show_prompt(s);
        return 1;
    }
    if (k >= 32 && k < 127 && s->shell_len + 1 < (int)sizeof(s->shell_line)) {
        s->shell_line[s->shell_len++] = (char)k;
        s->shell_line[s->shell_len] = '\0';
        dos_video_putc(s, (char)k);
        return 1;
    }
    return 1; /* swallow unknown at prompt */
}
