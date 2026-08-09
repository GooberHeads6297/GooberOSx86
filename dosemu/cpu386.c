#include "dosemu_priv.h"

#define CF 0x0001u
#define PF 0x0004u
#define AF 0x0010u
#define ZF 0x0040u
#define SF 0x0080u
#define IF 0x0200u
#define DF 0x0400u
#define OF 0x0800u

void cpu386_sync_from_16(dos_session_t* s) {
    cpu8086_t* c;
    if (!s) return;
    c = &s->cpu;
    s->eax = (s->eax & 0xFFFF0000u) | c->ax;
    s->ebx = (s->ebx & 0xFFFF0000u) | c->bx;
    s->ecx = (s->ecx & 0xFFFF0000u) | c->cx;
    s->edx = (s->edx & 0xFFFF0000u) | c->dx;
    s->esi = (s->esi & 0xFFFF0000u) | c->si;
    s->edi = (s->edi & 0xFFFF0000u) | c->di;
    s->ebp = (s->ebp & 0xFFFF0000u) | c->bp;
    s->esp32 = (s->esp32 & 0xFFFF0000u) | c->sp;
    s->eip = (s->eip & 0xFFFF0000u) | c->ip;
    s->eflags = (s->eflags & 0xFFFF0000u) | c->flags;
}

void cpu386_sync_to_16(dos_session_t* s) {
    cpu8086_t* c;
    if (!s) return;
    c = &s->cpu;
    c->ax = (uint16_t)s->eax;
    c->bx = (uint16_t)s->ebx;
    c->cx = (uint16_t)s->ecx;
    c->dx = (uint16_t)s->edx;
    c->si = (uint16_t)s->esi;
    c->di = (uint16_t)s->edi;
    c->bp = (uint16_t)s->ebp;
    c->sp = (uint16_t)s->esp32;
    c->ip = (uint16_t)s->eip;
    c->flags = (uint16_t)s->eflags;
}

static uint8_t fetch8_lin(dos_session_t* s) {
    uint32_t lin = pm_ea(s, &s->seg_cs, s->eip);
    uint8_t v = dos_read8(s, lin);
    s->eip++;
    s->cpu.ip = (uint16_t)s->eip;
    return v;
}

static uint16_t fetch16_lin(dos_session_t* s) {
    uint16_t v = (uint16_t)fetch8_lin(s);
    v |= (uint16_t)fetch8_lin(s) << 8;
    return v;
}

static uint32_t fetch32_lin(dos_session_t* s) {
    uint32_t v = fetch16_lin(s);
    v |= (uint32_t)fetch16_lin(s) << 16;
    return v;
}

static uint8_t fetch8_rm(dos_session_t* s) {
    uint8_t v = dos_read8(s, dos_seg_off(s->cpu.cs, s->cpu.ip));
    s->cpu.ip++;
    s->eip = (s->eip & 0xFFFF0000u) | s->cpu.ip;
    return v;
}

static uint16_t fetch16_rm(dos_session_t* s) {
    uint16_t lo = fetch8_rm(s);
    return (uint16_t)(lo | ((uint16_t)fetch8_rm(s) << 8));
}

static uint32_t fetch32_rm(dos_session_t* s) {
    uint32_t v = fetch16_rm(s);
    return v | ((uint32_t)fetch16_rm(s) << 16);
}

static uint8_t fetch8(dos_session_t* s) {
    return (s->pe && s->cpu32) ? fetch8_lin(s) : fetch8_rm(s);
}

static uint16_t fetch16(dos_session_t* s) {
    return (s->pe && s->cpu32) ? fetch16_lin(s) : fetch16_rm(s);
}

static uint32_t fetch32(dos_session_t* s) {
    return (s->pe && s->cpu32) ? fetch32_lin(s) : fetch32_rm(s);
}

static uint32_t* reg32(dos_session_t* s, int r) {
    switch (r & 7) {
    case 0: return &s->eax;
    case 1: return &s->ecx;
    case 2: return &s->edx;
    case 3: return &s->ebx;
    case 4: return &s->esp32;
    case 5: return &s->ebp;
    case 6: return &s->esi;
    default: return &s->edi;
    }
}

static void set_szp32(dos_session_t* s, uint32_t v) {
    uint32_t f = s->eflags;
    f &= ~(ZF | SF | PF);
    if (v == 0) f |= ZF;
    if (v & 0x80000000u) f |= SF;
    /* coarse PF */
    {
        uint8_t b = (uint8_t)(v & 0xFF);
        int bits = 0, i;
        for (i = 0; i < 8; i++) if (b & (1u << i)) bits++;
        if ((bits & 1) == 0) f |= PF;
    }
    s->eflags = f;
    s->cpu.flags = (uint16_t)f;
}

static void set_szp16(dos_session_t* s, uint16_t v) {
    uint16_t f = s->cpu.flags;
    f &= (uint16_t)~(ZF | SF | PF);
    if (v == 0) f |= ZF;
    if (v & 0x8000u) f |= SF;
    {
        uint8_t b = (uint8_t)(v & 0xFF);
        int bits = 0, i;
        for (i = 0; i < 8; i++) if (b & (1u << i)) bits++;
        if ((bits & 1) == 0) f |= PF;
    }
    s->cpu.flags = f;
    s->eflags = (s->eflags & 0xFFFF0000u) | f;
}

static pm_seg_t* seg_override(dos_session_t* s) {
    if (!s->has_seg_override) return &s->seg_ds;
    if (s->seg_override == s->cpu.es) return &s->seg_es;
    if (s->seg_override == s->cpu.cs) return &s->seg_cs;
    if (s->seg_override == s->cpu.ss) return &s->seg_ss;
    if (s->seg_override == s->fs) return &s->seg_fs;
    if (s->seg_override == s->gs) return &s->seg_gs;
    if (s->seg_override == s->cpu.ds) return &s->seg_ds;
    /* override stored as raw selector value in RM */
    {
        static pm_seg_t tmp;
        if (pm_load_selector(s, s->seg_override, &tmp) == 0) return &tmp;
    }
    return &s->seg_ds;
}

static void load_data_sel(dos_session_t* s, uint16_t sel, pm_seg_t* cache, uint16_t* vis) {
    pm_seg_t tmp;
    if (pm_load_selector(s, sel, &tmp) != 0) {
        pm_load_selector(s, 0, &tmp);
    }
    *cache = tmp;
    if (vis) *vis = sel;
}

static uint32_t ea32(dos_session_t* s, uint8_t modrm, pm_seg_t** seg_out) {
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    uint8_t rm = (uint8_t)(modrm & 7);
    uint32_t addr = 0;
    pm_seg_t* seg = seg_override(s);

    if (mod == 3) {
        if (seg_out) *seg_out = NULL;
        return 0;
    }

    if (rm == 4) {
        /* SIB */
        uint8_t sib = fetch8(s);
        uint8_t base = (uint8_t)(sib & 7);
        uint8_t index = (uint8_t)((sib >> 3) & 7);
        uint8_t scale = (uint8_t)((sib >> 6) & 3);
        uint32_t b = 0, idx = 0;
        if (!(mod == 0 && base == 5)) b = *reg32(s, base);
        else {
            b = fetch32(s);
            seg = &s->seg_ds;
        }
        if (index != 4) idx = *reg32(s, index) << scale;
        addr = b + idx;
        if (base == 5 || base == 4) {
            /* ss for EBP-based when applicable */
            if (base == 5 && mod != 0) seg = &s->seg_ss;
            if (base == 4) { /* ESP */ seg = s->has_seg_override ? seg : &s->seg_ss; }
        }
        if (mod == 1) addr += (int32_t)(int8_t)fetch8(s);
        else if (mod == 2) addr += fetch32(s);
    } else if (mod == 0 && rm == 5) {
        addr = fetch32(s);
        seg = &s->seg_ds;
    } else {
        addr = *reg32(s, rm);
        if (rm == 5) seg = s->has_seg_override ? seg : &s->seg_ss;
        if (mod == 1) addr += (int32_t)(int8_t)fetch8(s);
        else if (mod == 2) addr += fetch32(s);
    }
    if (seg_out) *seg_out = seg;
    return pm_ea(s, seg, addr);
}

