#include "config_boot.h"
#include "filesystem.h"
#include "../lib/string.h"

extern void print(const char*);

static int cfg_is_true(const char* v) {
    if (!v || !v[0]) return 0;
    if ((v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') &&
        v[1] == '\0')
        return 1;
    if ((v[0] == 't' || v[0] == 'T') &&
        (v[1] == 'r' || v[1] == 'R') &&
        (v[2] == 'u' || v[2] == 'U') &&
        (v[3] == 'e' || v[3] == 'E') &&
        v[4] == '\0')
        return 1;
    return 0;
}

static void cfg_skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\r') (*p)++;
}

static int cfg_parse_hide_grub(const char* text) {
    const char* p = text;
    int hide = 0;
    while (*p) {
        cfg_skip_ws(&p);
        if (*p == '#' || *p == '\n') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        if (p[0] == 'H' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e' &&
            p[4] == 'G' && p[5] == 'R' && p[6] == 'U' && p[7] == 'B' &&
            p[8] == '=') {
            p += 9;
            cfg_skip_ws(&p);
            char val[16];
            size_t n = 0;
            while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' &&
                   n + 1 < sizeof(val))
                val[n++] = *p++;
            val[n] = '\0';
            hide = cfg_is_true(val);
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return hide;
}

static void cfg_write_grub_boot(int hide) {
    char body[128];
    size_t i = 0;
    const char* line1 = "# Generated from Config/boot.cfg\n";
    const char* to;
    const char* def = "set default=0\n";
    size_t j;

    if (hide)
        to = "set timeout=0\n";
    else
        to = "set timeout=3\n";

    for (j = 0; line1[j] && i + 1 < sizeof(body); j++) body[i++] = line1[j];
    for (j = 0; to[j] && i + 1 < sizeof(body); j++) body[i++] = to[j];
    for (j = 0; def[j] && i + 1 < sizeof(body); j++) body[i++] = def[j];
    body[i] = '\0';

    /* Ensure Config/ exists, then write grub-boot.cfg */
    (void)fs_create_dir("Config");
    if (fs_change_dir("Config") != 0) {
        print("config: cannot enter Config/\n");
        return;
    }
    if (fs_write("grub-boot.cfg", (const uint8_t*)body, i) != 0)
        print("config: failed to write Config/grub-boot.cfg\n");
    fs_cd_up();
}

void config_boot_apply(void) {
    FileHandle* fh;
    uint8_t buf[512];
    size_t n;
    int hide;

    if (!fs_is_persistent()) return;

    if (fs_change_dir("Config") != 0) {
        /* Seed defaults on first boot if missing. */
        if (fs_create_dir("Config") != 0) return;
        if (fs_change_dir("Config") != 0) return;
        {
            static const char seed[] =
                "# GooberOS boot configuration\n"
                "HideGRUB=false\n";
            (void)fs_write("boot.cfg", (const uint8_t*)seed, sizeof(seed) - 1);
        }
        fs_cd_up();
        if (fs_change_dir("Config") != 0) return;
    }

    fh = fs_open("boot.cfg");
    if (!fh) {
        static const char seed[] =
            "# GooberOS boot configuration\n"
            "HideGRUB=false\n";
        (void)fs_write("boot.cfg", (const uint8_t*)seed, sizeof(seed) - 1);
        fh = fs_open("boot.cfg");
        if (!fh) {
            fs_cd_up();
            return;
        }
    }

    n = fs_read(fh, buf, sizeof(buf) - 1);
    fs_close(fh);
    buf[n] = '\0';
    hide = cfg_parse_hide_grub((const char*)buf);
    fs_cd_up();

    cfg_write_grub_boot(hide);
    print(hide ? "config: HideGRUB=true (menu hidden next boot)\n"
               : "config: HideGRUB=false (menu shown next boot)\n");
}
