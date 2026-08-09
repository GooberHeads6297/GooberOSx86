#include "dosemu_priv.h"

#define CF 0x0001u
#define PF 0x0004u
#define AF 0x0010u
#define ZF 0x0040u
#define SF 0x0080u
#define TF 0x0100u
#define IF 0x0200u
#define DF 0x0400u
#define OF 0x0800u

void cpu8086_reset(cpu8086_t* c, uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp) {
    if (!c) return;
    c->ax = c->bx = c->cx = c->dx = 0;
    c->si = c->di = c->bp = 0;
    c->sp = sp;
    c->cs = cs;
    c->ds = cs;
    c->es = cs;
    c->ss = ss;
    c->ip = ip;
    c->flags = 0x0202;
}

static uint32_t linear_seg_off(dos_session_t* s, uint16_t seg, uint16_t off) {
    if (!s->pe) return dos_seg_off(seg, off);
    if (seg == s->cpu.cs) return pm_ea(s, &s->seg_cs, off);
    if (seg == s->cpu.ds) return pm_ea(s, &s->seg_ds, off);
    if (seg == s->cpu.es) return pm_ea(s, &s->seg_es, off);
    if (seg == s->cpu.ss) return pm_ea(s, &s->seg_ss, off);
    if (seg == s->fs) return pm_ea(s, &s->seg_fs, off);
    if (seg == s->gs) return pm_ea(s, &s->seg_gs, off);
    {
        pm_seg_t tmp;
        if (pm_load_selector(s, seg, &tmp) == 0) return pm_ea(s, &tmp, off);
    }
    return dos_seg_off(seg, off);
}

static void load_sreg(dos_session_t* s, int which, uint16_t val) {
    pm_seg_t tmp;
    if (pm_load_selector(s, val, &tmp) != 0) {
        tmp.sel = val;
        tmp.base = (uint32_t)val << 4;
        tmp.limit = 0xFFFF;
        tmp.present = 1;
        tmp.use32 = 0;
        tmp.code = 0;
        tmp.readable = 1;
        tmp.writable = 1;
    }
    switch (which) {
    case 0: s->cpu.es = val; s->seg_es = tmp; break;
    case 1: s->cpu.cs = val; s->seg_cs = tmp; s->cpu32 = (s->pe && tmp.use32) ? 1 : 0; break;
    case 2: s->cpu.ss = val; s->seg_ss = tmp; break;
    case 3: s->cpu.ds = val; s->seg_ds = tmp; break;
    case 4: s->fs = val; s->seg_fs = tmp; break;
    case 5: s->gs = val; s->seg_gs = tmp; break;
    }
}

static uint8_t fetch8(dos_session_t* s) {
    uint8_t v = dos_read8(s, linear_seg_off(s, s->cpu.cs, s->cpu.ip));
    s->cpu.ip++;
    s->eip = (s->eip & 0xFFFF0000u) | s->cpu.ip;
    return v;
}

static uint16_t fetch16(dos_session_t* s) {
    uint16_t v = dos_read16(s, linear_seg_off(s, s->cpu.cs, s->cpu.ip));
    s->cpu.ip = (uint16_t)(s->cpu.ip + 2);
    s->eip = (s->eip & 0xFFFF0000u) | s->cpu.ip;
    return v;
}

static uint16_t* reg16(cpu8086_t* c, int r) {
    switch (r & 7) {
    case 0: return &c->ax;
    case 1: return &c->cx;
    case 2: return &c->dx;
    case 3: return &c->bx;
    case 4: return &c->sp;
    case 5: return &c->bp;
    case 6: return &c->si;
    default: return &c->di;
    }
}

static uint8_t get_reg8(cpu8086_t* c, int r) {
    uint16_t v;
    switch (r & 7) {
    case 0: return (uint8_t)(c->ax & 0xFF);
    case 1: return (uint8_t)(c->cx & 0xFF);
    case 2: return (uint8_t)(c->dx & 0xFF);
    case 3: return (uint8_t)(c->bx & 0xFF);
    case 4: return (uint8_t)((c->ax >> 8) & 0xFF);
    case 5: return (uint8_t)((c->cx >> 8) & 0xFF);
    case 6: return (uint8_t)((c->dx >> 8) & 0xFF);
    default: return (uint8_t)((c->bx >> 8) & 0xFF);
    }
    (void)v;
}

static void set_reg8(cpu8086_t* c, int r, uint8_t val) {
    switch (r & 7) {
    case 0: c->ax = (uint16_t)((c->ax & 0xFF00) | val); break;
    case 1: c->cx = (uint16_t)((c->cx & 0xFF00) | val); break;
    case 2: c->dx = (uint16_t)((c->dx & 0xFF00) | val); break;
    case 3: c->bx = (uint16_t)((c->bx & 0xFF00) | val); break;
    case 4: c->ax = (uint16_t)((c->ax & 0x00FF) | ((uint16_t)val << 8)); break;
    case 5: c->cx = (uint16_t)((c->cx & 0x00FF) | ((uint16_t)val << 8)); break;
    case 6: c->dx = (uint16_t)((c->dx & 0x00FF) | ((uint16_t)val << 8)); break;
    case 7: c->bx = (uint16_t)((c->bx & 0x00FF) | ((uint16_t)val << 8)); break;
    }
}

static void set_szp8(cpu8086_t* c, uint8_t v) {
    int bits, i;
    if (v == 0) c->flags |= ZF; else c->flags &= (uint16_t)~ZF;
    if (v & 0x80) c->flags |= SF; else c->flags &= (uint16_t)~SF;
    bits = 0;
    for (i = 0; i < 8; i++) if (v & (1u << i)) bits++;
    if ((bits & 1) == 0) c->flags |= PF; else c->flags &= (uint16_t)~PF;
}

static void set_szp16(cpu8086_t* c, uint16_t v) {
    if (v == 0) c->flags |= ZF; else c->flags &= (uint16_t)~ZF;
    if (v & 0x8000) c->flags |= SF; else c->flags &= (uint16_t)~SF;
    set_szp8(c, (uint8_t)(v & 0xFF)); /* PF from low byte */
}

static uint16_t seg_for_ea(dos_session_t* s, uint8_t rm, uint8_t mod) {
    if (s->has_seg_override) return s->seg_override;
    if (rm == 2 || rm == 3 || (rm == 6 && mod != 0)) return s->cpu.ss;
    return s->cpu.ds;
}

static uint32_t ea_addr(dos_session_t* s, uint8_t modrm) {
    cpu8086_t* c = &s->cpu;
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    uint8_t rm = (uint8_t)(modrm & 7);
    int16_t disp = 0;
    uint16_t seg, off = 0;

    if (mod == 1) disp = (int8_t)fetch8(s);
    else if (mod == 2 || (mod == 0 && rm == 6)) disp = (int16_t)fetch16(s);

    switch (rm) {
    case 0: off = (uint16_t)(c->bx + c->si + disp); break;
    case 1: off = (uint16_t)(c->bx + c->di + disp); break;
    case 2: off = (uint16_t)(c->bp + c->si + disp); break;
    case 3: off = (uint16_t)(c->bp + c->di + disp); break;
    case 4: off = (uint16_t)(c->si + disp); break;
    case 5: off = (uint16_t)(c->di + disp); break;
    case 6:
        if (mod == 0) off = (uint16_t)disp;
        else off = (uint16_t)(c->bp + disp);
        break;
    case 7: off = (uint16_t)(c->bx + disp); break;
    }
    seg = seg_for_ea(s, rm, mod);
    return linear_seg_off(s, seg, off);
}