static uint32_t ea16_to_lin(dos_session_t* s, uint8_t modrm, pm_seg_t** seg_out) {
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    uint8_t rm = (uint8_t)(modrm & 7);
    uint16_t off = 0;
    uint16_t segv;
    cpu8086_t* c = &s->cpu;

    if (mod == 3) {
        if (seg_out) *seg_out = NULL;
        return 0;
    }
    switch (rm) {
    case 0: off = (uint16_t)(c->bx + c->si); break;
    case 1: off = (uint16_t)(c->bx + c->di); break;
    case 2: off = (uint16_t)(c->bp + c->si); break;
    case 3: off = (uint16_t)(c->bp + c->di); break;
    case 4: off = c->si; break;
    case 5: off = c->di; break;
    case 6: off = (mod == 0) ? 0 : c->bp; break;
    case 7: off = c->bx; break;
    }
    if (mod == 0 && rm == 6) off = fetch16(s);
    else if (mod == 1) off = (uint16_t)(off + (int16_t)(int8_t)fetch8(s));
    else if (mod == 2) off = (uint16_t)(off + fetch16(s));

    if (s->has_seg_override) segv = s->seg_override;
    else if (rm == 2 || rm == 3 || (rm == 6 && mod != 0)) segv = c->ss;
    else segv = c->ds;

    if (s->pe) {
        static pm_seg_t tmp;
        pm_load_selector(s, segv, &tmp);
        if (seg_out) *seg_out = &tmp;
        return pm_ea(s, &tmp, off);
    }
    if (seg_out) *seg_out = NULL;
    return dos_seg_off(segv, off);
}

static uint32_t modrm_addr(dos_session_t* s, uint8_t modrm, int* is_reg, int* regn) {
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    if (mod == 3) {
        *is_reg = 1;
        *regn = modrm & 7;
        return 0;
    }
    *is_reg = 0;
    *regn = 0;
    if (s->addr32 || (s->pe && s->cpu32))
        return ea32(s, modrm, NULL);
    return ea16_to_lin(s, modrm, NULL);
}

static uint32_t read_rm32(dos_session_t* s, uint8_t modrm) {
    int is_reg, regn;
    uint32_t a = modrm_addr(s, modrm, &is_reg, &regn);
    if (is_reg) return *reg32(s, regn);
    return dos_read32(s, a);
}

static void write_rm32(dos_session_t* s, uint8_t modrm, uint32_t v) {
    int is_reg, regn;
    uint32_t a = modrm_addr(s, modrm, &is_reg, &regn);
    if (is_reg) *reg32(s, regn) = v;
    else dos_write32(s, a, v);
}

/* Decode once — use for read-modify-write so displacement is not re-fetched. */
static uint32_t load_rm32(dos_session_t* s, uint8_t modrm, int* is_reg, int* regn, uint32_t* addr) {
    *addr = modrm_addr(s, modrm, is_reg, regn);
    if (*is_reg) return *reg32(s, *regn);
    return dos_read32(s, *addr);
}

static void store_rm32(dos_session_t* s, int is_reg, int regn, uint32_t addr, uint32_t v) {
    if (is_reg) *reg32(s, regn) = v;
    else dos_write32(s, addr, v);
}

static uint16_t read_rm16(dos_session_t* s, uint8_t modrm) {
    int is_reg, regn;
    uint32_t a;
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    if (mod == 3) return (uint16_t)(*reg32(s, modrm & 7) & 0xFFFF);
    a = modrm_addr(s, modrm, &is_reg, &regn);
    return dos_read16(s, a);
}

static void write_rm16(dos_session_t* s, uint8_t modrm, uint16_t v) {
    int is_reg, regn;
    uint32_t a;
    uint8_t mod = (uint8_t)((modrm >> 6) & 3);
    if (mod == 3) {
        uint32_t* r = reg32(s, modrm & 7);
        *r = (*r & 0xFFFF0000u) | v;
        return;
    }
    a = modrm_addr(s, modrm, &is_reg, &regn);
    dos_write16(s, a, v);
}

static void push32(dos_session_t* s, uint32_t v) {
    if (s->pe && s->seg_ss.use32) {
        s->esp32 -= 4;
        dos_write32(s, pm_ea(s, &s->seg_ss, s->esp32), v);
        s->cpu.sp = (uint16_t)s->esp32;
    } else {
        s->cpu.sp = (uint16_t)(s->cpu.sp - 4);
        s->esp32 = (s->esp32 & 0xFFFF0000u) | s->cpu.sp;
        dos_write32(s, dos_seg_off(s->cpu.ss, s->cpu.sp), v);
    }
}

static uint32_t pop32(dos_session_t* s) {
    uint32_t v;
    if (s->pe && s->seg_ss.use32) {
        v = dos_read32(s, pm_ea(s, &s->seg_ss, s->esp32));
        s->esp32 += 4;
        s->cpu.sp = (uint16_t)s->esp32;
    } else {
        v = dos_read32(s, dos_seg_off(s->cpu.ss, s->cpu.sp));
        s->cpu.sp = (uint16_t)(s->cpu.sp + 4);
        s->esp32 = (s->esp32 & 0xFFFF0000u) | s->cpu.sp;
    }
    return v;
}

static void push16(dos_session_t* s, uint16_t v) {
    s->cpu.sp = (uint16_t)(s->cpu.sp - 2);
    s->esp32 = (s->esp32 & 0xFFFF0000u) | s->cpu.sp;
    if (s->pe)
        dos_write16(s, pm_ea(s, &s->seg_ss, s->esp32), v);
    else
        dos_write16(s, dos_seg_off(s->cpu.ss, s->cpu.sp), v);
}

static uint16_t pop16(dos_session_t* s) {
    uint16_t v;
    if (s->pe)
        v = dos_read16(s, pm_ea(s, &s->seg_ss, s->esp32));
    else
        v = dos_read16(s, dos_seg_off(s->cpu.ss, s->cpu.sp));
    s->cpu.sp = (uint16_t)(s->cpu.sp + 2);
    s->esp32 = (s->esp32 & 0xFFFF0000u) | s->cpu.sp;
    return v;
}

static void alu32(dos_session_t* s, int op, uint32_t* dst, uint32_t src) {
    uint64_t a = *dst, b = src, r = 0;
    uint32_t f = s->eflags;
    f &= ~(CF | OF | AF);
    switch (op) {
    case 0:
        r = a + b;
        if (r > 0xFFFFFFFFu) f |= CF;
        if ((~(a ^ b) & (a ^ r)) & 0x80000000u) f |= OF;
        break;
    case 1: r = a | b; break;
    case 2:
        r = a + b + ((s->eflags & CF) ? 1 : 0);
        if (r > 0xFFFFFFFFu) f |= CF;
        if ((~(a ^ b) & (a ^ r)) & 0x80000000u) f |= OF;
        break;
    case 3:
        r = a - b - ((s->eflags & CF) ? 1 : 0);
        if (a < b + ((s->eflags & CF) ? 1 : 0)) f |= CF;
        if (((a ^ b) & (a ^ r)) & 0x80000000u) f |= OF;
        break;
    case 4: r = a & b; break;
    case 5:
        r = a - b;
        if (a < b) f |= CF;
        if (((a ^ b) & (a ^ r)) & 0x80000000u) f |= OF;
        break;
    case 6: r = a ^ b; break;
    case 7:
        r = a - b;
        if (a < b) f |= CF;
        if (((a ^ b) & (a ^ r)) & 0x80000000u) f |= OF;
        s->eflags = f;
        set_szp32(s, (uint32_t)r);
        return;
    }
    *dst = (uint32_t)r;
    s->eflags = f;
    set_szp32(s, (uint32_t)r);
}

