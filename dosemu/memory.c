#include "dosemu_priv.h"
#include "../lib/memory.h"
#include "../lib/string.h"

uint32_t dos_seg_off(uint16_t seg, uint16_t off) {
    return ((uint32_t)seg << 4) + (uint32_t)off;
}

int dos_mem_alloc(dos_session_t* s) {
    if (!s) return -1;
    if (s->mem) return 0;
    s->mem = (uint8_t*)kmalloc(DOS_MEM_SIZE);
    if (!s->mem) return -1;
    memset(s->mem, 0, DOS_MEM_SIZE);
    return 0;
}

void dos_mem_free(dos_session_t* s) {
    if (!s) return;
    if (s->vga13) {
        kfree(s->vga13);
        s->vga13 = NULL;
    }
    if (!s->mem) return;
    kfree(s->mem);
    s->mem = NULL;
}

uint8_t dos_read8(dos_session_t* s, uint32_t linear) {
    if (!s || !s->mem || linear >= DOS_MEM_SIZE) return 0;
    return s->mem[linear];
}

uint16_t dos_read16(dos_session_t* s, uint32_t linear) {
    return (uint16_t)dos_read8(s, linear) |
           ((uint16_t)dos_read8(s, linear + 1) << 8);
}

void dos_write8(dos_session_t* s, uint32_t linear, uint8_t v) {
    if (!s || !s->mem || linear >= DOS_MEM_SIZE) return;
    s->mem[linear] = v;
}

void dos_write16(dos_session_t* s, uint32_t linear, uint16_t v) {
    dos_write8(s, linear, (uint8_t)(v & 0xFF));
    dos_write8(s, linear + 1, (uint8_t)((v >> 8) & 0xFF));
}

uint32_t dos_read32(dos_session_t* s, uint32_t linear) {
    return (uint32_t)dos_read16(s, linear) |
           ((uint32_t)dos_read16(s, linear + 2) << 16);
}

void dos_write32(dos_session_t* s, uint32_t linear, uint32_t v) {
    dos_write16(s, linear, (uint16_t)(v & 0xFFFF));
    dos_write16(s, linear + 2, (uint16_t)((v >> 16) & 0xFFFF));
}

void dos_setup_ivt(dos_session_t* s) {
    uint16_t vec;
    if (!s || !s->mem) return;
    dos_write8(s, dos_seg_off(DOS_IVT_IRET, 0), 0xCF); /* IRET */
    for (vec = 0; vec < 256; vec++) {
        dos_write16(s, (uint32_t)vec * 4u, 0);
        dos_write16(s, (uint32_t)vec * 4u + 2u, DOS_IVT_IRET);
    }
    dos_write16(s, 0x410, 0x0023); /* equipment: diskette + math coprocessor */
    dos_write16(s, 0x413, 640);
    dos_write16(s, 0x46C, 0);
    dos_write16(s, 0x46E, 0);
    /* keyboard buffer head/tail empty @ 40:1A / 40:1C */
    dos_write16(s, 0x41A, 0x1E);
    dos_write16(s, 0x41C, 0x1E);

    /* IBM AT system model byte at F000:FFFE (DOS/16M AT/PS2 probe) */
    dos_write8(s, 0xFFFFEul, 0xFC);
    dos_write8(s, 0xFFFFFul, 0x00);

    /* Minimal AT CMOS (ports 70h/71h) */
    memset(s->cmos_data, 0, sizeof(s->cmos_data));
    s->cmos_index = 0;
    s->cmos_data[0x0E] = 0x00; /* diagnostic: OK */
    s->cmos_data[0x10] = 0x40; /* one 1.44MB floppy */
    s->cmos_data[0x14] = 0x21; /* equipment */

    /* INT 15h AH=C0h ROM config table at F000:0200 (0100 is DPMI stub) */
    {
        uint32_t t = dos_seg_off(DOS_IVT_IRET, 0x0200);
        dos_write16(s, t + 0, 8);      /* length of following bytes */
        dos_write8(s, t + 2, 0xFC);    /* model: IBM AT */
        dos_write8(s, t + 3, 0x01);    /* submodel (<4 → DOS/16M ROM fallback) */
        dos_write8(s, t + 4, 0x00);    /* BIOS revision */
        dos_write8(s, t + 5, 0x70);    /* feature1: 2nd 8259, RTC, etc. */
        dos_write8(s, t + 6, 0x00);
        dos_write8(s, t + 7, 0x00);
        dos_write8(s, t + 8, 0x00);
        dos_write8(s, t + 9, 0x00);
    }
}

int dos_ivt_is_default(dos_session_t* s, uint8_t vec) {
    uint32_t a = (uint32_t)vec * 4u;
    uint16_t ip = dos_read16(s, a);
    uint16_t cs = dos_read16(s, a + 2);
    return (cs == DOS_IVT_IRET && ip == 0);
}

