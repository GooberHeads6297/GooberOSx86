#include "dosemu_priv.h"
#include "../lib/string.h"
#include "../lib/memory.h"

static void set_carry(dos_session_t* s, int err) {
    if (err) s->cpu.flags |= 0x0001;
    else s->cpu.flags &= (uint16_t)~0x0001;
}

static void read_asz(dos_session_t* s, uint16_t seg, uint16_t off, char* out, size_t out_sz) {
    size_t n = 0;
    uint32_t addr = dos_seg_off(seg, off);
    while (n + 1 < out_sz) {
        uint8_t ch = dos_read8(s, addr + (uint32_t)n);
        if (!ch) break;
        out[n++] = (char)ch;
    }
    out[n] = '\0';
}

int dos_guest_to_host(const char* guest, char* host, size_t host_sz) {
    size_t i = 0, j = 0;
    char combined[160];
    const char* g = guest;
    int abs_from_root = 0;
    if (!guest || !host || host_sz < 8) return -1;

    if (((g[0] >= 'A' && g[0] <= 'Z') || (g[0] >= 'a' && g[0] <= 'z')) && g[1] == ':')
        g += 2;
    if (*g == '\\' || *g == '/') {
        abs_from_root = 1;
        while (*g == '\\' || *g == '/') g++;
    }

    if (!abs_from_root && g[0] && g_dos_session.guest_cwd[0] &&
        !(g[0] == '.' && (g[1] == '\\' || g[1] == '/' || g[1] == '\0'))) {
        size_t k = 0;
        while (g_dos_session.guest_cwd[k] && k + 1 < sizeof(combined)) {
            combined[k] = g_dos_session.guest_cwd[k];
            k++;
        }
        if (k && combined[k - 1] != '/' && k + 1 < sizeof(combined))
            combined[k++] = '/';
        while (g[i] && k + 1 < sizeof(combined)) {
            char ch = g[i++];
            if (ch == '\\') ch = '/';
            combined[k++] = ch;
        }
        combined[k] = '\0';
        g = combined;
        i = 0;
    }

    host[j++] = 'D'; host[j++] = 'o'; host[j++] = 's';
    if (*g) host[j++] = '/';
    while (g[i] && j + 1 < host_sz) {
        char ch = g[i++];
        if (ch == '\\') ch = '/';
        host[j++] = ch;
    }
    host[j] = '\0';
    return 0;
}

Directory* dos_resolve_host_dir(const char* host_dir) {
    Directory* restore = fs_get_cwd_dir();
    Directory* root = restore;
    Directory* cur;
    const char* seg;
    char part[32];

    while (root && root->parent) root = root->parent;
    if (!root || !host_dir) return NULL;
    fs_set_current_dir(root);
    cur = root;
    seg = host_dir;
    while (*seg == '/') seg++;
    while (*seg) {
        size_t n = 0;
        Directory* next;
        while (seg[n] && seg[n] != '/' && n + 1 < sizeof(part)) {
            part[n] = seg[n];
            n++;
        }
        part[n] = '\0';
        if (part[0]) {
            next = fs_dir_find_child(cur, part);
            if (!next) {
                if (restore) fs_set_current_dir(restore);
                return NULL;
            }
            cur = next;
            fs_set_current_dir(cur);
        }
        seg += n;
        while (*seg == '/') seg++;
    }
    if (restore) fs_set_current_dir(restore);
    return cur;
}

static int split_host_path(const char* host, char* dir, size_t dir_sz, char* base, size_t base_sz) {
    const char* slash = NULL;
    const char* p = host;
    size_t di = 0, bi = 0;
    if (!host || !dir || !base) return -1;
    while (*p) {
        if (*p == '/') slash = p;
        p++;
    }
    if (!slash) {
        dir[0] = '\0';
        while (host[bi] && bi + 1 < base_sz) { base[bi] = host[bi]; bi++; }
        base[bi] = '\0';
        return 0;
    }
    while (host + di < slash && di + 1 < dir_sz) { dir[di] = host[di]; di++; }
    dir[di] = '\0';
    slash++;
    while (slash[bi] && bi + 1 < base_sz) { base[bi] = slash[bi]; bi++; }
    base[bi] = '\0';
    return 0;
}

int dos_host_mkdir(const char* host_path) {
    char dir[160], base[48];
    Directory* d;
    if (split_host_path(host_path, dir, sizeof(dir), base, sizeof(base)) != 0 || !base[0])
        return -1;
    d = dos_resolve_host_dir(dir[0] ? dir : "Dos");
    if (!d) return -1;
    return fs_dir_create_dir(d, base);
}