static int jcc32(dos_session_t* s, uint8_t cc) {
    uint32_t f = s->eflags;
    int cf = (f & CF) != 0, zf = (f & ZF) != 0, sf = (f & SF) != 0, of = (f & OF) != 0;
    switch (cc & 0xF) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return (f & PF) != 0;
    case 0xB: return (f & PF) == 0;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    case 0xF: return !zf && (sf == of);
    }
    return 0;
}

static void pm_fault(dos_session_t* s, const char* msg) {
    dos_video_puts(s, "\r\nGooberDOS: ");
    dos_video_puts(s, msg);
    dos_video_puts(s, "\r\n");
    s->halted = 1;
    s->shell_reentry = 1;
}

/* ---- 0F escape ---- */
int cpu_exec_0f(dos_session_t* s) {
    uint8_t op2 = fetch8(s);
    cpu386_sync_from_16(s);

    /* long conditional jumps */
    if (op2 >= 0x80 && op2 <= 0x8F) {
        if (s->op32 || (s->pe && s->cpu32)) {
            int32_t rel = (int32_t)fetch32(s);
            if (jcc32(s, (uint8_t)(op2 - 0x80))) {
                s->eip = (uint32_t)((int32_t)s->eip + rel);
                s->cpu.ip = (uint16_t)s->eip;
            }
        } else {
            int16_t rel = (int16_t)fetch16(s);
            if (jcc32(s, (uint8_t)(op2 - 0x80))) {
                s->cpu.ip = (uint16_t)(s->cpu.ip + rel);
                s->eip = (s->eip & 0xFFFF0000u) | s->cpu.ip;
            }
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    switch (op2) {
    case 0x01: { /* LGDT/LIDT/LMSW/SMSW */
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr;
        if (grp == 4 || grp == 6) { /* SMSW / LMSW */
            if (((modrm >> 6) & 3) == 3) {
                if (grp == 4) {
                    *reg32(s, modrm & 7) = (*reg32(s, modrm & 7) & 0xFFFF0000u) |
                                           (uint16_t)(s->cr0 & 0xFFFF);
                } else {
                    uint16_t v = (uint16_t)(*reg32(s, modrm & 7) & 0xFFFF);
                    pm_set_cr0(s, (s->cr0 & 0xFFFF0000u) | v);
                }
            } else {
                addr = modrm_addr(s, modrm, &is_reg, &regn);
                if (grp == 4) dos_write16(s, addr, (uint16_t)(s->cr0 & 0xFFFF));
                else pm_set_cr0(s, (s->cr0 & 0xFFFF0000u) | dos_read16(s, addr));
            }
            cpu386_sync_to_16(s);
            return 1;
        }
        if (grp == 2 || grp == 3) { /* LGDT / LIDT */
            uint16_t lim;
            uint32_t base;
            addr = modrm_addr(s, modrm, &is_reg, &regn);
            lim = dos_read16(s, addr);
            base = dos_read32(s, addr + 2);
            if (!s->op32 && !(s->pe && s->cpu32))
                base &= 0x00FFFFFFu; /* 24-bit in 16-bit op size */
            if (grp == 2) { s->gdtr_limit = lim; s->gdtr_base = base; }
            else { s->idtr_limit = lim; s->idtr_base = base; }
            cpu386_sync_to_16(s);
            return 1;
        }
        if (grp == 0 || grp == 1) { /* SGDT / SIDT */
            addr = modrm_addr(s, modrm, &is_reg, &regn);
            if (grp == 0) {
                dos_write16(s, addr, s->gdtr_limit);
                dos_write32(s, addr + 2, s->gdtr_base);
            } else {
                dos_write16(s, addr, s->idtr_limit);
                dos_write32(s, addr + 2, s->idtr_base);
            }
            cpu386_sync_to_16(s);
            return 1;
        }
        /* INVLPG etc — nop */
        if (((modrm >> 6) & 3) != 3) (void)modrm_addr(s, modrm, &is_reg, &regn);
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0x00: { /* SLDT/STR/LLDT/LTR/VERR/VERW — mostly nop/store 0 */
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        int is_reg, regn;
        if (grp == 0 || grp == 1) { /* SLDT / STR */
            if (((modrm >> 6) & 3) == 3)
                *reg32(s, modrm & 7) = (*reg32(s, modrm & 7) & 0xFFFF0000u);
            else {
                uint32_t a = modrm_addr(s, modrm, &is_reg, &regn);
                dos_write16(s, a, 0);
            }
        } else if (((modrm >> 6) & 3) != 3) {
            (void)modrm_addr(s, modrm, &is_reg, &regn);
        }
        /* LLDT/LTR: accept and ignore */
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0x06: /* CLTS */
        s->cr0 &= ~(uint32_t)0x8;
        cpu386_sync_to_16(s);
        return 1;
    case 0x20: { /* MOV r32, CRx */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int rm = modrm & 7;
        uint32_t val = 0;
        if (reg == 0) val = s->cr0;
        else if (reg == 2) val = 0; /* CR2 */
        else if (reg == 3) val = 0; /* CR3 */
        else val = 0;
        *reg32(s, rm) = val;
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0x22: { /* MOV CRx, r32 */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int rm = modrm & 7;
        uint32_t val = *reg32(s, rm);
        if (reg == 0) pm_set_cr0(s, val);
        /* CR2/CR3 ignored */
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0xA0: /* PUSH FS */
        if (s->op32 || (s->pe && s->cpu32)) push32(s, s->fs);
        else push16(s, s->fs);
        cpu386_sync_to_16(s);
        return 1;
    case 0xA1: /* POP FS */
        if (s->op32 || (s->pe && s->cpu32)) {
            uint32_t v = pop32(s);
            s->fs = (uint16_t)v;
        } else s->fs = pop16(s);
        load_data_sel(s, s->fs, &s->seg_fs, NULL);
        cpu386_sync_to_16(s);
        return 1;
    case 0xA8: /* PUSH GS */
        if (s->op32 || (s->pe && s->cpu32)) push32(s, s->gs);
        else push16(s, s->gs);
        cpu386_sync_to_16(s);
        return 1;
    case 0xA9: /* POP GS */
        if (s->op32 || (s->pe && s->cpu32)) {
            uint32_t v = pop32(s);
            s->gs = (uint16_t)v;
        } else s->gs = pop16(s);
        load_data_sel(s, s->gs, &s->seg_gs, NULL);
        cpu386_sync_to_16(s);
        return 1;
    case 0xA3: { /* BT r/m, r */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        uint32_t bit = *reg32(s, reg);
        uint32_t val = read_rm32(s, modrm);
        if (val & (1u << (bit & 31))) s->eflags |= CF;
        else s->eflags &= ~CF;
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0xBA: { /* BT/BTS/BTR/BTC r/m, imm8 */
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr = 0;
        uint32_t val;
        uint8_t imm;
        uint32_t mask;
        if (((modrm >> 6) & 3) == 3) {
            is_reg = 1;
            regn = modrm & 7;
            val = *reg32(s, regn);
        } else {
            is_reg = 0;
            addr = modrm_addr(s, modrm, &is_reg, &regn);
            val = dos_read32(s, addr);
        }
        imm = fetch8(s);
        mask = 1u << (imm & 31);
        if (val & mask) s->eflags |= CF; else s->eflags &= ~CF;
        if (grp == 5) val |= mask;
        else if (grp == 6) val &= ~mask;
        else if (grp == 7) val ^= mask;
        if (grp != 4) {
            if (((modrm >> 6) & 3) == 3) *reg32(s, modrm & 7) = val;
            else dos_write32(s, addr, val);
        }
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0xB6: /* MOVZX r32, r/m8 */
    case 0xB7: { /* MOVZX r32, r/m16 */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t a;
        uint32_t val;
        if (((modrm >> 6) & 3) == 3) {
            if (op2 == 0xB6) val = (uint8_t)(*reg32(s, modrm & 7) & 0xFF);
            else val = (uint16_t)(*reg32(s, modrm & 7) & 0xFFFF);
        } else {
            a = modrm_addr(s, modrm, &is_reg, &regn);
            val = (op2 == 0xB6) ? dos_read8(s, a) : dos_read16(s, a);
        }
        *reg32(s, reg) = val;
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0xA2: { /* CPUID */
        uint32_t leaf = s->eax;
        if (leaf == 0) {
            s->eax = 1;
            s->ebx = 0x756E6547u; /* "Genu" */
            s->edx = 0x49656E69u; /* "ineI" */
            s->ecx = 0x6C65746Eu; /* "ntel" */
        } else {
            /* Family 4 (486), model 3 — DOS/16M accepts 386/486 */
            s->eax = 0x00000430u;
            s->ebx = 0;
            s->ecx = 0;
            s->edx = 0x00000001u; /* FPU */
        }
        cpu386_sync_to_16(s);
        return 1;
    }
    case 0xBE: /* MOVSX r32, r/m8 */
    case 0xBF: { /* MOVSX r32, r/m16 */
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t a;
        int32_t val;
        if (((modrm >> 6) & 3) == 3) {
            if (op2 == 0xBE) val = (int8_t)(*reg32(s, modrm & 7) & 0xFF);
            else val = (int16_t)(*reg32(s, modrm & 7) & 0xFFFF);
        } else {
            a = modrm_addr(s, modrm, &is_reg, &regn);
            if (op2 == 0xBE) val = (int8_t)dos_read8(s, a);
            else val = (int16_t)dos_read16(s, a);
        }
        *reg32(s, reg) = (uint32_t)val;
        cpu386_sync_to_16(s);
        return 1;
    }
    default: {
        char hx[4];
        hx[0] = "0123456789ABCDEF"[(op2 >> 4) & 0xF];
        hx[1] = "0123456789ABCDEF"[op2 & 0xF];
        hx[2] = '\0';
        dos_video_puts(s, "\r\nGooberDOS: unhandled 0F ");
        dos_video_puts(s, hx);
        dos_video_puts(s, "h (PM/386 path)\r\n");
        s->halted = 1;
        s->shell_reentry = 1;
        return 0;
    }
    }
}

/* Execute one insn that needs 32-bit operand size or full PM32. */
static int cpu386_exec_op(dos_session_t* s, uint8_t op) {
    /* Default operand size: 32 in use32 CS, else 16. Prefix 66 toggles. */
    int op32;
    if (s->pe && s->cpu32 && s->seg_cs.use32)
        op32 = s->op32 ? 0 : 1;
    else
        op32 = s->op32 ? 1 : 0;

    cpu386_sync_from_16(s);
    /* Keep descriptor caches aligned with live RM segment registers */
    if (!s->pe) {
        s->seg_cs.base = (uint32_t)s->cpu.cs << 4;
        s->seg_ds.base = (uint32_t)s->cpu.ds << 4;
        s->seg_es.base = (uint32_t)s->cpu.es << 4;
        s->seg_ss.base = (uint32_t)s->cpu.ss << 4;
        s->seg_fs.base = (uint32_t)s->fs << 4;
        s->seg_gs.base = (uint32_t)s->gs << 4;
        s->seg_cs.sel = s->cpu.cs;
        s->seg_ds.sel = s->cpu.ds;
        s->seg_es.sel = s->cpu.es;
        s->seg_ss.sel = s->cpu.ss;
    }

    if (op == 0x0F) return cpu_exec_0f(s);

    /* MOV r32, imm32 */
    if (op >= 0xB8 && op <= 0xBF && op32) {
        *reg32(s, op - 0xB8) = fetch32(s);
        cpu386_sync_to_16(s);
        return 1;
    }
    /* PUSH/POP r32 */
    if (op >= 0x50 && op <= 0x57 && op32) {
        push32(s, *reg32(s, op - 0x50));
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op >= 0x58 && op <= 0x5F && op32) {
        *reg32(s, op - 0x58) = pop32(s);
        cpu386_sync_to_16(s);
        return 1;
    }
    /* INC/DEC r32 */
    if (op >= 0x40 && op <= 0x47 && op32) {
        uint32_t* r = reg32(s, op - 0x40);
        uint32_t a = *r, res = a + 1;
        *r = res; set_szp32(s, res);
        if ((~(a ^ 1) & (a ^ res)) & 0x80000000u) s->eflags |= OF; else s->eflags &= ~OF;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op >= 0x48 && op <= 0x4F && op32) {
        uint32_t* r = reg32(s, op - 0x48);
        uint32_t a = *r, res = a - 1;
        *r = res; set_szp32(s, res);
        if (((a ^ 1) & (a ^ res)) & 0x80000000u) s->eflags |= OF; else s->eflags &= ~OF;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* ALU EAX, imm32 */
    if (op32 && (op == 0x05 || op == 0x0D || op == 0x15 || op == 0x1D ||
                 op == 0x25 || op == 0x2D || op == 0x35 || op == 0x3D)) {
        int alu = (op >> 3) & 7;
        uint32_t imm = fetch32(s);
        alu32(s, alu, &s->eax, imm);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* ALU r/m32, r32 / r32, r/m32 */
    if (op32 && op < 0x40 && (op & 1) && ((op & 0x06) != 0x06)) {
        int d = (op >> 1) & 1;
        int alu = (op >> 3) & 7;
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr, rm, rv;
        rm = load_rm32(s, modrm, &is_reg, &regn, &addr);
        rv = *reg32(s, reg);
        if (d) { alu32(s, alu, &rv, rm); *reg32(s, reg) = rv; }
        else { alu32(s, alu, &rm, rv); if (alu != 7) store_rm32(s, is_reg, regn, addr, rm); }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* Grp1 r/m32, imm */
    if (op32 && (op == 0x81 || op == 0x83)) {
        uint8_t modrm = fetch8(s);
        int alu = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr, rm, imm;
        rm = load_rm32(s, modrm, &is_reg, &regn, &addr);
        imm = (op == 0x83) ? (uint32_t)(int32_t)(int8_t)fetch8(s) : fetch32(s);
        alu32(s, alu, &rm, imm);
        if (alu != 7) store_rm32(s, is_reg, regn, addr, rm);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* Shift/rotate r/m32 (DOS/16M CPUID probe uses 66 C1 /5 imm8) */
    if (op32 && (op == 0xC0 || op == 0xC1 || op == 0xD0 || op == 0xD1 ||
                 op == 0xD2 || op == 0xD3)) {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        int is_reg, regn, count, i;
        uint32_t addr, v;
        v = load_rm32(s, modrm, &is_reg, &regn, &addr);
        if (op == 0xC0 || op == 0xC1) count = fetch8(s) & 31;
        else if (op == 0xD0 || op == 0xD1) count = 1;
        else count = (int)(s->ecx & 31);
        for (i = 0; i < count; i++) {
            uint32_t cf;
            if (grp == 0) { /* ROL */
                cf = (v >> 31) & 1u;
                v = (v << 1) | cf;
                if (cf) s->eflags |= CF; else s->eflags &= ~CF;
            } else if (grp == 1) { /* ROR */
                cf = v & 1u;
                v = (v >> 1) | (cf << 31);
                if (cf) s->eflags |= CF; else s->eflags &= ~CF;
            } else if (grp == 2) { /* RCL */
                cf = (v >> 31) & 1u;
                v = (v << 1) | ((s->eflags & CF) ? 1u : 0u);
                if (cf) s->eflags |= CF; else s->eflags &= ~CF;
            } else if (grp == 3) { /* RCR */
                cf = v & 1u;
                v = (v >> 1) | ((s->eflags & CF) ? 0x80000000u : 0u);
                if (cf) s->eflags |= CF; else s->eflags &= ~CF;
            } else if (grp == 4 || grp == 6) { /* SHL/SAL */
                if (v & 0x80000000u) s->eflags |= CF; else s->eflags &= ~CF;
                v <<= 1;
            } else if (grp == 5) { /* SHR */
                if (v & 1u) s->eflags |= CF; else s->eflags &= ~CF;
                v >>= 1;
            } else if (grp == 7) { /* SAR */
                if (v & 1u) s->eflags |= CF; else s->eflags &= ~CF;
                v = (uint32_t)((int32_t)v >> 1);
            }
        }
        if (count) {
            store_rm32(s, is_reg, regn, addr, v);
            set_szp32(s, v);
        }
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* MOV r/m32, imm32 */
    if (op32 && op == 0xC7) {
        uint8_t modrm = fetch8(s);
        int is_reg, regn;
        uint32_t addr, imm;
        (void)load_rm32(s, modrm, &is_reg, &regn, &addr); /* consume EA */
        imm = fetch32(s);
        if (((modrm >> 3) & 7) == 0)
            store_rm32(s, is_reg, regn, addr, imm);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* TEST r/m32, r32 / TEST EAX, imm32 */
    if (op32 && op == 0x85) {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        uint32_t rm = read_rm32(s, modrm);
        uint32_t r = *reg32(s, reg);
        uint32_t res = rm & r;
        set_szp32(s, res);
        s->eflags &= ~(CF | OF);
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0xA9) {
        uint32_t imm = fetch32(s);
        uint32_t res = s->eax & imm;
        set_szp32(s, res);
        s->eflags &= ~(CF | OF);
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* Grp3 F7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m32 */
    if (op32 && op == 0xF7) {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr, rm;
        rm = load_rm32(s, modrm, &is_reg, &regn, &addr);
        if (grp == 0 || grp == 1) { /* TEST r/m32, imm32 */
            uint32_t imm = fetch32(s);
            uint32_t res = rm & imm;
            set_szp32(s, res);
            s->eflags &= ~(CF | OF);
        } else if (grp == 2) { /* NOT */
            store_rm32(s, is_reg, regn, addr, ~rm);
        } else if (grp == 3) { /* NEG */
            uint32_t res = (uint32_t)(0u - rm);
            store_rm32(s, is_reg, regn, addr, res);
            set_szp32(s, res);
            if (rm == 0) s->eflags &= ~CF; else s->eflags |= CF;
            if (res == 0x80000000u) s->eflags |= OF; else s->eflags &= ~OF;
        } else if (grp == 4) { /* MUL r/m32 */
            uint64_t p = (uint64_t)s->eax * (uint64_t)rm;
            s->eax = (uint32_t)p;
            s->edx = (uint32_t)(p >> 32);
            if (s->edx) s->eflags |= (CF | OF); else s->eflags &= ~(CF | OF);
        } else if (grp == 5) { /* IMUL r/m32 */
            int64_t p = (int64_t)(int32_t)s->eax * (int64_t)(int32_t)rm;
            s->eax = (uint32_t)p;
            s->edx = (uint32_t)((uint64_t)p >> 32);
            if (s->edx != (uint32_t)((int32_t)s->eax >> 31))
                s->eflags |= (CF | OF);
            else
                s->eflags &= ~(CF | OF);
        } else if (grp == 6) { /* DIV r/m32 */
            uint64_t n = ((uint64_t)s->edx << 32) | s->eax;
            if (rm == 0) {
                pm_fault(s, "DIV by zero");
                return 0;
            }
            s->eax = (uint32_t)(n / rm);
            s->edx = (uint32_t)(n % rm);
        } else if (grp == 7) { /* IDIV r/m32 */
            int64_t n = (int64_t)(((uint64_t)s->edx << 32) | s->eax);
            if (rm == 0) {
                pm_fault(s, "IDIV by zero");
                return 0;
            }
            s->eax = (uint32_t)(n / (int32_t)rm);
            s->edx = (uint32_t)(n % (int32_t)rm);
        }
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* POP r/m32 */
    if (op32 && op == 0x8F) {
        uint8_t modrm = fetch8(s);
        write_rm32(s, modrm, pop32(s));
        cpu386_sync_to_16(s);
        return 1;
    }

    /* XCHG r/m32, r32 */
    if (op32 && op == 0x87) {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t addr, rm, rv;
        rm = load_rm32(s, modrm, &is_reg, &regn, &addr);
        rv = *reg32(s, reg);
        store_rm32(s, is_reg, regn, addr, rv);
        *reg32(s, reg) = rm;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* MOV r/m32, r32 / r32, r/m32 */
    if (op32 && (op == 0x89 || op == 0x8B)) {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        if (op == 0x89) write_rm32(s, modrm, *reg32(s, reg));
        else *reg32(s, reg) = read_rm32(s, modrm);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* MOV r/m16, Sreg / Sreg, r/m16 — keep 16-bit even with 66 for FS/GS */
    if (op == 0x8C || op == 0x8E) {
        uint8_t modrm = fetch8(s);
        int sr = (modrm >> 3) & 7;
        uint16_t val;
        if (op == 0x8C) {
            if (sr == 0) val = s->cpu.es;
            else if (sr == 1) val = s->cpu.cs;
            else if (sr == 2) val = s->cpu.ss;
            else if (sr == 3) val = s->cpu.ds;
            else if (sr == 4) val = s->fs;
            else if (sr == 5) val = s->gs;
            else val = 0;
            write_rm16(s, modrm, val);
        } else {
            val = read_rm16(s, modrm);
            if (sr == 0) { s->cpu.es = val; load_data_sel(s, val, &s->seg_es, NULL); }
            else if (sr == 2) { s->cpu.ss = val; load_data_sel(s, val, &s->seg_ss, NULL); }
            else if (sr == 3) { s->cpu.ds = val; load_data_sel(s, val, &s->seg_ds, NULL); }
            else if (sr == 4) { s->fs = val; load_data_sel(s, val, &s->seg_fs, NULL); }
            else if (sr == 5) { s->gs = val; load_data_sel(s, val, &s->seg_gs, NULL); }
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* MOV EAX, moffs32 / moffs32, EAX */
    if (op32 && (op == 0xA1 || op == 0xA3)) {
        uint32_t off = (s->addr32 || (s->pe && s->cpu32)) ? fetch32(s) : fetch16(s);
        uint32_t lin;
        if (!s->pe) {
            uint16_t segv = s->has_seg_override ? s->seg_override : s->cpu.ds;
            lin = dos_seg_off(segv, (uint16_t)off);
        } else {
            pm_seg_t* seg = seg_override(s);
            lin = pm_ea(s, seg, off);
        }
        if (op == 0xA1) s->eax = dos_read32(s, lin);
        else dos_write32(s, lin, s->eax);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* XCHG EAX, r32 */
    if (op32 && op >= 0x90 && op <= 0x97) {
        if (op != 0x90) {
            uint32_t* r = reg32(s, op - 0x90);
            uint32_t t = s->eax;
            s->eax = *r;
            *r = t;
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* PUSH/POP imm */
    if (op32 && op == 0x68) { push32(s, fetch32(s)); cpu386_sync_to_16(s); return 1; }
    if (op32 && op == 0x6A) {
        push32(s, (uint32_t)(int32_t)(int8_t)fetch8(s));
        cpu386_sync_to_16(s);
        return 1;
    }

    /* near Jcc rel8 still 16-bit path; Jcc rel32 via 0F */

    /* JMP/CALL/RET near 32 */
    if (op32 && op == 0xE9) {
        int32_t rel = (int32_t)fetch32(s);
        s->eip = (uint32_t)((int32_t)s->eip + rel);
        s->cpu.ip = (uint16_t)s->eip;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0xE8) {
        int32_t rel = (int32_t)fetch32(s);
        push32(s, s->eip);
        s->eip = (uint32_t)((int32_t)s->eip + rel);
        s->cpu.ip = (uint16_t)s->eip;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0xC3) {
        s->eip = pop32(s);
        s->cpu.ip = (uint16_t)s->eip;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0xC2) {
        uint16_t imm = fetch16(s);
        s->eip = pop32(s);
        s->esp32 += imm;
        s->cpu.sp = (uint16_t)s->esp32;
        s->cpu.ip = (uint16_t)s->eip;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* LEA */
    if (op == 0x8D) {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        int is_reg, regn;
        uint32_t a;
        /* LEA: address without segment base for flat; with for PM use offset only */
        if (s->addr32 || (s->pe && s->cpu32)) {
            /* re-parse as offset without adding base — recreate */
            uint32_t eip_save = s->eip;
            uint16_t ip_save = s->cpu.ip;
            pm_seg_t* seg = NULL;
            uint32_t off;
            /* rewind fetch of modrm already done; use ea helpers carefully */
            (void)eip_save; (void)ip_save; (void)seg;
            a = modrm_addr(s, modrm, &is_reg, &regn);
            /* subtract segment base to get offset */
            if (s->pe && s->seg_ds.base) {
                /* approximate: use flat offset from ea32 path — store linear for Doom flat model */
                off = a; /* flat: base 0 */
            } else {
                off = a;
            }
            if (op32) *reg32(s, reg) = off;
            else {
                uint32_t* r = reg32(s, reg);
                *r = (*r & 0xFFFF0000u) | (uint16_t)off;
            }
        } else {
            a = modrm_addr(s, modrm, &is_reg, &regn);
            if (op32) *reg32(s, reg) = a;
            else {
                uint32_t* r = reg32(s, reg);
                *r = (*r & 0xFFFF0000u) | (uint16_t)a;
            }
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* far JMP/CALL */
    if (op == 0xEA) {
        uint32_t new_ip = op32 ? fetch32(s) : fetch16(s);
        uint16_t new_cs = fetch16(s);
        pm_far_jump(s, new_cs, new_ip);
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op == 0x9A) {
        uint32_t new_ip = op32 ? fetch32(s) : fetch16(s);
        uint16_t new_cs = fetch16(s);
        if (op32) {
            push32(s, s->cpu.cs);
            push32(s, s->eip);
        } else {
            push16(s, s->cpu.cs);
            push16(s, s->cpu.ip);
        }
        pm_far_jump(s, new_cs, new_ip);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* PUSHF/POPF 32 — 386: IOPL/NT writable; AC (18) not sticky; ID (21) sticky for CPUID probe */
    if (op32 && op == 0x9C) {
        push32(s, (s->eflags | 2u) & ~0x10000u);
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0x9D) {
        uint32_t nf = pop32(s) | 2u;
        /* Drop RF; keep ID so CPUID path can run; clear AC (486-only) */
        nf &= ~0x10000u;
        nf &= ~0x40000u;
        s->eflags = nf;
        s->cpu.flags = (uint16_t)nf;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* PUSHA/POPA 32 */
    if (op32 && op == 0x60) {
        uint32_t esp = s->esp32;
        push32(s, s->eax); push32(s, s->ecx); push32(s, s->edx); push32(s, s->ebx);
        push32(s, esp); push32(s, s->ebp); push32(s, s->esi); push32(s, s->edi);
        cpu386_sync_to_16(s);
        return 1;
    }
    if (op32 && op == 0x61) {
        s->edi = pop32(s); s->esi = pop32(s); s->ebp = pop32(s);
        (void)pop32(s); /* skip esp */
        s->ebx = pop32(s); s->edx = pop32(s); s->ecx = pop32(s); s->eax = pop32(s);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* MOVZX-style already in 0F; string ops with 67/66 */
    if ((op == 0xA5 || op == 0xA4 || op == 0xAB || op == 0xAA) &&
        (s->addr32 || op32 || s->rep_prefix)) {
        /* simplified MOVSD/MOVSW/STOSD */
        int w32 = op32 || (op == 0xA5 && (s->pe && s->cpu32));
        uint32_t delta = (op == 0xA4 || op == 0xAA) ? 1u : (w32 ? 4u : 2u);
        int df = (s->cpu.flags & DF) ? -1 : 1;
        for (;;) {
            if (s->rep_prefix && s->cpu.cx == 0 && !(s->pe && s->cpu32 && s->ecx == 0)) break;
            if (s->rep_prefix && s->pe && s->cpu32 && s->ecx == 0) break;
            if (op == 0xA4) {
                uint8_t v = dos_read8(s, pm_ea(s, &s->seg_ds, s->addr32 ? s->esi : s->cpu.si));
                /* FS override for MOVS when 64h prefix — seg_override */
                if (s->has_seg_override)
                    v = dos_read8(s, pm_ea(s, seg_override(s), s->addr32 ? s->esi : s->cpu.si));
                dos_write8(s, pm_ea(s, &s->seg_es, s->addr32 ? s->edi : s->cpu.di), v);
            } else if (op == 0xA5) {
                uint32_t src_off = s->addr32 ? s->esi : s->cpu.si;
                uint32_t dst_off = s->addr32 ? s->edi : s->cpu.di;
                pm_seg_t* srcseg = s->has_seg_override ? seg_override(s) : &s->seg_ds;
                if (w32) {
                    uint32_t v = dos_read32(s, pm_ea(s, srcseg, src_off));
                    dos_write32(s, pm_ea(s, &s->seg_es, dst_off), v);
                } else {
                    uint16_t v = dos_read16(s, pm_ea(s, srcseg, src_off));
                    dos_write16(s, pm_ea(s, &s->seg_es, dst_off), v);
                }
            } else if (op == 0xAA) {
                dos_write8(s, pm_ea(s, &s->seg_es, s->addr32 ? s->edi : s->cpu.di),
                           (uint8_t)(s->eax & 0xFF));
            } else if (op == 0xAB) {
                uint32_t dst = s->addr32 ? s->edi : s->cpu.di;
                if (w32) dos_write32(s, pm_ea(s, &s->seg_es, dst), s->eax);
                else dos_write16(s, pm_ea(s, &s->seg_es, dst), (uint16_t)s->eax);
            }
            if (s->addr32 || (s->pe && s->cpu32)) {
                if (op == 0xA4 || op == 0xA5) s->esi = (uint32_t)((int32_t)s->esi + df * (int)delta);
                s->edi = (uint32_t)((int32_t)s->edi + df * (int)delta);
                s->cpu.si = (uint16_t)s->esi;
                s->cpu.di = (uint16_t)s->edi;
            } else {
                if (op == 0xA4 || op == 0xA5)
                    s->cpu.si = (uint16_t)(s->cpu.si + df * (int)delta);
                s->cpu.di = (uint16_t)(s->cpu.di + df * (int)delta);
            }
            if (!s->rep_prefix) break;
            if (s->pe && s->cpu32) {
                s->ecx--;
                s->cpu.cx = (uint16_t)s->ecx;
            } else {
                s->cpu.cx--;
            }
            if (s->cpu.cx == 0 && !(s->pe && s->cpu32 && s->ecx)) break;
            if (s->pe && s->cpu32 && s->ecx == 0) break;
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* INS/OUTS (6C–6F), including 66-prefixed dword forms */
    if (op >= 0x6C && op <= 0x6F) {
        int is_out = (op == 0x6E || op == 0x6F);
        int is_byte = (op == 0x6C || op == 0x6E);
        int w32 = !is_byte && op32;
        uint32_t delta = is_byte ? 1u : (w32 ? 4u : 2u);
        int df = (s->cpu.flags & DF) ? -1 : 1;
        uint16_t port = s->cpu.dx;
        pm_seg_t* srcseg = s->has_seg_override ? seg_override(s) : &s->seg_ds;
        for (;;) {
            if (s->rep_prefix && s->cpu.cx == 0 && !(s->pe && s->cpu32 && s->ecx == 0))
                break;
            if (s->rep_prefix && s->pe && s->cpu32 && s->ecx == 0)
                break;
            if (is_out) {
                uint32_t off = s->addr32 ? s->esi : s->cpu.si;
                uint32_t a = pm_ea(s, srcseg, off);
                if (is_byte) {
                    dos_out_port(s, port, dos_read8(s, a));
                } else if (w32) {
                    uint32_t v = dos_read32(s, a);
                    dos_out_port(s, port, (uint8_t)(v & 0xFF));
                    dos_out_port(s, port, (uint8_t)((v >> 8) & 0xFF));
                    dos_out_port(s, port, (uint8_t)((v >> 16) & 0xFF));
                    dos_out_port(s, port, (uint8_t)((v >> 24) & 0xFF));
                } else {
                    uint16_t v = dos_read16(s, a);
                    dos_out_port(s, port, (uint8_t)(v & 0xFF));
                    dos_out_port(s, port, (uint8_t)((v >> 8) & 0xFF));
                }
            } else {
                uint32_t off = s->addr32 ? s->edi : s->cpu.di;
                uint32_t a = pm_ea(s, &s->seg_es, off);
                if (is_byte) {
                    dos_write8(s, a, dos_in_port(s, port));
                } else if (w32) {
                    uint32_t v = (uint32_t)dos_in_port(s, port);
                    v |= (uint32_t)dos_in_port(s, port) << 8;
                    v |= (uint32_t)dos_in_port(s, port) << 16;
                    v |= (uint32_t)dos_in_port(s, port) << 24;
                    dos_write32(s, a, v);
                } else {
                    uint16_t v = (uint16_t)dos_in_port(s, port);
                    v |= (uint16_t)dos_in_port(s, port) << 8;
                    dos_write16(s, a, v);
                }
            }
            if (s->addr32 || (s->pe && s->cpu32)) {
                if (is_out)
                    s->esi = (uint32_t)((int32_t)s->esi + df * (int)delta);
                else
                    s->edi = (uint32_t)((int32_t)s->edi + df * (int)delta);
                s->cpu.si = (uint16_t)s->esi;
                s->cpu.di = (uint16_t)s->edi;
            } else {
                if (is_out)
                    s->cpu.si = (uint16_t)(s->cpu.si + df * (int)delta);
                else
                    s->cpu.di = (uint16_t)(s->cpu.di + df * (int)delta);
            }
            if (!s->rep_prefix) break;
            if (s->pe && s->cpu32) {
                s->ecx--;
                s->cpu.cx = (uint16_t)s->ecx;
            } else {
                s->cpu.cx--;
            }
            if (s->cpu.cx == 0 && !(s->pe && s->cpu32 && s->ecx)) break;
            if (s->pe && s->cpu32 && s->ecx == 0) break;
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* INT — still via 16-bit handler when in RM/PM16 */
    if (op == 0xCD) {
        uint8_t vec = fetch8(s);
        cpu386_sync_to_16(s);
        if (vec == 0x31 && s->dpmi_installed) return dpmi_int31(s) == 0 ? 1 : 0;
        if (vec == 0x2F) return dos_bios_int2f(s) == 0 ? 1 : 0;
        if (dos_ivt_is_default(s, vec)) {
            if (dos_handle_int(s, vec) != 0) return 0;
        } else {
            dos_soft_int(s, vec);
        }
        cpu386_sync_from_16(s);
        return 1;
    }

    /* IRET */
    if (op == 0xCF) {
        if (op32 || (s->pe && s->cpu32)) {
            s->eip = pop32(s);
            {
                uint32_t ncs = pop32(s);
                pm_far_jump(s, (uint16_t)ncs, s->eip);
            }
            s->eflags = pop32(s) | 2u;
            s->cpu.flags = (uint16_t)s->eflags;
        } else {
            uint16_t nip = pop16(s);
            uint16_t ncs = pop16(s);
            s->cpu.flags = pop16(s);
            pm_far_jump(s, ncs, nip);
        }
        cpu386_sync_to_16(s);
        return 1;
    }

    /* Grp FF */
    if (op == 0xFF) {
        uint8_t modrm = fetch8(s);
        int grp = (modrm >> 3) & 7;
        if (grp == 4 || grp == 5) { /* JMP near/far */
            if (grp == 4) {
                uint32_t t = op32 ? read_rm32(s, modrm) : read_rm16(s, modrm);
                s->eip = t;
                s->cpu.ip = (uint16_t)t;
            } else {
                int is_reg, regn;
                uint32_t a = modrm_addr(s, modrm, &is_reg, &regn);
                uint32_t nip = op32 ? dos_read32(s, a) : dos_read16(s, a);
                uint16_t ncs = dos_read16(s, a + (op32 ? 4u : 2u));
                pm_far_jump(s, ncs, nip);
            }
            cpu386_sync_to_16(s);
            return 1;
        }
        if (grp == 2 || grp == 3) { /* CALL near/far */
            if (grp == 2) {
                uint32_t t = op32 ? read_rm32(s, modrm) : read_rm16(s, modrm);
                if (op32) push32(s, s->eip); else push16(s, s->cpu.ip);
                s->eip = t;
                s->cpu.ip = (uint16_t)t;
            } else {
                int is_reg, regn;
                uint32_t a = modrm_addr(s, modrm, &is_reg, &regn);
                uint32_t nip = op32 ? dos_read32(s, a) : dos_read16(s, a);
                uint16_t ncs = dos_read16(s, a + (op32 ? 4u : 2u));
                if (op32) { push32(s, s->cpu.cs); push32(s, s->eip); }
                else { push16(s, s->cpu.cs); push16(s, s->cpu.ip); }
                pm_far_jump(s, ncs, nip);
            }
            cpu386_sync_to_16(s);
            return 1;
        }
        if (grp == 6) { /* PUSH r/m */
            if (op32) push32(s, read_rm32(s, modrm));
            else push16(s, read_rm16(s, modrm));
            cpu386_sync_to_16(s);
            return 1;
        }
        if (grp == 0 || grp == 1) { /* INC/DEC */
            if (op32) {
                int is_reg, regn;
                uint32_t addr, v;
                v = load_rm32(s, modrm, &is_reg, &regn, &addr);
                v = (grp == 0) ? v + 1 : v - 1;
                store_rm32(s, is_reg, regn, addr, v);
                set_szp32(s, v);
            } else {
                uint16_t v = read_rm16(s, modrm);
                v = (uint16_t)((grp == 0) ? v + 1 : v - 1);
                write_rm16(s, modrm, v);
                set_szp16(s, v);
            }
            cpu386_sync_to_16(s);
            return 1;
        }
    }

    /* IMUL r32, r/m32, imm */
    if (op32 && (op == 0x69 || op == 0x6B)) {
        uint8_t modrm = fetch8(s);
        int reg = (modrm >> 3) & 7;
        uint32_t rm = read_rm32(s, modrm);
        int32_t imm = (op == 0x6B) ? (int32_t)(int8_t)fetch8(s) : (int32_t)fetch32(s);
        int64_t p = (int64_t)(int32_t)rm * (int64_t)imm;
        *reg32(s, reg) = (uint32_t)p;
        if ((int32_t)(uint32_t)p != p) s->eflags |= (CF | OF);
        else s->eflags &= ~(CF | OF);
        s->cpu.flags = (uint16_t)s->eflags;
        cpu386_sync_to_16(s);
        return 1;
    }

    /* fall back: not handled as 32-bit special */
    (void)pm_fault;
    return -1;
}

int cpu386_step(dos_session_t* s) {
    uint8_t op;
    int prefixes = 0;
    int r;
    if (!s || s->halted || !s->mem) return 0;

    s->has_seg_override = 0;
    s->rep_prefix = 0;
    s->op32 = 0;
    s->addr32 = 0;

    /* In use32 CS, default op/addr size is 32; prefixes toggle */
    for (;;) {
        op = fetch8(s);
        if (op == 0x26) { s->seg_override = s->cpu.es; s->has_seg_override = 1; }
        else if (op == 0x2E) { s->seg_override = s->cpu.cs; s->has_seg_override = 1; }
        else if (op == 0x36) { s->seg_override = s->cpu.ss; s->has_seg_override = 1; }
        else if (op == 0x3E) { s->seg_override = s->cpu.ds; s->has_seg_override = 1; }
        else if (op == 0x64) { s->seg_override = s->fs; s->has_seg_override = 1; }
        else if (op == 0x65) { s->seg_override = s->gs; s->has_seg_override = 1; }
        else if (op == 0x66) { s->op32 = 1; }
        else if (op == 0x67) { s->addr32 = 1; }
        else if (op == 0x9B) { /* WAIT/FWAIT */ }
        else if (op == 0xF0) { /* LOCK — ignored */ }
        else if (op == 0xF3) { s->rep_prefix = 1; }
        else if (op == 0xF2) { s->rep_prefix = 2; }
        else break;
        if (++prefixes > 6) break;
    }

    if ((op & 0xF8) == 0xD8) {
        r = cpu_exec_fpu(s, op);
        if (r >= 0) return r;
    }

    r = cpu386_exec_op(s, op);
    if (r >= 0) return r;

    /* Unhandled in 32-bit path — try a few more common 16-bit-like ops in PM */
    if (op == 0x90) return 1;
    if (op == 0xF4) { s->halted = 1; return 0; }
    if (op == 0xFB) { s->cpu.flags |= IF; s->eflags |= IF; return 1; }
    if (op == 0xFA) { s->cpu.flags &= (uint16_t)~IF; s->eflags &= ~IF; return 1; }
    if (op == 0xFC) { s->cpu.flags &= (uint16_t)~DF; s->eflags &= ~DF; return 1; }
    if (op == 0xFD) { s->cpu.flags |= DF; s->eflags |= DF; return 1; }
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)fetch8(s);
        if (jcc32(s, (uint8_t)(op - 0x70))) {
            s->eip = (uint32_t)((int32_t)s->eip + rel);
            s->cpu.ip = (uint16_t)s->eip;
        }
        return 1;
    }
    if (op == 0xEB) {
        int8_t rel = (int8_t)fetch8(s);
        s->eip = (uint32_t)((int32_t)s->eip + rel);
        s->cpu.ip = (uint16_t)s->eip;
        return 1;
    }

    {
        char hx[8];
        hx[0] = "0123456789ABCDEF"[(op >> 4) & 0xF];
        hx[1] = "0123456789ABCDEF"[op & 0xF];
        hx[2] = '\0';
        dos_video_puts(s, "\r\nGooberDOS: PM32 opcode ");
        dos_video_puts(s, hx);
        dos_video_puts(s, "h not implemented yet\r\n");
    }
    s->halted = 1;
    s->shell_reentry = 1;
    return 0;
}

/* Called from cpu8086 when 66/67/64/65 seen or for shared 0F */
int cpu386_rm_after_prefix(dos_session_t* s, uint8_t op) {
    return cpu386_exec_op(s, op);
}

/*
 * Soft x87: consume D8–DF escapes. Enough for DOS/4GW / Watcom probes
 * (FNINIT, FLDCW/FNSTCW, FNSTSW) without a full numeric stack.
 */
int cpu_exec_fpu(dos_session_t* s, uint8_t esc_op) {
    uint8_t modrm;
    int mod, reg, rm;
    uint32_t addr = 0;

    if (!s || (esc_op & 0xF8) != 0xD8) return -1;
    cpu386_sync_from_16(s);
    modrm = (s->pe && s->cpu32) ? fetch8_lin(s) : fetch8_rm(s);
    mod = (modrm >> 6) & 3;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;

    if (mod != 3) {
        /* Discard EA (and its displacement) so IP stays correct */
        if (s->addr32 || (s->pe && s->cpu32))
            addr = ea32(s, modrm, NULL);
        else
            addr = ea16_to_lin(s, modrm, NULL);
    }

    /* DB E3 FNINIT / DB E2 FNCLEX */
    if (esc_op == 0xDB && mod == 3 && reg == 4 && rm == 3) {
        s->fpu_cw = 0x037F;
        s->fpu_sw = 0;
        s->fpu_tw = 0xFFFF;
        s->fpu_top = 0;
        cpu386_sync_to_16(s);
        return 1;
    }
    if (esc_op == 0xDB && mod == 3 && reg == 4 && rm == 2) {
        s->fpu_sw &= (uint16_t)~0x00FFu; /* clear exception flags */
        cpu386_sync_to_16(s);
        return 1;
    }
    /* DF E0 FNSTSW AX */
    if (esc_op == 0xDF && mod == 3 && reg == 4 && rm == 0) {
        s->cpu.ax = s->fpu_sw;
        s->eax = (s->eax & 0xFFFF0000u) | s->cpu.ax;
        cpu386_sync_to_16(s);
        return 1;
    }
    /* D9 /5 FLDCW m16, D9 /7 FNSTCW m16 */
    if (esc_op == 0xD9 && mod != 3 && reg == 5) {
        s->fpu_cw = dos_read16(s, addr);
        cpu386_sync_to_16(s);
        return 1;
    }
    if (esc_op == 0xD9 && mod != 3 && reg == 7) {
        dos_write16(s, addr, s->fpu_cw);
        cpu386_sync_to_16(s);
        return 1;
    }
    /* DD /7 FNSTSW m16 */
    if (esc_op == 0xDD && mod != 3 && reg == 7) {
        dos_write16(s, addr, s->fpu_sw);
        cpu386_sync_to_16(s);
        return 1;
    }

    /* Other x87 ops: accept and ignore (keep stack tags empty) */
    (void)rm;
    cpu386_sync_to_16(s);
    return 1;
}