void dos_soft_int(dos_session_t* s, uint8_t vec) {
    uint32_t a = (uint32_t)vec * 4u;
    uint16_t ip = dos_read16(s, a);
    uint16_t cs = dos_read16(s, a + 2);
    if (!s || dos_ivt_is_default(s, vec)) return;
    if (s->cpu.sp < 6) return;
    s->cpu.sp = (uint16_t)(s->cpu.sp - 2);
    dos_write16(s, dos_seg_off(s->cpu.ss, s->cpu.sp), s->cpu.flags);
    s->cpu.sp = (uint16_t)(s->cpu.sp - 2);
    dos_write16(s, dos_seg_off(s->cpu.ss, s->cpu.sp), s->cpu.cs);
    s->cpu.sp = (uint16_t)(s->cpu.sp - 2);
    dos_write16(s, dos_seg_off(s->cpu.ss, s->cpu.sp), s->cpu.ip);
    s->cpu.cs = cs;
    s->cpu.ip = ip;
    s->cpu.flags &= (uint16_t)~0x0200;
}

void dos_setup_psp(dos_session_t* s, const char* cmdline) {
    uint32_t psp;
    uint32_t env;
    size_t i, clen;
    const char* cmd = cmdline ? cmdline : "";
    if (!s || !s->mem) return;
    /* Caller may pre-set psp_seg (EXE: block base); otherwise use defaults */
    if (!s->psp_seg) s->psp_seg = DOS_PSP_SEG;
    if (!s->env_seg) s->env_seg = DOS_ENV_SEG;
    psp = dos_seg_off(s->psp_seg, 0);
    env = dos_seg_off(s->env_seg, 0);

    dos_write8(s, env + 0, 'C');
    dos_write8(s, env + 1, 'O');
    dos_write8(s, env + 2, 'M');
    dos_write8(s, env + 3, 'S');
    dos_write8(s, env + 4, 'P');
    dos_write8(s, env + 5, 'E');
    dos_write8(s, env + 6, 'C');
    dos_write8(s, env + 7, '=');
    dos_write8(s, env + 8, 'C');
    dos_write8(s, env + 9, ':');
    dos_write8(s, env + 10, '\\');
    dos_write8(s, env + 11, 'C');
    dos_write8(s, env + 12, 'O');
    dos_write8(s, env + 13, 'M');
    dos_write8(s, env + 14, 'M');
    dos_write8(s, env + 15, 'A');
    dos_write8(s, env + 16, 'N');
    dos_write8(s, env + 17, 'D');
    dos_write8(s, env + 18, '.');
    dos_write8(s, env + 19, 'C');
    dos_write8(s, env + 20, 'O');
    dos_write8(s, env + 21, 'M');
    dos_write8(s, env + 22, 0);
    dos_write8(s, env + 23, 0);
    dos_write8(s, env + 24, 1);
    dos_write8(s, env + 25, 0);
    /* Full guest path — DOS/16M reopens the bound EXE for the LE image */
    for (i = 0; i < 80 && s->path[i]; i++)
        dos_write8(s, env + 26 + (uint32_t)i, (uint8_t)s->path[i]);
    dos_write8(s, env + 26 + (uint32_t)i, 0);

    dos_write16(s, psp + 0x00, 0x20CD);
    dos_write16(s, psp + 0x02, DOS_MEM_TOP_SEG);
    dos_write8(s, psp + 0x05, 0xEA);
    dos_write16(s, psp + 0x06, 0);
    dos_write16(s, psp + 0x08, DOS_IVT_IRET);
    dos_write16(s, psp + 0x2C, s->env_seg);
    dos_write16(s, psp + 0x32, DOS_MAX_FILES);
    dos_write16(s, psp + 0x34, 0x18);
    dos_write16(s, psp + 0x36, s->psp_seg);
    dos_write8(s, psp + 0x18, 0);
    dos_write8(s, psp + 0x19, 1);
    dos_write8(s, psp + 0x1A, 2);
    for (i = 3; i < 20; i++) dos_write8(s, psp + 0x18 + (uint32_t)i, 0xFF);

    while (*cmd == ' ') cmd++;
    clen = 0;
    while (cmd[clen] && clen < 126) clen++;
    dos_write8(s, psp + 0x80, (uint8_t)clen);
    for (i = 0; i < clen; i++)
        dos_write8(s, psp + 0x81 + (uint32_t)i, (uint8_t)cmd[i]);
    dos_write8(s, psp + 0x81 + (uint32_t)clen, 0x0D);

    s->dta_seg = s->psp_seg;
    s->dta_off = 0x80;
    s->find_index = -1;
    s->guest_cwd[0] = '\0';
}

/* ---- MCB arena ---- */
static uint8_t mcb_type(dos_session_t* s, uint16_t seg) {
    return dos_read8(s, dos_seg_off(seg, 0));
}
static uint16_t mcb_owner(dos_session_t* s, uint16_t seg) {
    return dos_read16(s, dos_seg_off(seg, 1));
}
static uint16_t mcb_size(dos_session_t* s, uint16_t seg) {
    return dos_read16(s, dos_seg_off(seg, 3));
}
static void mcb_set(dos_session_t* s, uint16_t seg, uint8_t typ, uint16_t owner, uint16_t sz) {
    dos_write8(s, dos_seg_off(seg, 0), typ);
    dos_write16(s, dos_seg_off(seg, 1), owner);
    dos_write16(s, dos_seg_off(seg, 3), sz);
}

