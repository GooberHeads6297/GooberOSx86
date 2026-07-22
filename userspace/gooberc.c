#include "userspace.h"
#include "syscall.h"
#include "../fs/filesystem.h"
#include "../lib/memory.h"
#include "../lib/string.h"

extern void print(const char*);

/*
 * Minimal GooberC compiler (v0): line-oriented subset → bytecode .gob
 *
 *   use goober.console | use goober.gui
 *   app gui | app console
 *   print "..."
 *   window "Title" WxH
 *   text "..."
 *   wait
 *   exit
 */

static const char* gc_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
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

int gooberc_compile(const char* src_path, const char* out_path) {
    FileHandle* fh;
    uint8_t* src;
    size_t src_cap = 32768;
    size_t src_len = 0;
    size_t n;
    uint8_t code[4096];
    uint8_t rodata[4096];
    uint32_t code_len = 0;
    uint32_t ro_len = 0;
    int kind = GOB_KIND_CONSOLE;
    const char* p;
    gob_header_t hdr;
    uint8_t* outbuf;
    size_t out_total;
    int has_exit = 0;
    uint32_t i;

    if (!src_path || !out_path) return -1;

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
        const char* line = gc_skip_ws(p);
        char lit[128];
        size_t li = 0;

        if (*line == '\0' || *line == '\n' || *line == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        if (line[0] == 'u' && line[1] == 's' && line[2] == 'e') {
            if (line_has(line, "gui")) kind = GOB_KIND_GUI;
        } else if (line[0] == 'a' && line[1] == 'p' && line[2] == 'p') {
            if (line_has(line, "gui")) kind = GOB_KIND_GUI;
            else kind = GOB_KIND_CONSOLE;
        } else if (line[0] == 'p' && line[1] == 'r' && line[2] == 'i' &&
                   line[3] == 'n' && line[4] == 't') {
            const char* q = gc_skip_ws(line + 5);
            if (*q == '"') {
                q++;
                while (*q && *q != '"' && li + 1 < sizeof(lit)) lit[li++] = *q++;
                lit[li] = '\0';
                if (code_len + 9 < sizeof(code) && ro_len + li + 1 < sizeof(rodata)) {
                    uint32_t off = ro_len;
                    memcpy(rodata + ro_len, lit, li + 1);
                    ro_len += (uint32_t)li + 1;
                    code[code_len++] = GBC_WRITE;
                    wr32(code + code_len, off); code_len += 4;
                    wr32(code + code_len, (uint32_t)li); code_len += 4;
                }
            }
        } else if (line[0] == 'w' && line[1] == 'i' && line[2] == 'n' &&
                   line[3] == 'd' && line[4] == 'o' && line[5] == 'w') {
            const char* q = gc_skip_ws(line + 6);
            uint16_t w = 420, h = 240;
            if (*q == '"') {
                q++;
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
                if (code_len + 9 < sizeof(code) && ro_len + li + 1 < sizeof(rodata)) {
                    uint32_t off = ro_len;
                    memcpy(rodata + ro_len, lit, li + 1);
                    ro_len += (uint32_t)li + 1;
                    code[code_len++] = GBC_GUI_CREATE;
                    wr32(code + code_len, off); code_len += 4;
                    wr16(code + code_len, w); code_len += 2;
                    wr16(code + code_len, h); code_len += 2;
                    kind = GOB_KIND_GUI;
                }
            }
        } else if (line[0] == 't' && line[1] == 'e' && line[2] == 'x' &&
                   line[3] == 't') {
            const char* q = gc_skip_ws(line + 4);
            if (*q == '"') {
                q++;
                while (*q && *q != '"' && li + 1 < sizeof(lit)) lit[li++] = *q++;
                lit[li] = '\0';
                if (code_len + 13 < sizeof(code) && ro_len + li + 1 < sizeof(rodata)) {
                    uint32_t off = ro_len;
                    memcpy(rodata + ro_len, lit, li + 1);
                    ro_len += (uint32_t)li + 1;
                    code[code_len++] = GBC_GUI_TEXT;
                    wr32(code + code_len, 0); code_len += 4;
                    wr16(code + code_len, 0); code_len += 2;
                    wr16(code + code_len, 0); code_len += 2;
                    wr32(code + code_len, off); code_len += 4;
                }
            }
        } else if (line[0] == 'w' && line[1] == 'a' && line[2] == 'i' &&
                   line[3] == 't') {
            if (code_len + 5 < sizeof(code)) {
                code[code_len++] = GBC_GUI_WAIT;
                wr32(code + code_len, 0); code_len += 4;
            }
        } else if (line[0] == 'e' && line[1] == 'x' && line[2] == 'i' &&
                   line[3] == 't') {
            if (code_len + 5 < sizeof(code)) {
                code[code_len++] = GBC_EXIT;
                wr32(code + code_len, 0); code_len += 4;
                has_exit = 1;
            }
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!has_exit && code_len + 5 < sizeof(code)) {
        for (i = 0; i < code_len; i++)
            if (code[i] == GBC_EXIT) has_exit = 1;
        if (!has_exit) {
            code[code_len++] = GBC_EXIT;
            wr32(code + code_len, 0); code_len += 4;
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
    hdr.code_size = code_len;
    hdr.rodata_size = ro_len;

    out_total = sizeof(hdr) + code_len + ro_len;
    outbuf = (uint8_t*)kmalloc(out_total);
    if (!outbuf) { kfree(src); return -1; }
    memcpy(outbuf, &hdr, sizeof(hdr));
    memcpy(outbuf + sizeof(hdr), code, code_len);
    memcpy(outbuf + sizeof(hdr) + code_len, rodata, ro_len);

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
