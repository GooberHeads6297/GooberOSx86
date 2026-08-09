#include "dosemu_priv.h"

/* DPMI INT 31h helpers exist for later, but we do NOT advertise a host.
 * A stub mode-switch made DOS/16M take the DPMI path and die with error [32].
 * Raw CR0/LGDT + INT 15h extended memory is the supported bring-up path.
 */

void dpmi_init(dos_session_t* s) {
    int i;
    if (!s) return;
    s->dpmi_installed = 0;
    s->dpmi_version = 0x005A; /* 0.90 — kept for when we re-enable a real host */
    s->dpmi_next_handle = 1;
    for (i = 0; i < DPMI_MAX_BLOCKS; i++) {
        s->dpmi_blocks[i].used = 0;
        s->dpmi_blocks[i].linear = 0;
        s->dpmi_blocks[i].pages = 0;
        s->dpmi_blocks[i].handle = 0;
    }
    if (s->himem_brk < 0x100000u) s->himem_brk = 0x100000u;
}

/* INT 2F multiplex: DPMI install check AX=1687h */
int dos_bios_int2f(dos_session_t* s) {
    uint16_t ax = s->cpu.ax;
    if (ax == 0x1687) {
        /* Not installed — leave AX=1687h so clients use raw/VCPI/XMS instead */
        return 0;
    }
    if (ax == 0x1600) {
        /* Windows enhanced mode check — not present */
        s->cpu.ax = 0;
        return 0;
    }
    /* XMS install check often via INT 2F AX=4310 — not present */
    if (ax == 0x4300) {
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | 0x00);
        return 0;
    }
    s->cpu.flags |= 0x0001;
    return 0;
}

static int dpmi_alloc_block(dos_session_t* s, uint32_t pages, uint32_t* out_lin, uint32_t* out_h) {
    int i;
    uint32_t need = pages * 4096u;
    uint32_t base;
    if (!s || pages == 0) return -1;
    base = (s->himem_brk + 0xFFFu) & ~0xFFFu;
    if (base + need > DOS_MEM_SIZE) return -1;
    for (i = 0; i < DPMI_MAX_BLOCKS; i++) {
        if (!s->dpmi_blocks[i].used) {
            s->dpmi_blocks[i].used = 1;
            s->dpmi_blocks[i].linear = base;
            s->dpmi_blocks[i].pages = pages;
            s->dpmi_blocks[i].handle = s->dpmi_next_handle++;
            s->himem_brk = base + need;
            *out_lin = base;
            *out_h = s->dpmi_blocks[i].handle;
            return 0;
        }
    }
    return -1;
}

static int dpmi_free_block(dos_session_t* s, uint32_t handle) {
    int i;
    for (i = 0; i < DPMI_MAX_BLOCKS; i++) {
        if (s->dpmi_blocks[i].used && s->dpmi_blocks[i].handle == handle) {
            s->dpmi_blocks[i].used = 0;
            return 0;
        }
    }
    return -1;
}