void dos_mcb_init(dos_session_t* s, uint16_t first_seg, uint16_t end_seg) {
    uint16_t size;
    if (!s || first_seg >= end_seg) return;
    /* MCB at first_seg; usable block at first_seg+1 .. end_seg */
    size = (uint16_t)(end_seg - first_seg - 1u);
    mcb_set(s, first_seg, 'Z', 0, size);
    s->mcb_first = first_seg;
}

static void mcb_coalesce(dos_session_t* s) {
    uint16_t cur = s->mcb_first;
    for (;;) {
        uint8_t t = mcb_type(s, cur);
        uint16_t sz = mcb_size(s, cur);
        uint16_t next;
        if (t != 'M' && t != 'Z') break;
        if (t == 'Z') break;
        next = (uint16_t)(cur + sz + 1u);
        if (mcb_owner(s, cur) == 0 && mcb_owner(s, next) == 0) {
            uint8_t nt = mcb_type(s, next);
            uint16_t nsz = mcb_size(s, next);
            mcb_set(s, cur, nt, 0, (uint16_t)(sz + 1u + nsz));
            continue;
        }
        cur = next;
    }
}

int dos_mcb_alloc(dos_session_t* s, uint16_t paras, uint16_t* out_seg) {
    uint16_t cur;
    if (!s || !paras) return -1;
    mcb_coalesce(s);
    cur = s->mcb_first;
    for (;;) {
        uint8_t t = mcb_type(s, cur);
        uint16_t sz = mcb_size(s, cur);
        uint16_t owner = mcb_owner(s, cur);
        if (t != 'M' && t != 'Z') return -1;
        if (owner == 0 && sz >= paras) {
            if (sz > paras + 1u) {
                uint16_t rem = (uint16_t)(sz - paras - 1u);
                uint16_t nseg = (uint16_t)(cur + paras + 1u);
                mcb_set(s, cur, 'M', s->psp_seg, paras);
                mcb_set(s, nseg, t, 0, rem);
            } else {
                mcb_set(s, cur, t, s->psp_seg, sz);
            }
            if (out_seg) *out_seg = (uint16_t)(cur + 1u);
            return 0;
        }
        if (t == 'Z') break;
        cur = (uint16_t)(cur + sz + 1u);
    }
    return -1;
}

int dos_mcb_free(dos_session_t* s, uint16_t seg) {
    uint16_t mcb;
    if (!s || seg < 2) return -1;
    mcb = (uint16_t)(seg - 1u);
    if (mcb_owner(s, mcb) == 0) return -1;
    mcb_set(s, mcb, mcb_type(s, mcb), 0, mcb_size(s, mcb));
    mcb_coalesce(s);
    return 0;
}

int dos_mcb_resize(dos_session_t* s, uint16_t seg, uint16_t paras, uint16_t* out_max) {
    uint16_t mcb, sz, owner;
    uint8_t t;
    if (!s || seg < 2) return -1;
    mcb = (uint16_t)(seg - 1u);
    t = mcb_type(s, mcb);
    sz = mcb_size(s, mcb);
    owner = mcb_owner(s, mcb);
    if (owner == 0) return -1;
    if (out_max) *out_max = sz;
    if (paras == sz) return 0;
    if (paras < sz) {
        if (sz > paras + 1u) {
            uint16_t nseg = (uint16_t)(mcb + paras + 1u);
            uint8_t nt = t;
            mcb_set(s, mcb, 'M', owner, paras);
            mcb_set(s, nseg, nt, 0, (uint16_t)(sz - paras - 1u));
            mcb_coalesce(s);
        } else {
            mcb_set(s, mcb, t, owner, sz);
        }
        return 0;
    }
    /* grow: need following free block */
    if (t == 'Z') return -1;
    {
        uint16_t next = (uint16_t)(mcb + sz + 1u);
        uint16_t nsz = mcb_size(s, next);
        uint8_t nt = mcb_type(s, next);
        uint16_t need = (uint16_t)(paras - sz);
        if (mcb_owner(s, next) != 0 || nsz + 1u < need) {
            if (out_max) *out_max = (uint16_t)(sz + (mcb_owner(s, next) == 0 ? nsz + 1u : 0));
            return -1;
        }
        if (nsz + 1u == need) {
            mcb_set(s, mcb, nt, owner, paras);
        } else {
            uint16_t rem = (uint16_t)(nsz - need);
            uint16_t nseg = (uint16_t)(mcb + paras + 1u);
            mcb_set(s, mcb, 'M', owner, paras);
            mcb_set(s, nseg, nt, 0, rem);
        }
    }
    return 0;
}