int dos_host_rmdir(const char* host_path) {
    char dir[160], base[48];
    Directory* restore;
    Directory* d;
    int rc;
    if (split_host_path(host_path, dir, sizeof(dir), base, sizeof(base)) != 0 || !base[0])
        return -1;
    d = dos_resolve_host_dir(dir[0] ? dir : "Dos");
    if (!d) return -1;
    restore = fs_get_cwd_dir();
    fs_set_current_dir(d);
    rc = fs_delete_dir(base);
    if (restore) fs_set_current_dir(restore);
    return rc;
}

int dos_host_rename(const char* old_host, const char* new_host) {
    char odir[160], obase[48], ndir[160], nbase[48];
    Directory* d;
    Directory* restore;
    int rc;
    if (split_host_path(old_host, odir, sizeof(odir), obase, sizeof(obase)) != 0) return -1;
    if (split_host_path(new_host, ndir, sizeof(ndir), nbase, sizeof(nbase)) != 0) return -1;
    if (strcmp(odir, ndir) != 0) return -1; /* cross-dir rename unsupported */
    d = dos_resolve_host_dir(odir[0] ? odir : "Dos");
    if (!d) return -1;
    restore = fs_get_cwd_dir();
    fs_set_current_dir(d);
    rc = fs_rename(obase, nbase);
    if (restore) fs_set_current_dir(restore);
    return rc;
}

int dos_host_write(const char* host_path, const uint8_t* data, size_t size) {
    char dir[160], base[48];
    Directory* d;
    if (split_host_path(host_path, dir, sizeof(dir), base, sizeof(base)) != 0 || !base[0])
        return -1;
    d = dos_resolve_host_dir(dir[0] ? dir : "Dos");
    if (!d) return -1;
    return fs_dir_write(d, base, data, size);
}

int dos_host_delete(const char* host_path) {
    char dir[160], base[48];
    Directory* d;
    Directory* restore;
    int rc;
    if (split_host_path(host_path, dir, sizeof(dir), base, sizeof(base)) != 0 || !base[0])
        return -1;
    d = dos_resolve_host_dir(dir[0] ? dir : "Dos");
    if (!d) return -1;
    restore = fs_get_cwd_dir();
    fs_set_current_dir(d);
    rc = fs_delete(base);
    if (restore) fs_set_current_dir(restore);
    return rc;
}

static int alloc_fd(dos_session_t* s) {
    int i;
    for (i = 3; i < DOS_MAX_FILES; i++)
        if (!s->files[i].used) return i;
    return -1;
}

static int match_glob(const char* name, const char* pat) {
    int ni = 0, pi = 0;
    if (!name || !pat) return 0;
    while (pat[pi] && name[ni]) {
        char p = pat[pi];
        char n = name[ni];
        if (p >= 'a' && p <= 'z') p = (char)(p - 'a' + 'A');
        if (n >= 'a' && n <= 'z') n = (char)(n - 'a' + 'A');
        if (p == '*') {
            if (pat[pi + 1] == '\0') return 1;
            while (name[ni]) {
                if (match_glob(name + ni, pat + pi + 1)) return 1;
                ni++;
            }
            return match_glob(name + ni, pat + pi + 1);
        }
        if (p != '?' && p != n) return 0;
        pi++; ni++;
    }
    while (pat[pi] == '*') pi++;
    return pat[pi] == '\0' && name[ni] == '\0';
}

static void fill_ffblk(dos_session_t* s, const char* name, int is_dir, uint32_t size) {
    uint32_t dta = dos_seg_off(s->dta_seg, s->dta_off);
    size_t i;
    for (i = 0; i < 43; i++) dos_write8(s, dta + (uint32_t)i, 0);
    dos_write8(s, dta + 0x15, is_dir ? 0x10 : 0x20);
    dos_write16(s, dta + 0x1A, (uint16_t)(size & 0xFFFF));
    dos_write16(s, dta + 0x1C, (uint16_t)((size >> 16) & 0xFFFF));
    for (i = 0; name[i] && i < 12; i++)
        dos_write8(s, dta + 0x1E + (uint32_t)i, (uint8_t)name[i]);
}