static uint8_t read_rm8(dos_session_t* s, uint8_t modrm) {
    if (((modrm >> 6) & 3) == 3) return get_reg8(&s->cpu, modrm & 7);
    return dos_read8(s, ea_addr(s, modrm));
}

static void write_rm8(dos_session_t* s, uint8_t modrm, uint8_t v) {
    if (((modrm >> 6) & 3) == 3) set_reg8(&s->cpu, modrm & 7, v);
    else dos_write8(s, ea_addr(s, modrm), v);
}

static uint16_t read_rm16(dos_session_t* s, uint8_t modrm) {
    if (((modrm >> 6) & 3) == 3) return *reg16(&s->cpu, modrm & 7);
    return dos_read16(s, ea_addr(s, modrm));
}

static void write_rm16(dos_session_t* s, uint8_t modrm, uint16_t v) {
    if (((modrm >> 6) & 3) == 3) *reg16(&s->cpu, modrm & 7) = v;
    else dos_write16(s, ea_addr(s, modrm), v);
}

/* Note: write_rm after read_rm re-parses modrm and re-fetches disp — broken!
 * Need to parse EA once. Fix with helper that caches. */

typedef struct {
    int is_reg;
    int reg;
    uint32_t addr;
} ea_t;

static ea_t parse_ea(dos_session_t* s, uint8_t modrm) {
    ea_t e;
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    if (mod == 3) {
        e.is_reg = 1;
        e.reg = modrm & 7;
        e.addr = 0;
        return e;
    }
    e.is_reg = 0;
    e.reg = 0;
    e.addr = ea_addr(s, modrm);
    return e;
}

static uint8_t ea_read8(dos_session_t* s, ea_t e) {
    if (e.is_reg) return get_reg8(&s->cpu, e.reg);
    return dos_read8(s, e.addr);
}

static void ea_write8(dos_session_t* s, ea_t e, uint8_t v) {
    if (e.is_reg) set_reg8(&s->cpu, e.reg, v);
    else dos_write8(s, e.addr, v);
}

static uint16_t ea_read16(dos_session_t* s, ea_t e) {
    if (e.is_reg) return *reg16(&s->cpu, e.reg);
    return dos_read16(s, e.addr);
}

static void ea_write16(dos_session_t* s, ea_t e, uint16_t v) {
    if (e.is_reg) *reg16(&s->cpu, e.reg) = v;
    else dos_write16(s, e.addr, v);
}

static void push16(dos_session_t* s, uint16_t v) {
    s->cpu.sp = (uint16_t)(s->cpu.sp - 2);
    dos_write16(s, linear_seg_off(s, s->cpu.ss, s->cpu.sp), v);
}

static uint16_t pop16(dos_session_t* s) {
    uint16_t v = dos_read16(s, linear_seg_off(s, s->cpu.ss, s->cpu.sp));
    s->cpu.sp = (uint16_t)(s->cpu.sp + 2);
    return v;
}

static void set_of_add8(cpu8086_t* c, uint8_t a, uint8_t b, uint8_t r) {
    if ((uint8_t)((a ^ r) & (b ^ r) & 0x80)) c->flags |= OF; else c->flags &= (uint16_t)~OF;
}

static void set_of_sub8(cpu8086_t* c, uint8_t a, uint8_t b, uint8_t r) {
    if ((uint8_t)((a ^ b) & (a ^ r) & 0x80)) c->flags |= OF; else c->flags &= (uint16_t)~OF;
}

static void set_of_add16(cpu8086_t* c, uint16_t a, uint16_t b, uint16_t r) {
    if ((uint16_t)((a ^ r) & (b ^ r) & 0x8000)) c->flags |= OF; else c->flags &= (uint16_t)~OF;
}

static void set_of_sub16(cpu8086_t* c, uint16_t a, uint16_t b, uint16_t r) {
    if ((uint16_t)((a ^ b) & (a ^ r) & 0x8000)) c->flags |= OF; else c->flags &= (uint16_t)~OF;
}

static void alu8(cpu8086_t* c, int op, uint8_t* dst, uint8_t src) {
    uint16_t r;
    uint8_t a = *dst, res, cf;
    switch (op) {
    case 0: /* ADD */
        r = (uint16_t)a + src; res = (uint8_t)r; *dst = res;
        if (r > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ res) ^ src) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_add8(c, a, src, res); set_szp8(c, res); break;
    case 1: r = (uint16_t)a | src; *dst = (uint8_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp8(c, *dst); break;
    case 2: /* ADC */
        cf = (c->flags & CF) ? 1u : 0u;
        r = (uint16_t)a + src + cf; res = (uint8_t)r; *dst = res;
        if (r > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ res) ^ src) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_add8(c, a, src, res); set_szp8(c, res); break;
    case 3: /* SBB */
        cf = (c->flags & CF) ? 1u : 0u;
        r = (uint16_t)a - src - cf; res = (uint8_t)r; *dst = res;
        if (r > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ src) ^ res) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_sub8(c, a, src, res); set_szp8(c, res); break;
    case 4: r = (uint16_t)a & src; *dst = (uint8_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp8(c, *dst); break;
    case 5: case 7: /* SUB / CMP */
        r = (uint16_t)a - src; res = (uint8_t)r;
        if (op == 5) *dst = res;
        if (r > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ src) ^ res) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_sub8(c, a, src, res); set_szp8(c, res); break;
    case 6: r = (uint16_t)a ^ src; *dst = (uint8_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp8(c, *dst); break;
    }
}

static void alu16(cpu8086_t* c, int op, uint16_t* dst, uint16_t src) {
    uint32_t r;
    uint16_t a = *dst, res, cf;
    switch (op) {
    case 0: /* ADD */
        r = (uint32_t)a + src; res = (uint16_t)r; *dst = res;
        if (r > 0xFFFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ res) ^ src) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_add16(c, a, src, res); set_szp16(c, res); break;
    case 1: r = (uint32_t)a | src; *dst = (uint16_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp16(c, *dst); break;
    case 2: /* ADC */
        cf = (c->flags & CF) ? 1u : 0u;
        r = (uint32_t)a + src + cf; res = (uint16_t)r; *dst = res;
        if (r > 0xFFFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ res) ^ src) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_add16(c, a, src, res); set_szp16(c, res); break;
    case 3: /* SBB */
        cf = (c->flags & CF) ? 1u : 0u;
        r = (uint32_t)a - src - cf; res = (uint16_t)r; *dst = res;
        if (r > 0xFFFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ src) ^ res) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_sub16(c, a, src, res); set_szp16(c, res); break;
    case 4: r = (uint32_t)a & src; *dst = (uint16_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp16(c, *dst); break;
    case 5: case 7: /* SUB / CMP */
        r = (uint32_t)a - src; res = (uint16_t)r;
        if (op == 5) *dst = res;
        if (r > 0xFFFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
        if (((a ^ src) ^ res) & 0x10) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        set_of_sub16(c, a, src, res); set_szp16(c, res); break;
    case 6: r = (uint32_t)a ^ src; *dst = (uint16_t)r; c->flags &= (uint16_t)~(CF|OF); set_szp16(c, *dst); break;
    }
}