int dpmi_int31(dos_session_t* s) {
    uint16_t ax = s->cpu.ax;
    s->cpu.flags &= (uint16_t)~0x0001;

    switch (ax) {
    case 0x0000: { /* Allocate LDT descriptors */
        /* Return a fake selector */
        uint16_t n = s->cpu.cx;
        if (n == 0) { s->cpu.flags |= 1; s->cpu.ax = 0x8011; return 0; }
        s->cpu.ax = 0x00A0; /* growing fake selectors */
        return 0;
    }
    case 0x0001: /* Free LDT descriptor */
        return 0;
    case 0x0006: { /* Get segment base address */
        pm_seg_t seg;
        if (pm_load_selector(s, s->cpu.bx, &seg) != 0) {
            s->cpu.flags |= 1;
            s->cpu.ax = 0x8022;
            return 0;
        }
        s->cpu.cx = (uint16_t)(seg.base >> 16);
        s->cpu.dx = (uint16_t)(seg.base & 0xFFFF);
        return 0;
    }
    case 0x0007: { /* Set segment base address */
        /* Update cache if matches */
        uint32_t base = ((uint32_t)s->cpu.cx << 16) | s->cpu.dx;
        uint16_t sel = s->cpu.bx;
        if (sel == s->cpu.ds) { s->seg_ds.base = base; }
        if (sel == s->cpu.es) { s->seg_es.base = base; }
        if (sel == s->cpu.ss) { s->seg_ss.base = base; }
        if (sel == s->fs) { s->seg_fs.base = base; }
        if (sel == s->gs) { s->seg_gs.base = base; }
        return 0;
    }
    case 0x0008: { /* Set segment limit */
        uint32_t lim = ((uint32_t)s->cpu.cx << 16) | s->cpu.dx;
        uint16_t sel = s->cpu.bx;
        if (sel == s->cpu.ds) s->seg_ds.limit = lim;
        if (sel == s->cpu.es) s->seg_es.limit = lim;
        if (sel == s->cpu.ss) s->seg_ss.limit = lim;
        return 0;
    }
    case 0x0009: /* Set descriptor access rights — accept */
        return 0;
    case 0x000A: /* Create alias descriptor */
        s->cpu.ax = (uint16_t)(s->cpu.bx + 8);
        return 0;
    case 0x0500: { /* Get free memory information */
        uint32_t free = (DOS_MEM_SIZE > s->himem_brk) ? (DOS_MEM_SIZE - s->himem_brk) : 0;
        uint32_t addr = dos_seg_off(s->cpu.es, s->cpu.di);
        dos_write32(s, addr + 0, free);           /* largest block */
        dos_write32(s, addr + 4, free / 4096u);   /* max unlocked pages */
        dos_write32(s, addr + 8, free / 4096u);
        dos_write32(s, addr + 12, free / 4096u);
        dos_write32(s, addr + 16, free / 4096u);
        dos_write32(s, addr + 20, free / 4096u);
        dos_write32(s, addr + 24, free / 4096u);
        dos_write32(s, addr + 28, 0);
        dos_write32(s, addr + 32, 0);
        dos_write32(s, addr + 36, 4096);
        return 0;
    }
    case 0x0501: { /* Allocate memory block */
        uint32_t bytes = ((uint32_t)s->cpu.bx << 16) | s->cpu.cx;
        uint32_t pages = (bytes + 4095u) / 4096u;
        uint32_t lin = 0, h = 0;
        if (dpmi_alloc_block(s, pages ? pages : 1, &lin, &h) != 0) {
            s->cpu.flags |= 1;
            s->cpu.ax = 0x8012;
            return 0;
        }
        s->cpu.bx = (uint16_t)(lin >> 16);
        s->cpu.cx = (uint16_t)(lin & 0xFFFF);
        s->cpu.si = (uint16_t)(h >> 16);
        s->cpu.di = (uint16_t)(h & 0xFFFF);
        return 0;
    }
    case 0x0502: { /* Free memory block */
        uint32_t h = ((uint32_t)s->cpu.si << 16) | s->cpu.di;
        if (dpmi_free_block(s, h) != 0) {
            s->cpu.flags |= 1;
            s->cpu.ax = 0x8023;
        }
        return 0;
    }
    case 0x0600: /* Lock linear region — nop success */
    case 0x0601: /* Unlock */
        return 0;
    case 0x0300: /* Simulate real-mode interrupt */
    case 0x0301: /* Call real-mode far */
    case 0x0302:
        /* Not fully implemented — return success with no-op for now */
        return 0;
    case 0x0400: /* Get DPMI version */
        s->cpu.ax = s->dpmi_version;
        s->cpu.bx = 0x0001;
        s->cpu.cx = (uint16_t)((s->cpu.cx & 0xFF00) | 3);
        s->cpu.dx = 0;
        return 0;
    case 0x0900: /* Get/set virtual interrupt state */
        s->cpu.ax = (uint16_t)((s->cpu.ax & 0xFF00) | ((s->cpu.flags & 0x200) ? 1 : 0));
        return 0;
    case 0x0901:
        s->cpu.flags |= 0x200;
        s->eflags |= 0x200;
        return 0;
    case 0x0902:
        s->cpu.flags &= (uint16_t)~0x200;
        s->eflags &= ~0x200u;
        return 0;
    default:
        s->cpu.flags |= 1;
        s->cpu.ax = 0x8001; /* unsupported */
        return 0;
    }
}