static int dos_find_next_entry(dos_session_t* s) {
    Directory* dir;
    char gdir[100];
    char hdir[160];
    char pat[14];
    int idx, total;
    size_t i;

    gdir[0] = '\\';
    i = 0;
    while (s->guest_cwd[i] && i + 2 < sizeof(gdir)) {
        gdir[i + 1] = s->guest_cwd[i];
        i++;
    }
    gdir[i + 1] = '\0';
    if (dos_guest_to_host(gdir, hdir, sizeof(hdir)) != 0) return -1;
    for (i = 0; i < sizeof(pat) - 1 && s->find_pat[i]; i++) pat[i] = s->find_pat[i];
    pat[i] = '\0';

    dir = dos_resolve_host_dir(hdir);
    if (!dir) return -1;
    fs_dir_refresh(dir);

    total = (int)dir->child_count + (int)dir->file_count;
    for (idx = s->find_index; idx < total; idx++) {
        const char* nm;
        int is_dir;
        uint32_t sz = 0;
        if (idx < (int)dir->child_count) {
            nm = dir->children[idx].name;
            is_dir = 1;
        } else {
            FileEntry* fe = &dir->files[idx - (int)dir->child_count];
            nm = fe->name;
            is_dir = 0;
            sz = (uint32_t)fe->size;
        }
        if (!match_glob(nm, pat)) continue;
        s->find_index = idx + 1;
        fill_ffblk(s, nm, is_dir, sz);
        return 0;
    }
    return -1;
}

int dos_file_seek(dos_session_t* s, int fd, uint32_t pos) {
    FileHandle* fh;
    if (!s || fd < 3 || fd >= DOS_MAX_FILES || !s->files[fd].used) return -1;
    fh = s->files[fd].fh;
    if (!fh || !fh->entry) return -1;
    if (fh->entry->fat32) {
        /* reset FAT cursor then skip */
        uint8_t sink[256];
        uint32_t left = pos;
        fh->offset = 0;
        fh->fat_cluster = fh->entry->fat_cluster;
        fh->fat_cluster_base = 0;
        while (left) {
            size_t chunk = left > sizeof(sink) ? sizeof(sink) : (size_t)left;
            size_t got = fs_read(fh, sink, chunk);
            if (got == 0) break;
            left -= (uint32_t)got;
        }
        s->files[fd].pos = fh->offset;
    } else {
        if (pos > fh->entry->size) pos = (uint32_t)fh->entry->size;
        fh->offset = pos;
        s->files[fd].pos = pos;
    }
    return 0;
}

int dos_file_write(dos_session_t* s, int fd, uint32_t addr, uint16_t count) {
    dos_file_t* f;
    FileHandle* fh;
    uint16_t i;
    uint8_t* buf;
    uint32_t need;

    if (!s || fd < 3 || fd >= DOS_MAX_FILES || !s->files[fd].used) return -1;
    f = &s->files[fd];
    fh = f->fh;
    if (!fh || !fh->entry) return -1;

    buf = (uint8_t*)kmalloc(count ? count : 1);
    if (!buf) return -1;
    for (i = 0; i < count; i++) buf[i] = dos_read8(s, addr + i);

    if (!fh->entry->fat32) {
        need = f->pos + count;
        if (need > fh->entry->size || !fh->entry->data) {
            uint8_t* nd = (uint8_t*)krealloc(fh->entry->data, need ? need : 1);
            uint32_t old = (uint32_t)fh->entry->size;
            if (!nd) { kfree(buf); return -1; }
            if (need > old) memset(nd + old, 0, need - old);
            fh->entry->data = nd;
            fh->entry->size = need;
            fh->entry->owned = 1;
        }
        for (i = 0; i < count; i++) fh->entry->data[f->pos + i] = buf[i];
        f->pos += count;
        fh->offset = f->pos;
        kfree(buf);
        return (int)count;
    }

    /* FAT32: rewrite whole file via path */
    {
        uint8_t* whole;
        uint32_t old_sz = (uint32_t)fh->entry->size;
        uint32_t new_sz = f->pos + count;
        if (new_sz < old_sz) new_sz = old_sz;
        whole = (uint8_t*)kmalloc(new_sz ? new_sz : 1);
        if (!whole) { kfree(buf); return -1; }
        memset(whole, 0, new_sz);
        if (old_sz) {
            FileHandle* rh = dos_open_host_path(f->host_path);
            if (rh) {
                size_t got = fs_read(rh, whole, old_sz);
                (void)got;
                fs_close(rh);
            }
        }
        for (i = 0; i < count; i++) whole[f->pos + i] = buf[i];
        if (dos_host_write(f->host_path, whole, new_sz) != 0) {
            kfree(whole); kfree(buf); return -1;
        }
        f->pos += count;
        fs_close(fh);
        f->fh = dos_open_host_path(f->host_path);
        if (f->fh) dos_file_seek(s, fd, f->pos);
        kfree(whole);
        kfree(buf);
        return (int)count;
    }
}