static int jcc(cpu8086_t* c, uint8_t cc) {
    int cf = (c->flags & CF) != 0;
    int zf = (c->flags & ZF) != 0;
    int sf = (c->flags & SF) != 0;
    int of = (c->flags & OF) != 0;
    int pf = (c->flags & PF) != 0;
    switch (cc) {
    case 0x0: return of; case 0x1: return !of;
    case 0x2: return cf; case 0x3: return !cf;
    case 0x4: return zf; case 0x5: return !zf;
    case 0x6: return cf || zf; case 0x7: return !cf && !zf;
    case 0x8: return sf; case 0x9: return !sf;
    case 0xA: return pf; case 0xB: return !pf;
    case 0xC: return sf != of; case 0xD: return sf == of;
    case 0xE: return zf || (sf != of); case 0xF: return !zf && (sf == of);
    default: return 0;
    }
}

void dos_out_port(dos_session_t* s, uint16_t port, uint8_t val) {
    if (!s) return;
    if (port == 0x3C8) {
        s->vga_dac_idx = val;
        s->vga_dac_comp = 0;
        return;
    }
    if (port == 0x3C9) {
        s->vga_pal[s->vga_dac_idx][s->vga_dac_comp] = (uint8_t)(val & 0x3F);
        s->vga_dac_comp++;
        if (s->vga_dac_comp >= 3) {
            s->vga_dac_comp = 0;
            s->vga_dac_idx++;
        }
        return;
    }
    /* CMOS index (AT) — NMI-disable bit in 0x80 ignored for data path */
    if (port == 0x70) {
        s->cmos_index = (uint8_t)(val & 0x7F);
        return;
    }
    if (port == 0x71) {
        s->cmos_data[s->cmos_index & 0x7F] = val;
        return;
    }
    if (port == 0x92) return; /* FAST A20 — guest arena is always flat/A20-on */
    if (port == 0x43) { /* 8253 PIT mode */
        s->pit_mode = val;
        s->pit_latch_lo = 1;
        return;
    }
    if (port == 0x40) {
        if (s->pit_latch_lo) {
            s->pit_reload = (uint16_t)((s->pit_reload & 0xFF00u) | val);
            s->pit_latch_lo = 0;
        } else {
            s->pit_reload = (uint16_t)((s->pit_reload & 0x00FFu) | ((uint16_t)val << 8));
            s->pit_count = s->pit_reload ? s->pit_reload : 0xFFFFu;
            s->pit_latch_lo = 1;
        }
        return;
    }
    /* Sound Blaster / AdLib — ignore silently */
    if ((port >= 0x220 && port <= 0x22F) || port == 0x388 || port == 0x389)
        return;
    (void)val;
}

uint8_t dos_in_port(dos_session_t* s, uint16_t port) {
    if (!s) return 0xFF;
    if (port == 0x3DA) {
        s->vga_status_toggle ^= 1;
        return s->vga_status_toggle ? (uint8_t)0x08 : (uint8_t)0x01;
    }
    if (port == 0x70) return s->cmos_index;
    if (port == 0x71) return s->cmos_data[s->cmos_index & 0x7F];
    if (port == 0x40) {
        /* Countdown so busy-waits on the PIT eventually complete */
        uint16_t v = s->pit_count;
        if (s->pit_count > 0) s->pit_count--;
        else s->pit_count = s->pit_reload ? s->pit_reload : 0xFFFFu;
        if (s->pit_latch_lo) {
            s->pit_latch_lo = 0;
            return (uint8_t)(v & 0xFF);
        }
        s->pit_latch_lo = 1;
        return (uint8_t)((v >> 8) & 0xFF);
    }
    /* AT PPI / system control ports — benign defaults */
    if (port == 0x61) return 0x00;
    if (port == 0x64) return 0x1C; /* keyboard ctrl: self-test done, no data */
    if (port == 0x92) return 0x02; /* A20 enabled */
    return 0xFF;
}

static void shift_rm(dos_session_t* s, int w, int grp, ea_t e, int count) {
    cpu8086_t* c = &s->cpu;
    int i;
    if (count <= 0) return;
    if (!w) {
        uint8_t v = ea_read8(s, e);
        for (i = 0; i < count; i++) {
            uint8_t cf;
            if (grp == 0) { /* ROL */
                cf = (uint8_t)((v >> 7) & 1); v = (uint8_t)((v << 1) | cf);
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 1) { /* ROR */
                cf = (uint8_t)(v & 1); v = (uint8_t)((v >> 1) | (cf << 7));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 2) { /* RCL */
                cf = (uint8_t)((v >> 7) & 1);
                v = (uint8_t)((v << 1) | ((c->flags & CF) ? 1 : 0));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 3) { /* RCR */
                cf = (uint8_t)(v & 1);
                v = (uint8_t)((v >> 1) | ((c->flags & CF) ? 0x80 : 0));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 4) { /* SHL */
                if (v & 0x80) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint8_t)(v << 1);
            } else if (grp == 5) { /* SHR */
                if (v & 1) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint8_t)(v >> 1);
            } else if (grp == 7) { /* SAR */
                if (v & 1) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint8_t)((int8_t)v >> 1);
            }
        }
        ea_write8(s, e, v); set_szp8(c, v);
        if (count == 1 && (grp == 0 || grp == 2 || grp == 4)) {
            uint8_t of = (uint8_t)(((v >> 7) ^ ((c->flags & CF) ? 1 : 0)) & 1);
            if (of) c->flags |= OF; else c->flags &= (uint16_t)~OF;
        } else if (count == 1 && (grp == 1 || grp == 3)) {
            if (((v >> 6) ^ (v >> 7)) & 1) c->flags |= OF; else c->flags &= (uint16_t)~OF;
        } else if (count == 1 && grp == 5) {
            if (v & 0x80) c->flags |= OF; else c->flags &= (uint16_t)~OF;
        } else if (count == 1 && grp == 7) {
            c->flags &= (uint16_t)~OF;
        }
    } else {
        uint16_t v = ea_read16(s, e);
        for (i = 0; i < count; i++) {
            uint16_t cf;
            if (grp == 0) {
                cf = (uint16_t)((v >> 15) & 1); v = (uint16_t)((v << 1) | cf);
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 1) {
                cf = (uint16_t)(v & 1); v = (uint16_t)((v >> 1) | (cf << 15));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 2) { /* RCL */
                cf = (uint16_t)((v >> 15) & 1);
                v = (uint16_t)((v << 1) | ((c->flags & CF) ? 1 : 0));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 3) { /* RCR */
                cf = (uint16_t)(v & 1);
                v = (uint16_t)((v >> 1) | ((c->flags & CF) ? 0x8000 : 0));
                if (cf) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            } else if (grp == 4) {
                if (v & 0x8000) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint16_t)(v << 1);
            } else if (grp == 5) {
                if (v & 1) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint16_t)(v >> 1);
            } else if (grp == 7) {
                if (v & 1) c->flags |= CF; else c->flags &= (uint16_t)~CF;
                v = (uint16_t)((int16_t)v >> 1);
            }
        }
        ea_write16(s, e, v); set_szp16(c, v);
    }
}

