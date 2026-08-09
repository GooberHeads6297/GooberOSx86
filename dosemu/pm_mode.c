#include "dosemu_priv.h"

static void pm_real_seg(pm_seg_t* out, uint16_t sel) {
    out->sel = sel;
    out->base = (uint32_t)sel << 4;
    out->limit = 0xFFFF;
    out->dpl = 0;
    out->present = 1;
    out->code = 0;
    out->use32 = 0;
    out->readable = 1;
    out->writable = 1;
}

void pm_sync_real_segs(dos_session_t* s) {
    if (!s) return;
    pm_real_seg(&s->seg_cs, s->cpu.cs);
    pm_real_seg(&s->seg_ds, s->cpu.ds);
    pm_real_seg(&s->seg_es, s->cpu.es);
    pm_real_seg(&s->seg_ss, s->cpu.ss);
    pm_real_seg(&s->seg_fs, s->fs);
    pm_real_seg(&s->seg_gs, s->gs);
    s->eip = s->cpu.ip;
    s->esp32 = s->cpu.sp;
    s->eflags = s->cpu.flags | 2u;
    cpu386_sync_from_16(s);
}

void pm_init_session(dos_session_t* s) {
    if (!s) return;
    s->cr0 = 0x00000010u; /* ET */
    s->pe = 0;
    s->cpu32 = 0;
    s->op32 = 0;
    s->addr32 = 0;
    s->pm_enabled = 1;
    s->eflags = 0x00000002u;
    s->eip = 0;
    s->esp32 = 0;
    s->eax = s->ebx = s->ecx = s->edx = 0;
    s->esi = s->edi = s->ebp = 0;
    s->fs = s->gs = 0;
    s->gdtr_base = 0;
    s->gdtr_limit = 0xFFFF;
    s->idtr_base = 0;
    s->idtr_limit = 0xFFFF;
    s->himem_brk = 0x100000u;
    s->fpu_cw = 0x037F;
    s->fpu_sw = 0;
    s->fpu_tw = 0xFFFF;
    s->fpu_top = 0;
    pm_real_seg(&s->seg_cs, 0);
    pm_real_seg(&s->seg_ds, 0);
    pm_real_seg(&s->seg_es, 0);
    pm_real_seg(&s->seg_ss, 0);
    pm_real_seg(&s->seg_fs, 0);
    pm_real_seg(&s->seg_gs, 0);
}

static int parse_gdt_entry(dos_session_t* s, uint16_t sel, pm_seg_t* out) {
    uint32_t addr;
    uint32_t limit, base;
    uint8_t access, flags;
    uint16_t idx = (uint16_t)(sel >> 3);

    if ((sel & 0x4) != 0) {
        /* LDT — not implemented; treat as null */
        return -1;
    }
    if (sel < 8) {
        /* null selector */
        out->sel = sel;
        out->base = 0;
        out->limit = 0;
        out->present = 0;
        out->use32 = 0;
        out->code = 0;
        out->readable = 0;
        out->writable = 0;
        return 0;
    }
    addr = s->gdtr_base + (uint32_t)idx * 8u;
    if (addr + 7u >= DOS_MEM_SIZE) return -1;

    limit = dos_read16(s, addr);
    base = dos_read16(s, addr + 2);
    base |= (uint32_t)dos_read8(s, addr + 4) << 16;
    access = dos_read8(s, addr + 5);
    flags = dos_read8(s, addr + 6);
    limit |= (uint32_t)(flags & 0x0F) << 16;
    base |= (uint32_t)dos_read8(s, addr + 7) << 24;
    if (flags & 0x80) {
        /* granularity: limit in 4K pages */
        limit = (limit << 12) | 0xFFFu;
    }

    out->sel = sel;
    out->base = base;
    out->limit = limit;
    out->dpl = (uint8_t)((access >> 5) & 3);
    out->present = (access & 0x80) ? 1 : 0;
    out->code = (access & 0x08) ? 1 : 0;
    out->use32 = (flags & 0x40) ? 1 : 0;
    out->readable = out->code ? ((access & 0x02) ? 1 : 0) : 1;
    out->writable = out->code ? 0 : ((access & 0x02) ? 1 : 0);
    return out->present ? 0 : -1;
}

int pm_load_selector(dos_session_t* s, uint16_t sel, pm_seg_t* out) {
    if (!s || !out) return -1;
    if (!s->pe) {
        pm_real_seg(out, sel);
        return 0;
    }
    return parse_gdt_entry(s, sel, out);
}

void pm_set_cr0(dos_session_t* s, uint32_t v) {
    if (!s) return;
    /* Soft guest: ignore PG; identity-map all linear addresses. */
    s->cr0 = (v & 0x0000001Fu) | 0x10u; /* PE/MP/EM/TS + ET */
    s->pe = (s->cr0 & 1u) ? 1 : 0;
    if (!s->pe) {
        s->cpu32 = 0;
        pm_real_seg(&s->seg_cs, s->cpu.cs);
        pm_real_seg(&s->seg_ds, s->cpu.ds);
        pm_real_seg(&s->seg_es, s->cpu.es);
        pm_real_seg(&s->seg_ss, s->cpu.ss);
        pm_real_seg(&s->seg_fs, s->fs);
        pm_real_seg(&s->seg_gs, s->gs);
    }
}

uint32_t pm_ea(dos_session_t* s, const pm_seg_t* seg, uint32_t off) {
    uint32_t linear;
    (void)s;
    if (!seg) return off;
    linear = seg->base + off;
    return linear;
}

int pm_far_jump(dos_session_t* s, uint16_t new_cs, uint32_t new_eip) {
    pm_seg_t cs;
    if (!s) return -1;
    if (pm_load_selector(s, new_cs, &cs) != 0) {
        /* If GDT not ready, fall back to real-mode semantics */
        pm_real_seg(&cs, new_cs);
        cs.code = 1;
    }
    s->seg_cs = cs;
    s->cpu.cs = new_cs;
    s->eip = new_eip;
    s->cpu.ip = (uint16_t)(new_eip & 0xFFFF);
    s->cpu32 = (s->pe && cs.use32) ? 1 : 0;
    return 0;
}