static void store_cwd_from_host(dos_session_t* s, const char* hpath) {
    const char* p = hpath;
    size_t k = 0;
    if (p[0] == 'D' && p[1] == 'o' && p[2] == 's') {
        p += 3;
        if (*p == '/') p++;
    }
    while (*p && k + 1 < sizeof(s->guest_cwd)) s->guest_cwd[k++] = *p++;
    s->guest_cwd[k] = '\0';
}

static int dos_int21(dos_session_t* s) {
    uint8_t ah = (uint8_t)((s->cpu.ax >> 8) & 0xFF);
    uint8_t al = (uint8_t)(s->cpu.ax & 0xFF);

    switch (ah) {
    case 0x00:
    case 0x4C:
        s->return_code = al;
        s->halted = 1;
        s->shell_reentry = 1; /* return to GooberDOS prompt */
        set_carry(s, 0);
        return 0;
    case 0x01: {
        char ch;
        if (!dos_key_pop(s, &ch)) { s->cpu.ip = (uint16_t)(s->cpu.ip - 2); return 0; }
        if (ch) dos_video_putc(s, ch);
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | (uint8_t)ch);
        set_carry(s, 0);
        return 0;
    }
    case 0x02:
        dos_video_putc(s, (char)(s->cpu.dx & 0xFF));
        set_carry(s, 0);
        return 0;
    case 0x06: {
        if ((s->cpu.dx & 0xFF) != 0xFF) {
            dos_video_putc(s, (char)(s->cpu.dx & 0xFF));
            set_carry(s, 0);
        } else {
            uint16_t w;
            if (dos_key_pop_word(s, &w)) {
                s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | (w & 0xFF));
                s->cpu.flags &= (uint16_t)~0x0040;
            } else {
                s->cpu.flags |= 0x0040;
                s->cpu.ax &= 0xFF00;
            }
        }
        return 0;
    }
    case 0x07: case 0x08: {
        char ch;
        if (!dos_key_pop(s, &ch)) { s->cpu.ip = (uint16_t)(s->cpu.ip - 2); return 0; }
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | (uint8_t)ch);
        set_carry(s, 0);
        return 0;
    }
    case 0x09: {
        uint32_t addr = dos_seg_off(s->cpu.ds, s->cpu.dx);
        for (;;) {
            uint8_t ch = dos_read8(s, addr++);
            if (ch == '$') break;
            dos_video_putc(s, (char)ch);
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x0A: {
        uint32_t addr = dos_seg_off(s->cpu.ds, s->cpu.dx);
        uint8_t max = dos_read8(s, addr);
        uint8_t n = 0;
        char ch;
        if (max < 1) { set_carry(s, 0); return 0; }
        while (n + 1 < max) {
            if (!dos_key_pop(s, &ch)) { s->cpu.ip = (uint16_t)(s->cpu.ip - 2); return 0; }
            if (ch == '\r' || ch == '\n') {
                dos_video_putc(s, '\r'); dos_video_putc(s, '\n'); break;
            }
            if (ch == '\b') { if (n > 0) { n--; dos_video_putc(s, '\b'); } continue; }
            if (!ch) continue;
            dos_write8(s, addr + 2 + n, (uint8_t)ch);
            dos_video_putc(s, ch);
            n++;
        }
        dos_write8(s, addr + 1, n);
        dos_write8(s, addr + 2 + n, '\r');
        set_carry(s, 0);
        return 0;
    }
    case 0x0B: {
        uint16_t w;
        if (dos_key_peek_word(s, &w)) s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 0xFF);
        else s->cpu.ax = (uint16_t)(s->cpu.ax & 0xFF00);
        set_carry(s, 0);
        return 0;
    }
    case 0x0E: /* set drive — accept C: only */
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 1); /* last drive = A..? report 1 */
        set_carry(s, 0);
        return 0;
    case 0x19:
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 2);
        set_carry(s, 0);
        return 0;
    case 0x1A:
        s->dta_seg = s->cpu.ds;
        s->dta_off = s->cpu.dx;
        set_carry(s, 0);
        return 0;
    case 0x2A:
        s->cpu.cx = 2026;
        s->cpu.dx = (8 << 8) | 7;
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 5);
        set_carry(s, 0);
        return 0;
    case 0x2B: case 0x2D: /* set date/time accepted */
        set_carry(s, 0);
        return 0;
    case 0x2C: {
        uint32_t t = s->bios_ticks;
        uint32_t sec = (t / 18) % 60;
        uint32_t min = (t / 18 / 60) % 60;
        uint32_t hr = (t / 18 / 60 / 60) % 24;
        s->cpu.cx = (uint16_t)((hr << 8) | min);
        s->cpu.dx = (uint16_t)((sec << 8) | ((t % 18) * 5));
        set_carry(s, 0);
        return 0;
    }
    case 0x2F:
        s->cpu.es = s->dta_seg;
        s->cpu.bx = s->dta_off;
        set_carry(s, 0);
        return 0;
    case 0x30:
        s->cpu.ax = 0x0005;
        s->cpu.bx = 0xFF00;
        s->cpu.cx = 0;
        set_carry(s, 0);
        return 0;
    case 0x33: /* Ctrl-Break */
        if (al == 0) s->cpu.dx = (uint16_t)((s->cpu.dx & 0xFF00) | s->break_flag);
        else if (al == 1) s->break_flag = (uint8_t)(s->cpu.dx & 1);
        else if (al == 2) {
            s->cpu.dx = (uint16_t)((s->cpu.dx & 0xFF00) | s->break_flag);
            s->break_flag = (uint8_t)(s->cpu.dx & 1);
        }
        set_carry(s, 0);
        return 0;
    case 0x25: {
        uint8_t vec = al;
        uint32_t a = (uint32_t)vec * 4u;
        dos_write16(s, a, s->cpu.dx);
        dos_write16(s, a + 2, s->cpu.ds);
        set_carry(s, 0);
        return 0;
    }
    case 0x35: {
        uint8_t vec = al;
        uint32_t a = (uint32_t)vec * 4u;
        s->cpu.bx = dos_read16(s, a);
        s->cpu.es = dos_read16(s, a + 2);
        set_carry(s, 0);
        return 0;
    }
    case 0x39: {
        char gpath[128], hpath[160];
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0 || dos_host_mkdir(hpath) != 0) {
            s->cpu.ax = 3; set_carry(s, 1); return 0;
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x3A: {
        char gpath[128], hpath[160];
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0 || dos_host_rmdir(hpath) != 0) {
            s->cpu.ax = 3; set_carry(s, 1); return 0;
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x3B: {
        char gpath[128], hpath[160];
        Directory* d;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0) {
            s->cpu.ax = 3; set_carry(s, 1); return 0;
        }
        d = dos_resolve_host_dir(hpath);
        if (!d) { s->cpu.ax = 3; set_carry(s, 1); return 0; }
        store_cwd_from_host(s, hpath);
        set_carry(s, 0);
        return 0;
    }
    case 0x47: {
        uint32_t addr = dos_seg_off(s->cpu.ds, s->cpu.si);
        size_t i = 0;
        while (s->guest_cwd[i] && i + 1 < 64) {
            char ch = s->guest_cwd[i];
            if (ch == '/') ch = '\\';
            dos_write8(s, addr + (uint32_t)i, (uint8_t)ch);
            i++;
        }
        dos_write8(s, addr + (uint32_t)i, 0);
        set_carry(s, 0);
        return 0;
    }
    case 0x3C: case 0x3D: {
        char gpath[128], hpath[160];
        FileHandle* fh;
        int fd;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0) {
            s->cpu.ax = 3; set_carry(s, 1); return 0;
        }
        if (ah == 0x3C) (void)dos_host_write(hpath, (const uint8_t*)"", 0);
        fh = dos_open_host_path(hpath);
        if (!fh) { s->cpu.ax = 2; set_carry(s, 1); return 0; }
        fd = alloc_fd(s);
        if (fd < 0) { fs_close(fh); s->cpu.ax = 4; set_carry(s, 1); return 0; }
        s->files[fd].used = 1;
        s->files[fd].fh = fh;
        s->files[fd].is_std = 0;
        s->files[fd].pos = 0;
        strncpy(s->files[fd].host_path, hpath, sizeof(s->files[fd].host_path) - 1);
        s->cpu.ax = (uint16_t)fd;
        set_carry(s, 0);
        return 0;
    }
    case 0x3E: {
        int fd = (int)s->cpu.bx;
        if (fd >= 3 && fd < DOS_MAX_FILES && s->files[fd].used) {
            if (s->files[fd].fh) fs_close(s->files[fd].fh);
            s->files[fd].used = 0;
            s->files[fd].fh = NULL;
            s->files[fd].host_path[0] = '\0';
            set_carry(s, 0);
        } else { s->cpu.ax = 6; set_carry(s, 1); }
        return 0;
    }
    case 0x3F: {
        int fd = (int)s->cpu.bx;
        uint16_t count = s->cpu.cx;
        uint32_t addr = dos_seg_off(s->cpu.ds, s->cpu.dx);
        uint8_t buf[512];
        size_t got = 0, chunk, i;
        if (fd < 3) {
            char ch;
            while (got < count && dos_key_pop(s, &ch)) {
                dos_write8(s, addr + (uint32_t)got, (uint8_t)ch);
                got++;
                if (ch == '\r') break;
            }
            if (got == 0 && count) { s->cpu.ip = (uint16_t)(s->cpu.ip - 2); return 0; }
            s->cpu.ax = (uint16_t)got;
            set_carry(s, 0);
            return 0;
        }
        if (fd >= DOS_MAX_FILES || !s->files[fd].used || !s->files[fd].fh) {
            s->cpu.ax = 6; set_carry(s, 1); return 0;
        }
        if (s->files[fd].fh->offset != s->files[fd].pos)
            dos_file_seek(s, fd, s->files[fd].pos);
        while (got < count) {
            chunk = count - got;
            if (chunk > sizeof(buf)) chunk = sizeof(buf);
            chunk = fs_read(s->files[fd].fh, buf, chunk);
            if (chunk == 0) break;
            for (i = 0; i < chunk; i++)
                dos_write8(s, addr + (uint32_t)got + (uint32_t)i, buf[i]);
            got += chunk;
            s->files[fd].pos += (uint32_t)chunk;
        }
        s->cpu.ax = (uint16_t)got;
        set_carry(s, 0);
        return 0;
    }
    case 0x40: {
        int fd = (int)s->cpu.bx;
        uint16_t count = s->cpu.cx;
        uint32_t addr = dos_seg_off(s->cpu.ds, s->cpu.dx);
        uint16_t i;
        if (fd < 3) {
            for (i = 0; i < count; i++)
                dos_video_putc(s, (char)dos_read8(s, addr + i));
            s->cpu.ax = count;
            set_carry(s, 0);
            return 0;
        }
        {
            int n = dos_file_write(s, fd, addr, count);
            if (n < 0) { s->cpu.ax = 5; set_carry(s, 1); return 0; }
            s->cpu.ax = (uint16_t)n;
            set_carry(s, 0);
            return 0;
        }
    }
    case 0x41: {
        char gpath[128], hpath[160];
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0 || dos_host_delete(hpath) != 0) {
            s->cpu.ax = 2; set_carry(s, 1); return 0;
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x42: {
        int fd = (int)s->cpu.bx;
        uint32_t off = ((uint32_t)s->cpu.cx << 16) | s->cpu.dx;
        uint32_t newpos;
        if (fd < 3 || fd >= DOS_MAX_FILES || !s->files[fd].used || !s->files[fd].fh) {
            s->cpu.ax = 6; set_carry(s, 1); return 0;
        }
        if (al == 0) newpos = off;
        else if (al == 1) newpos = s->files[fd].pos + off;
        else newpos = (uint32_t)s->files[fd].fh->entry->size + off;
        if (dos_file_seek(s, fd, newpos) != 0) { s->cpu.ax = 25; set_carry(s, 1); return 0; }
        s->cpu.dx = (uint16_t)(s->files[fd].pos >> 16);
        s->cpu.ax = (uint16_t)(s->files[fd].pos & 0xFFFF);
        set_carry(s, 0);
        return 0;
    }
    case 0x43: { /* get/set attributes */
        char gpath[128], hpath[160];
        Directory* d;
        char dir[160], base[48];
        size_t i;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0) {
            s->cpu.ax = 2; set_carry(s, 1); return 0;
        }
        if (split_host_path(hpath, dir, sizeof(dir), base, sizeof(base)) != 0) {
            s->cpu.ax = 2; set_carry(s, 1); return 0;
        }
        d = dos_resolve_host_dir(dir[0] ? dir : "Dos");
        if (!d) { s->cpu.ax = 2; set_carry(s, 1); return 0; }
        if (al == 0) {
            for (i = 0; i < d->file_count; i++)
                if (strcmp(d->files[i].name, base) == 0) {
                    s->cpu.cx = 0x20; set_carry(s, 0); return 0;
                }
            for (i = 0; i < d->child_count; i++)
                if (strcmp(d->children[i].name, base) == 0) {
                    s->cpu.cx = 0x10; set_carry(s, 0); return 0;
                }
            s->cpu.ax = 2; set_carry(s, 1); return 0;
        }
        set_carry(s, 0); /* set attrs ignored */
        return 0;
    }
    case 0x44:
        if (al == 0) {
            if (s->cpu.bx < 3) s->cpu.dx = 0x80D3; /* device + cooked + stdin/out */
            else s->cpu.dx = 0x0000; /* disk file */
            set_carry(s, 0);
        } else if (al == 7) { /* get output status */
            s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 0xFF);
            set_carry(s, 0);
        } else {
            set_carry(s, 0);
        }
        return 0;
    case 0x4E: {
        char gpath[128];
        const char* slash = NULL;
        size_t i = 0;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        while (gpath[i]) {
            if (gpath[i] == '\\' || gpath[i] == '/') slash = &gpath[i];
            i++;
        }
        if (slash) {
            char dirpart[96];
            size_t n = (size_t)(slash - gpath);
            char hpath[160];
            if (n >= sizeof(dirpart)) n = sizeof(dirpart) - 1;
            for (i = 0; i < n; i++) dirpart[i] = gpath[i];
            dirpart[n] = '\0';
            if (dos_guest_to_host(dirpart, hpath, sizeof(hpath)) == 0)
                store_cwd_from_host(s, hpath);
            i = 0;
            slash++;
            while (slash[i] && i + 1 < sizeof(s->find_pat)) { s->find_pat[i] = slash[i]; i++; }
            s->find_pat[i] = '\0';
        } else {
            i = 0;
            while (gpath[i] && i + 1 < sizeof(s->find_pat)) { s->find_pat[i] = gpath[i]; i++; }
            s->find_pat[i] = '\0';
        }
        if (!s->find_pat[0]) { s->find_pat[0] = '*'; s->find_pat[1] = '\0'; }
        s->find_index = 0;
        if (dos_find_next_entry(s) != 0) { s->cpu.ax = 18; set_carry(s, 1); return 0; }
        set_carry(s, 0);
        return 0;
    }
    case 0x4F:
        if (s->find_index < 0) { s->cpu.ax = 18; set_carry(s, 1); return 0; }
        if (dos_find_next_entry(s) != 0) { s->cpu.ax = 18; set_carry(s, 1); return 0; }
        set_carry(s, 0);
        return 0;
    case 0x4B: { /* EXEC — load and run child in-session */
        char gpath[128];
        char args[128];
        uint32_t pb;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        args[0] = '\0';
        /* AL=0 load+exec: ES:BX -> param block; cmdline at +2 word offset */
        if (al == 0) {
            pb = dos_seg_off(s->cpu.es, s->cpu.bx);
            {
                uint16_t cmd_off = dos_read16(s, pb + 2);
                uint16_t cmd_seg = dos_read16(s, pb + 4);
                uint8_t len = dos_read8(s, dos_seg_off(cmd_seg, cmd_off));
                size_t i;
                if (len > 126) len = 126;
                for (i = 0; i < len && i + 1 < sizeof(args); i++)
                    args[i] = (char)dos_read8(s, dos_seg_off(cmd_seg, cmd_off + 1 + (uint16_t)i));
                args[i] = '\0';
            }
            if (dos_shell_launch(s, gpath, args) != 0) {
                s->cpu.ax = 2; set_carry(s, 1); return 0;
            }
            set_carry(s, 0);
            return 0;
        }
        s->cpu.ax = 1; set_carry(s, 1); return 0;
    }
    case 0x4D:
        s->cpu.ax = s->return_code;
        set_carry(s, 0);
        return 0;
    case 0x48: {
        uint16_t seg = 0;
        uint16_t need = s->cpu.bx;
        if (dos_mcb_alloc(s, need, &seg) != 0) {
            /* DOS: AX=8, BX=largest available block (paragraphs) */
            uint16_t cur = s->mcb_first, largest = 0;
            for (;;) {
                uint8_t t = dos_read8(s, dos_seg_off(cur, 0));
                uint16_t sz = dos_read16(s, dos_seg_off(cur, 3));
                uint16_t owner = dos_read16(s, dos_seg_off(cur, 1));
                if (t != 'M' && t != 'Z') break;
                if (owner == 0 && sz > largest) largest = sz;
                if (t == 'Z') break;
                cur = (uint16_t)(cur + sz + 1u);
            }
            s->cpu.ax = 8;
            s->cpu.bx = largest;
            set_carry(s, 1);
            return 0;
        }
        s->cpu.ax = seg;
        set_carry(s, 0);
        return 0;
    }
    case 0x49:
        if (dos_mcb_free(s, s->cpu.es) != 0) { s->cpu.ax = 9; set_carry(s, 1); return 0; }
        set_carry(s, 0);
        return 0;
    case 0x4A: {
        uint16_t mx = 0;
        if (dos_mcb_resize(s, s->cpu.es, s->cpu.bx, &mx) != 0) {
            s->cpu.ax = 8;
            s->cpu.bx = mx;
            set_carry(s, 1);
            return 0;
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x50: s->psp_seg = s->cpu.bx; set_carry(s, 0); return 0;
    case 0x51: case 0x62:
        s->cpu.bx = s->psp_seg;
        set_carry(s, 0);
        return 0;
    case 0x56: {
        char g1[128], g2[128], h1[160], h2[160];
        read_asz(s, s->cpu.ds, s->cpu.dx, g1, sizeof(g1));
        read_asz(s, s->cpu.es, s->cpu.di, g2, sizeof(g2));
        if (dos_guest_to_host(g1, h1, sizeof(h1)) != 0 ||
            dos_guest_to_host(g2, h2, sizeof(h2)) != 0 ||
            dos_host_rename(h1, h2) != 0) {
            s->cpu.ax = 2; set_carry(s, 1); return 0;
        }
        set_carry(s, 0);
        return 0;
    }
    case 0x57: /* get/set file date/time stub */
        if (al == 0) { s->cpu.cx = 0; s->cpu.dx = 0; }
        set_carry(s, 0);
        return 0;
    case 0x5A: case 0x5B: { /* create temp / create new */
        char gpath[128], hpath[160];
        FileHandle* fh;
        int fd;
        read_asz(s, s->cpu.ds, s->cpu.dx, gpath, sizeof(gpath));
        if (dos_guest_to_host(gpath, hpath, sizeof(hpath)) != 0) {
            s->cpu.ax = 3; set_carry(s, 1); return 0;
        }
        (void)dos_host_write(hpath, (const uint8_t*)"", 0);
        fh = dos_open_host_path(hpath);
        if (!fh) { s->cpu.ax = 2; set_carry(s, 1); return 0; }
        fd = alloc_fd(s);
        if (fd < 0) { fs_close(fh); s->cpu.ax = 4; set_carry(s, 1); return 0; }
        s->files[fd].used = 1;
        s->files[fd].fh = fh;
        s->files[fd].pos = 0;
        strncpy(s->files[fd].host_path, hpath, sizeof(s->files[fd].host_path) - 1);
        s->cpu.ax = (uint16_t)fd;
        set_carry(s, 0);
        return 0;
    }
    default:
        s->cpu.ax = 1;
        set_carry(s, 1);
        return 0;
    }
}

int dos_handle_int(dos_session_t* s, uint8_t vec) {
    if (!s) return -1;
    if (vec == 0x21) return dos_int21(s);
    if (vec == 0x20) { s->halted = 1; return 0; }
    if (vec == 0x10) return dos_bios_int10(s);
    if (vec == 0x16) return dos_bios_int16(s);
    if (vec == 0x1A) return dos_bios_int1a(s);
    if (vec == 0x15) return dos_bios_int15(s);
    if (vec == 0x2F) return dos_bios_int2f(s);
    if (vec == 0x31) return dpmi_int31(s);
    if (vec == 0x33) return dos_bios_int33(s);
    if (vec == 0x11) { s->cpu.ax = 0x0023; return 0; } /* diskette + FPU + 80x25 */
    if (vec == 0x12) { s->cpu.ax = 640; return 0; }
    if (vec == 0x1C) return 0;
    if (vec == 0x13) { /* disk BIOS — not supported; CF */
        s->cpu.flags |= 0x0001;
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 0x01);
        return 0;
    }
    return 0;
}