static void do_string_op(dos_session_t* s, uint8_t op) {
    cpu8086_t* c = &s->cpu;
    int step = (c->flags & DF) ? -1 : 1;
    uint16_t ds = s->has_seg_override ? s->seg_override : c->ds;

    if (op == 0xA4) { /* MOVSB */
        uint8_t v = dos_read8(s, linear_seg_off(s, ds, c->si));
        dos_write8(s, linear_seg_off(s, c->es, c->di), v);
        c->si = (uint16_t)(c->si + step);
        c->di = (uint16_t)(c->di + step);
    } else if (op == 0xA5) { /* MOVSW */
        uint16_t v = dos_read16(s, linear_seg_off(s, ds, c->si));
        dos_write16(s, linear_seg_off(s, c->es, c->di), v);
        c->si = (uint16_t)(c->si + step * 2);
        c->di = (uint16_t)(c->di + step * 2);
    } else if (op == 0xAA) { /* STOSB */
        dos_write8(s, linear_seg_off(s, c->es, c->di), get_reg8(c, 0));
        c->di = (uint16_t)(c->di + step);
    } else if (op == 0xAB) { /* STOSW */
        dos_write16(s, linear_seg_off(s, c->es, c->di), c->ax);
        c->di = (uint16_t)(c->di + step * 2);
    } else if (op == 0xAC) { /* LODSB */
        set_reg8(c, 0, dos_read8(s, linear_seg_off(s, ds, c->si)));
        c->si = (uint16_t)(c->si + step);
    } else if (op == 0xAD) { /* LODSW */
        c->ax = dos_read16(s, linear_seg_off(s, ds, c->si));
        c->si = (uint16_t)(c->si + step * 2);
    } else if (op == 0xA6) { /* CMPSB */
        uint8_t a = dos_read8(s, linear_seg_off(s, ds, c->si));
        uint8_t b = dos_read8(s, linear_seg_off(s, c->es, c->di));
        uint8_t tmp = a;
        alu8(c, 7, &tmp, b);
        c->si = (uint16_t)(c->si + step);
        c->di = (uint16_t)(c->di + step);
    } else if (op == 0xA7) { /* CMPSW */
        uint16_t a = dos_read16(s, linear_seg_off(s, ds, c->si));
        uint16_t b = dos_read16(s, linear_seg_off(s, c->es, c->di));
        uint16_t tmp = a;
        alu16(c, 7, &tmp, b);
        c->si = (uint16_t)(c->si + step * 2);
        c->di = (uint16_t)(c->di + step * 2);
    } else if (op == 0xAE) { /* SCASB */
        uint8_t a = get_reg8(c, 0);
        uint8_t b = dos_read8(s, linear_seg_off(s, c->es, c->di));
        alu8(c, 7, &a, b);
        c->di = (uint16_t)(c->di + step);
    } else if (op == 0xAF) { /* SCASW */
        uint16_t a = c->ax;
        uint16_t b = dos_read16(s, linear_seg_off(s, c->es, c->di));
        alu16(c, 7, &a, b);
        c->di = (uint16_t)(c->di + step * 2);
    } else if (op == 0x6C) { /* INSB */
        uint8_t v = dos_in_port(s, c->dx);
        dos_write8(s, linear_seg_off(s, c->es, c->di), v);
        c->di = (uint16_t)(c->di + step);
    } else if (op == 0x6D) { /* INSW */
        uint8_t lo = dos_in_port(s, c->dx);
        uint8_t hi = dos_in_port(s, c->dx);
        dos_write16(s, linear_seg_off(s, c->es, c->di),
                    (uint16_t)(lo | ((uint16_t)hi << 8)));
        c->di = (uint16_t)(c->di + step * 2);
    } else if (op == 0x6E) { /* OUTSB */
        uint8_t v = dos_read8(s, linear_seg_off(s, ds, c->si));
        dos_out_port(s, c->dx, v);
        c->si = (uint16_t)(c->si + step);
    } else if (op == 0x6F) { /* OUTSW */
        uint16_t v = dos_read16(s, linear_seg_off(s, ds, c->si));
        dos_out_port(s, c->dx, (uint8_t)(v & 0xFF));
        dos_out_port(s, c->dx, (uint8_t)((v >> 8) & 0xFF));
        c->si = (uint16_t)(c->si + step * 2);
    }
}

int cpu8086_step(dos_session_t* s) {
    uint8_t op;
    cpu8086_t* c;
    int prefixes = 0;
    if (!s || s->halted || !s->mem) return 0;
    c = &s->cpu;
    s->has_seg_override = 0;
    s->rep_prefix = 0;

    s->op32 = 0;
    s->addr32 = 0;

    /* prefixes (incl. 386 66/67 and FS/GS) */
    for (;;) {
        op = fetch8(s);
        if (op == 0x26) { s->seg_override = c->es; s->has_seg_override = 1; }
        else if (op == 0x2E) { s->seg_override = c->cs; s->has_seg_override = 1; }
        else if (op == 0x36) { s->seg_override = c->ss; s->has_seg_override = 1; }
        else if (op == 0x3E) { s->seg_override = c->ds; s->has_seg_override = 1; }
        else if (op == 0x64) { s->seg_override = s->fs; s->has_seg_override = 1; }
        else if (op == 0x65) { s->seg_override = s->gs; s->has_seg_override = 1; }
        else if (op == 0x66) { s->op32 = 1; }
        else if (op == 0x67) { s->addr32 = 1; }
        else if (op == 0x9B) { /* WAIT/FWAIT — no-op with soft FPU */ }
        else if (op == 0xF0) { /* LOCK — no-op on uniprocessor soft CPU */ }
        else if (op == 0xF3) { s->rep_prefix = 1; } /* REP/REPE */
        else if (op == 0xF2) { s->rep_prefix = 2; } /* REPNE */
        else break;
        if (++prefixes > 6) break;
    }

    /* x87 ESC D8–DF (e.g. FNINIT = DB E3) */
    if ((op & 0xF8) == 0xD8) {
        int r = cpu_exec_fpu(s, op);
        if (r >= 0) return r;
    }

    /* 32-bit operand/address or 0F escape → 386 helper */
    if (s->op32 || s->addr32 || op == 0x0F) {
        int r = cpu386_rm_after_prefix(s, op);
        if (r >= 0) return r;
        {
            char hx[12];
            hx[0] = "0123456789ABCDEF"[(op >> 4) & 0xF];
            hx[1] = "0123456789ABCDEF"[op & 0xF];
            hx[2] = '\0';
            dos_video_puts(s, "\r\nGooberDOS: 386 opcode ");
            dos_video_puts(s, hx);
            dos_video_puts(s, "h not implemented yet\r\n");
        }
        s->halted = 1;
        s->shell_reentry = 1;
        return 0;
    }

    /* REP string ops */
    if (s->rep_prefix && (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB ||
                          op == 0xAC || op == 0xAD || op == 0xA6 || op == 0xA7 ||
                          op == 0xAE || op == 0xAF ||
                          op == 0x6C || op == 0x6D || op == 0x6E || op == 0x6F)) {
        while (c->cx != 0) {
            do_string_op(s, op);
            c->cx--;
            if ((op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) &&
                s->rep_prefix == 1 && !(c->flags & ZF)) break;
            if ((op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) &&
                s->rep_prefix == 2 && (c->flags & ZF)) break;
        }
        return 1;
    }

    if (op >= 0xB0 && op <= 0xB7) { set_reg8(c, op - 0xB0, fetch8(s)); return 1; }
    if (op >= 0xB8 && op <= 0xBF) { *reg16(c, op - 0xB8) = fetch16(s); return 1; }
    if (op >= 0x40 && op <= 0x47) {
        uint16_t* r = reg16(c, op - 0x40);
        uint16_t a = *r, res = (uint16_t)(a + 1);
        *r = res; set_szp16(c, res); set_of_add16(c, a, 1, res);
        if ((a & 0x0F) == 0x0F) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        return 1;
    }
    if (op >= 0x48 && op <= 0x4F) {
        uint16_t* r = reg16(c, op - 0x48);
        uint16_t a = *r, res = (uint16_t)(a - 1);
        *r = res; set_szp16(c, res); set_of_sub16(c, a, 1, res);
        if ((a & 0x0F) == 0) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        return 1;
    }
    if (op >= 0x50 && op <= 0x57) { push16(s, *reg16(c, op - 0x50)); return 1; }
    if (op >= 0x58 && op <= 0x5F) { *reg16(c, op - 0x58) = pop16(s); return 1; }
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)fetch8(s);
        if (jcc(c, (uint8_t)(op - 0x70))) c->ip = (uint16_t)(c->ip + rel);
        return 1;
    }

    /* ALU AL/AX,imm */
    if ((op & 0xFE) == 0x04 || (op & 0xFE) == 0x0C || (op & 0xFE) == 0x14 ||
        (op & 0xFE) == 0x1C || (op & 0xFE) == 0x24 || (op & 0xFE) == 0x2C ||
        (op & 0xFE) == 0x34 || (op & 0xFE) == 0x3C) {
        int alu = (op >> 3) & 7;
        if (op & 1) {
            uint16_t imm = fetch16(s);
            alu16(c, alu, &c->ax, imm);
        } else {
            uint8_t imm = fetch8(s);
            uint8_t al = get_reg8(c, 0);
            alu8(c, alu, &al, imm);
            set_reg8(c, 0, al);
        }
        return 1;
    }

    /* ALU r/m, r  and r, r/m : 00-3F except prefixes handled */
    if (op < 0x40 && (op & 0x06) != 0x06) {
        int w = op & 1;
        int d = (op >> 1) & 1;
        int alu = (op >> 3) & 7;
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (!w) {
            uint8_t rm = ea_read8(s, e);
            uint8_t rv = get_reg8(c, reg);
            if (d) { alu8(c, alu, &rv, rm); set_reg8(c, reg, rv); }
            else { alu8(c, alu, &rm, rv); ea_write8(s, e, rm); }
        } else {
            uint16_t rm = ea_read16(s, e);
            uint16_t rv = *reg16(c, reg);
            if (d) { alu16(c, alu, &rv, rm); *reg16(c, reg) = rv; }
            else { alu16(c, alu, &rm, rv); ea_write16(s, e, rm); }
        }
        return 1;
    }

    switch (op) {
    case 0x90: return 1;
    case 0xF4: s->halted = 1; return 0;
    case 0xCD: {
        uint8_t vec = fetch8(s);
        if (dos_ivt_is_default(s, vec)) {
            if (dos_handle_int(s, vec) != 0) { s->halted = 1; return 0; }
            return s->halted ? 0 : 1;
        }
        dos_soft_int(s, vec);
        return s->halted ? 0 : 1;
    }
    case 0xCC: /* INT 3 */
        if (dos_ivt_is_default(s, 3)) {
            if (dos_handle_int(s, 3) != 0) { s->halted = 1; return 0; }
            return 1;
        }
        dos_soft_int(s, 3);
        return s->halted ? 0 : 1;
    case 0x27: { /* DAA */
        uint8_t al = get_reg8(c, 0);
        uint8_t old_al = al;
        uint8_t old_cf = (c->flags & CF) ? 1u : 0u;
        c->flags &= (uint16_t)~CF;
        if ((al & 0x0F) > 9 || (c->flags & AF)) {
            uint16_t t = (uint16_t)al + 6;
            al = (uint8_t)t;
            if (old_cf || t > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            c->flags |= AF;
        } else c->flags &= (uint16_t)~AF;
        if (old_al > 0x99 || old_cf) {
            al = (uint8_t)(al + 0x60);
            c->flags |= CF;
        }
        set_reg8(c, 0, al); set_szp8(c, al);
        return 1;
    }
    case 0x2F: { /* DAS */
        uint8_t al = get_reg8(c, 0);
        uint8_t old_al = al;
        uint8_t old_cf = (c->flags & CF) ? 1u : 0u;
        c->flags &= (uint16_t)~CF;
        if ((al & 0x0F) > 9 || (c->flags & AF)) {
            uint16_t t = (uint16_t)al - 6;
            al = (uint8_t)t;
            if (old_cf || t > 0xFF) c->flags |= CF; else c->flags &= (uint16_t)~CF;
            c->flags |= AF;
        } else c->flags &= (uint16_t)~AF;
        if (old_al > 0x99 || old_cf) {
            al = (uint8_t)(al - 0x60);
            c->flags |= CF;
        }
        set_reg8(c, 0, al); set_szp8(c, al);
        return 1;
    }
    case 0x37: { /* AAA */
        uint8_t al = get_reg8(c, 0);
        if ((al & 0x0F) > 9 || (c->flags & AF)) {
            set_reg8(c, 0, (uint8_t)(al + 6));
            set_reg8(c, 4, (uint8_t)(get_reg8(c, 4) + 1));
            c->flags |= AF | CF;
        } else c->flags &= (uint16_t)~(AF | CF);
        set_reg8(c, 0, (uint8_t)(get_reg8(c, 0) & 0x0F));
        return 1;
    }
    case 0x3F: { /* AAS */
        uint8_t al = get_reg8(c, 0);
        if ((al & 0x0F) > 9 || (c->flags & AF)) {
            set_reg8(c, 0, (uint8_t)(al - 6));
            set_reg8(c, 4, (uint8_t)(get_reg8(c, 4) - 1));
            c->flags |= AF | CF;
        } else c->flags &= (uint16_t)~(AF | CF);
        set_reg8(c, 0, (uint8_t)(get_reg8(c, 0) & 0x0F));
        return 1;
    }
    case 0xD4: { /* AAM */
        uint8_t base = fetch8(s);
        uint8_t al = get_reg8(c, 0);
        if (base == 0) { s->halted = 1; return 0; }
        set_reg8(c, 4, (uint8_t)(al / base));
        set_reg8(c, 0, (uint8_t)(al % base));
        set_szp8(c, get_reg8(c, 0));
        return 1;
    }
    case 0xD5: { /* AAD */
        uint8_t base = fetch8(s);
        uint8_t al = (uint8_t)(get_reg8(c, 4) * base + get_reg8(c, 0));
        set_reg8(c, 4, 0);
        set_reg8(c, 0, al);
        set_szp8(c, al);
        return 1;
    }
    case 0x60: { /* PUSHA */
        uint16_t old_sp = c->sp;
        push16(s, c->ax); push16(s, c->cx); push16(s, c->dx); push16(s, c->bx);
        push16(s, old_sp); push16(s, c->bp); push16(s, c->si); push16(s, c->di);
        return 1;
    }
    case 0x61: { /* POPA */
        c->di = pop16(s); c->si = pop16(s); c->bp = pop16(s);
        (void)pop16(s); /* discard SP */
        c->bx = pop16(s); c->dx = pop16(s); c->cx = pop16(s); c->ax = pop16(s);
        return 1;
    }
    case 0x68: push16(s, fetch16(s)); return 1; /* PUSH imm16 */
    case 0x6A: push16(s, (uint16_t)(int16_t)(int8_t)fetch8(s)); return 1; /* PUSH imm8 */
    case 0xC8: { /* ENTER imm16, imm8 */
        uint16_t size = fetch16(s);
        uint8_t nesting = (uint8_t)(fetch8(s) & 0x1F);
        uint16_t frame;
        uint8_t i;
        push16(s, c->bp);
        frame = c->sp;
        if (nesting > 0) {
            for (i = 1; i < nesting; i++) {
                c->bp = (uint16_t)(c->bp - 2);
                push16(s, dos_read16(s, dos_seg_off(c->ss, c->bp)));
            }
            push16(s, frame);
        }
        c->bp = frame;
        c->sp = (uint16_t)(c->sp - size);
        return 1;
    }
    case 0xC9: /* LEAVE */
        c->sp = c->bp;
        c->bp = pop16(s);
        return 1;
    case 0xCF: /* IRET */
        c->ip = pop16(s);
        c->cs = pop16(s);
        c->flags = pop16(s) | 0x0002;
        return 1;
    case 0x8F: { /* POP r/m16 */
        uint8_t modrm = fetch8(s);
        ea_t e = parse_ea(s, modrm);
        ea_write16(s, e, pop16(s));
        return 1;
    }
    case 0x82: /* alias of 0x80 on 8086 */
        /* fall through handled by duplicating 0x80 path — rewrite op */ {
            uint8_t modrm = fetch8(s);
            int alu = (modrm >> 3) & 7;
            ea_t e = parse_ea(s, modrm);
            uint8_t imm = fetch8(s);
            uint8_t v = ea_read8(s, e);
            alu8(c, alu, &v, imm);
            if (alu != 7) ea_write8(s, e, v);
            return 1;
        }
    case 0x9C: /* PUSHF — 386+ exposes IOPL/NT (bits 12–14) in real mode */
        push16(s, (uint16_t)(c->flags | 0x0002));
        return 1;
    case 0x9D: { /* POPF — allow IOPL/NT updates (DOS/16M 386 detect) */
        uint16_t nf = (uint16_t)(pop16(s) | 0x0002u);
        c->flags = nf;
        s->eflags = (s->eflags & 0xFFFF0000u) | nf;
        return 1;
    }
    case 0x9E: { /* SAHF */
        uint8_t ah = get_reg8(c, 4);
        c->flags = (uint16_t)((c->flags & 0xFF00) | (ah & 0xD5) | 0x02);
        return 1;
    }
    case 0x9F: set_reg8(c, 4, (uint8_t)(c->flags & 0xFF)); return 1; /* LAHF */
    case 0x98: /* CBW */
        if (get_reg8(c, 0) & 0x80) c->ax |= 0xFF00; else c->ax &= 0x00FF;
        return 1;
    case 0x99: /* CWD */
        c->dx = (c->ax & 0x8000) ? 0xFFFF : 0;
        return 1;
    case 0xC3: c->ip = pop16(s); return 1;
    case 0xC2: { uint16_t imm = fetch16(s); c->ip = pop16(s); c->sp = (uint16_t)(c->sp + imm); return 1; }
    case 0xCB: { /* RETF */
        uint16_t nip = pop16(s);
        uint16_t ncs = pop16(s);
        pm_far_jump(s, ncs, nip);
        return 1;
    }
    case 0xCA: {
        uint16_t imm = fetch16(s);
        uint16_t nip = pop16(s);
        uint16_t ncs = pop16(s);
        c->sp = (uint16_t)(c->sp + imm);
        pm_far_jump(s, ncs, nip);
        return 1;
    }
    case 0xE8: { int16_t rel = (int16_t)fetch16(s); push16(s, c->ip); c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xE9: { int16_t rel = (int16_t)fetch16(s); c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xEB: { int8_t rel = (int8_t)fetch8(s); c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xEA: { /* JMP far */
        uint16_t nip = fetch16(s);
        uint16_t ncs = fetch16(s);
        pm_far_jump(s, ncs, nip);
        return 1;
    }
    case 0x9A: { /* CALL far */
        uint16_t nip = fetch16(s);
        uint16_t ncs = fetch16(s);
        push16(s, c->cs); push16(s, c->ip);
        pm_far_jump(s, ncs, nip);
        return 1;
    }
    case 0xE2: { int8_t rel = (int8_t)fetch8(s); c->cx--; if (c->cx) c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xE0: { int8_t rel = (int8_t)fetch8(s); c->cx--; if (c->cx && !(c->flags & ZF)) c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xE1: { int8_t rel = (int8_t)fetch8(s); c->cx--; if (c->cx && (c->flags & ZF)) c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xE3: { int8_t rel = (int8_t)fetch8(s); if (c->cx == 0) c->ip = (uint16_t)(c->ip + rel); return 1; }
    case 0xFC: c->flags &= (uint16_t)~DF; return 1;
    case 0xFD: c->flags |= DF; return 1;
    case 0xFA: c->flags &= (uint16_t)~IF; return 1;
    case 0xFB: c->flags |= IF; return 1;
    case 0xF8: c->flags &= (uint16_t)~CF; return 1;
    case 0xF9: c->flags |= CF; return 1;
    case 0xF5: c->flags ^= CF; return 1;
    case 0x06: push16(s, c->es); return 1;
    case 0x07: load_sreg(s, 0, pop16(s)); return 1;
    case 0x0E: push16(s, c->cs); return 1;
    case 0x16: push16(s, c->ss); return 1;
    case 0x17: load_sreg(s, 2, pop16(s)); return 1;
    case 0x1E: push16(s, c->ds); return 1;
    case 0x1F: load_sreg(s, 3, pop16(s)); return 1;
    case 0xA0: { uint16_t off = fetch16(s); uint16_t seg = s->has_seg_override ? s->seg_override : c->ds;
        set_reg8(c, 0, dos_read8(s, linear_seg_off(s, seg, off))); return 1; }
    case 0xA1: { uint16_t off = fetch16(s); uint16_t seg = s->has_seg_override ? s->seg_override : c->ds;
        c->ax = dos_read16(s, linear_seg_off(s, seg, off)); return 1; }
    case 0xA2: { uint16_t off = fetch16(s); uint16_t seg = s->has_seg_override ? s->seg_override : c->ds;
        dos_write8(s, linear_seg_off(s, seg, off), get_reg8(c, 0)); return 1; }
    case 0xA3: { uint16_t off = fetch16(s); uint16_t seg = s->has_seg_override ? s->seg_override : c->ds;
        dos_write16(s, linear_seg_off(s, seg, off), c->ax); return 1; }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xA6: case 0xA7:
    case 0xAE: case 0xAF:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        do_string_op(s, op); return 1;
    case 0x8A: case 0x8B: case 0x88: case 0x89: {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        int w = op & 1;
        int d = (op == 0x8A || op == 0x8B);
        if (!w) {
            if (d) set_reg8(c, reg, ea_read8(s, e));
            else ea_write8(s, e, get_reg8(c, reg));
        } else {
            if (d) *reg16(c, reg) = ea_read16(s, e);
            else ea_write16(s, e, *reg16(c, reg));
        }
        return 1;
    }
    case 0x8C: { /* MOV r/m16, sreg */
        uint8_t modrm = fetch8(s);
        int sr = (modrm >> 3) & 7;
        uint16_t v = 0;
        ea_t e = parse_ea(s, modrm);
        if (sr == 0) v = c->es; else if (sr == 1) v = c->cs;
        else if (sr == 2) v = c->ss; else if (sr == 3) v = c->ds;
        else if (sr == 4) v = s->fs; else if (sr == 5) v = s->gs;
        ea_write16(s, e, v);
        return 1;
    }
    case 0x8E: { /* MOV sreg, r/m16 */
        uint8_t modrm = fetch8(s);
        int sr = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        uint16_t v = ea_read16(s, e);
        if (sr == 0 || sr == 2 || sr == 3 || sr == 4 || sr == 5)
            load_sreg(s, sr, v);
        /* MOV CS illegal — ignore */
        return 1;
    }
    case 0x8D: { /* LEA */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        /* compute offset only */
        {
            uint8_t mod = (uint8_t)((modrm >> 6) & 3);
            uint8_t rm = (uint8_t)(modrm & 7);
            int16_t disp = 0;
            uint16_t off = 0;
            if (mod == 3) { *reg16(c, reg) = *reg16(c, rm); return 1; }
            if (mod == 1) disp = (int8_t)fetch8(s);
            else if (mod == 2 || (mod == 0 && rm == 6)) disp = (int16_t)fetch16(s);
            switch (rm) {
            case 0: off = (uint16_t)(c->bx + c->si + disp); break;
            case 1: off = (uint16_t)(c->bx + c->di + disp); break;
            case 2: off = (uint16_t)(c->bp + c->si + disp); break;
            case 3: off = (uint16_t)(c->bp + c->di + disp); break;
            case 4: off = (uint16_t)(c->si + disp); break;
            case 5: off = (uint16_t)(c->di + disp); break;
            case 6: off = (mod == 0) ? (uint16_t)disp : (uint16_t)(c->bp + disp); break;
            case 7: off = (uint16_t)(c->bx + disp); break;
            }
            *reg16(c, reg) = off;
        }
        return 1;
    }
    case 0xC4: case 0xC5: { /* LES / LDS */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        uint16_t off = ea_read16(s, e);
        uint16_t seg;
        if (e.is_reg) return 1;
        seg = dos_read16(s, e.addr + 2);
        *reg16(c, reg) = off;
        if (op == 0xC4) load_sreg(s, 0, seg);
        else load_sreg(s, 3, seg);
        return 1;
    }
    case 0xC6: {
        uint8_t modrm = fetch8(s);
        ea_t e = parse_ea(s, modrm);
        uint8_t imm = fetch8(s);
        ea_write8(s, e, imm);
        return 1;
    }
    case 0xC7: {
        uint8_t modrm = fetch8(s);
        ea_t e = parse_ea(s, modrm);
        uint16_t imm = fetch16(s);
        ea_write16(s, e, imm);
        return 1;
    }
    case 0x80: case 0x81: case 0x83: {
        uint8_t modrm = fetch8(s);
        int alu = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (op == 0x80) {
            uint8_t imm = fetch8(s);
            uint8_t v = ea_read8(s, e);
            alu8(c, alu, &v, imm);
            if (alu != 7) ea_write8(s, e, v);
        } else {
            uint16_t imm = (op == 0x83) ? (uint16_t)(int16_t)(int8_t)fetch8(s) : fetch16(s);
            uint16_t v = ea_read16(s, e);
            alu16(c, alu, &v, imm);
            if (alu != 7) ea_write16(s, e, v);
        }
        return 1;
    }
    case 0x84: case 0x85: { /* TEST */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (op == 0x84) {
            uint8_t r = (uint8_t)(ea_read8(s, e) & get_reg8(c, reg));
            c->flags &= (uint16_t)~(CF|OF); set_szp8(c, r);
        } else {
            uint16_t r = (uint16_t)(ea_read16(s, e) & *reg16(c, reg));
            c->flags &= (uint16_t)~(CF|OF); set_szp16(c, r);
        }
        return 1;
    }
    case 0xA8: { uint8_t imm = fetch8(s); uint8_t r = (uint8_t)(get_reg8(c, 0) & imm);
        c->flags &= (uint16_t)~(CF|OF); set_szp8(c, r); return 1; }
    case 0xA9: { uint16_t imm = fetch16(s); uint16_t r = (uint16_t)(c->ax & imm);
        c->flags &= (uint16_t)~(CF|OF); set_szp16(c, r); return 1; }
    case 0x86: case 0x87: { /* XCHG */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (op == 0x86) {
            uint8_t t = ea_read8(s, e);
            ea_write8(s, e, get_reg8(c, reg));
            set_reg8(c, reg, t);
        } else {
            uint16_t t = ea_read16(s, e);
            ea_write16(s, e, *reg16(c, reg));
            *reg16(c, reg) = t;
        }
        return 1;
    }
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        uint16_t t = c->ax; c->ax = *reg16(c, op - 0x90); *reg16(c, op - 0x90) = t;
        return 1;
    }
    case 0xD7: { /* XLAT */
        uint16_t seg = s->has_seg_override ? s->seg_override : c->ds;
        set_reg8(c, 0, dos_read8(s, dos_seg_off(seg, (uint16_t)(c->bx + get_reg8(c, 0)))));
        return 1;
    }
    case 0xF6: case 0xF7: {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (op == 0xF6) {
            uint8_t v = ea_read8(s, e);
            if (grp == 0) { /* TEST imm */
                uint8_t imm = fetch8(s);
                uint8_t r = (uint8_t)(v & imm);
                c->flags &= (uint16_t)~(CF|OF); set_szp8(c, r);
            } else if (grp == 2) { ea_write8(s, e, (uint8_t)~v); }
            else if (grp == 3) { uint8_t r = (uint8_t)(-v); ea_write8(s, e, r); set_szp8(c, r);
                if (r) c->flags |= CF; else c->flags &= (uint16_t)~CF; }
            else if (grp == 4) { /* MUL */ uint16_t r = (uint16_t)get_reg8(c, 0) * v; c->ax = r;
                if (c->ax >> 8) c->flags |= CF|OF; else c->flags &= (uint16_t)~(CF|OF); }
            else if (grp == 5) { /* IMUL */ int16_t r = (int8_t)get_reg8(c, 0) * (int8_t)v; c->ax = (uint16_t)r;
                if ((int16_t)c->ax != (int8_t)c->ax) c->flags |= CF|OF; else c->flags &= (uint16_t)~(CF|OF); }
            else if (grp == 6 && v) { /* DIV */ uint16_t n = c->ax; set_reg8(c, 0, (uint8_t)(n / v)); set_reg8(c, 4, (uint8_t)(n % v)); }
            else if (grp == 7 && v) { /* IDIV */ int16_t n = (int16_t)c->ax; set_reg8(c, 0, (uint8_t)(n / (int8_t)v)); set_reg8(c, 4, (uint8_t)(n % (int8_t)v)); }
        } else {
            uint16_t v = ea_read16(s, e);
            if (grp == 0) {
                uint16_t imm = fetch16(s);
                uint16_t r = (uint16_t)(v & imm);
                c->flags &= (uint16_t)~(CF|OF); set_szp16(c, r);
            } else if (grp == 2) ea_write16(s, e, (uint16_t)~v);
            else if (grp == 3) { uint16_t r = (uint16_t)(-v); ea_write16(s, e, r); set_szp16(c, r);
                if (r) c->flags |= CF; else c->flags &= (uint16_t)~CF; }
            else if (grp == 4) { uint32_t r = (uint32_t)c->ax * v; c->ax = (uint16_t)r; c->dx = (uint16_t)(r >> 16);
                if (c->dx) c->flags |= CF|OF; else c->flags &= (uint16_t)~(CF|OF); }
            else if (grp == 5) { /* IMUL r/m16 */
                int32_t r = (int32_t)(int16_t)c->ax * (int32_t)(int16_t)v;
                c->ax = (uint16_t)r;
                c->dx = (uint16_t)((uint32_t)r >> 16);
                if (r != (int32_t)(int16_t)c->ax) c->flags |= CF|OF;
                else c->flags &= (uint16_t)~(CF|OF);
            }
            else if (grp == 6 && v) { uint32_t n = ((uint32_t)c->dx << 16) | c->ax; c->ax = (uint16_t)(n / v); c->dx = (uint16_t)(n % v); }
            else if (grp == 7 && v) { /* IDIV r/m16 */
                int32_t n = (int32_t)(((uint32_t)c->dx << 16) | c->ax);
                int16_t d = (int16_t)v;
                c->ax = (uint16_t)(n / d);
                c->dx = (uint16_t)(n % d);
            }
        }
        return 1;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        int w = op & 1;
        int count;
        if (op == 0xC0 || op == 0xC1) count = fetch8(s) & 0x1F;
        else if (op >= 0xD2) count = get_reg8(c, 1);
        else count = 1;
        if (count == 0) return 1;
        shift_rm(s, w, grp, e, count);
        return 1;
    }
    case 0xFE: {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        uint8_t a = ea_read8(s, e), res;
        if (grp == 0) {
            res = (uint8_t)(a + 1); ea_write8(s, e, res); set_szp8(c, res); set_of_add8(c, a, 1, res);
            if ((a & 0x0F) == 0x0F) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        } else if (grp == 1) {
            res = (uint8_t)(a - 1); ea_write8(s, e, res); set_szp8(c, res); set_of_sub8(c, a, 1, res);
            if ((a & 0x0F) == 0) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        }
        return 1;
    }
    case 0xFF: {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        ea_t e = parse_ea(s, modrm);
        if (grp == 0) {
            uint16_t a = ea_read16(s, e), res = (uint16_t)(a + 1);
            ea_write16(s, e, res); set_szp16(c, res); set_of_add16(c, a, 1, res);
            if ((a & 0x0F) == 0x0F) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        } else if (grp == 1) {
            uint16_t a = ea_read16(s, e), res = (uint16_t)(a - 1);
            ea_write16(s, e, res); set_szp16(c, res); set_of_sub16(c, a, 1, res);
            if ((a & 0x0F) == 0) c->flags |= AF; else c->flags &= (uint16_t)~AF;
        } else if (grp == 2) { uint16_t t = ea_read16(s, e); push16(s, c->ip); c->ip = t; }
        else if (grp == 3) { /* CALL far m16:16 */
            uint16_t nip = dos_read16(s, e.addr);
            uint16_t ncs = dos_read16(s, e.addr + 2);
            push16(s, c->cs); push16(s, c->ip); c->cs = ncs; c->ip = nip;
        } else if (grp == 4) c->ip = ea_read16(s, e);
        else if (grp == 5) { c->ip = dos_read16(s, e.addr); c->cs = dos_read16(s, e.addr + 2); }
        else if (grp == 6) push16(s, ea_read16(s, e));
        return 1;
    }
    case 0xE4: { /* IN AL,imm8 */
        uint8_t port = fetch8(s);
        set_reg8(c, 0, dos_in_port(s, port));
        return 1;
    }
    case 0xE5: { /* IN AX,imm8 */
        uint8_t port = fetch8(s);
        c->ax = dos_in_port(s, port);
        return 1;
    }
    case 0xEC: /* IN AL,DX */
        set_reg8(c, 0, dos_in_port(s, c->dx));
        return 1;
    case 0xED: /* IN AX,DX */
        c->ax = dos_in_port(s, c->dx);
        return 1;
    case 0xE6: { /* OUT imm8, AL */
        uint8_t port = fetch8(s);
        dos_out_port(s, port, get_reg8(c, 0));
        return 1;
    }
    case 0xE7: { /* OUT imm8, AX */
        uint8_t port = fetch8(s);
        dos_out_port(s, port, get_reg8(c, 0));
        return 1;
    }
    case 0xEE: /* OUT DX, AL */
        dos_out_port(s, c->dx, get_reg8(c, 0));
        return 1;
    case 0xEF: /* OUT DX, AX */
        dos_out_port(s, c->dx, get_reg8(c, 0));
        return 1;
    case 0x0F:
        return cpu_exec_0f(s);
    default: {
        char msg[48];
        int i = 0;
        const char* hex = "0123456789ABCDEF";
        msg[i++] = '\r'; msg[i++] = '\n';
        msg[i++] = 'u'; msg[i++] = 'n'; msg[i++] = 's'; msg[i++] = 'u';
        msg[i++] = 'p'; msg[i++] = 'p'; msg[i++] = 'o'; msg[i++] = 'r';
        msg[i++] = 't'; msg[i++] = 'e'; msg[i++] = 'd'; msg[i++] = ' ';
        msg[i++] = 'o'; msg[i++] = 'p'; msg[i++] = 'c'; msg[i++] = 'o';
        msg[i++] = 'd'; msg[i++] = 'e'; msg[i++] = ' ';
        msg[i++] = hex[(op >> 4) & 0xF];
        msg[i++] = hex[op & 0xF];
        msg[i++] = 'h';
        msg[i++] = '\r'; msg[i++] = '\n'; msg[i] = '\0';
        dos_video_puts(s, "\r\nGooberDOS: ");
        dos_video_puts(s, msg);
        s->halted = 1;
        s->shell_reentry = 1;
        return 0;
    }
    }
}
