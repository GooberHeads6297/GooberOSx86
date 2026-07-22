/*
 * fb_pat.c -- map the firmware GOP framebuffer Write-Combining via PAT.
 *
 * boot64.s identity-maps the low 4 GiB with 2 MiB pages (flags PS|RW|P only).
 * The GOP LFB therefore inherits the default memory type. On Braswell that
 * leaves the scanout UC or WB, and every large blit either crawls (UC) or
 * goes stale (WB). Linux solves this with ioremap_wc(); we do the same:
 * put WC in PAT slot 1 and retarget the FB PDEs to that slot.
 */

#include "fb_pat.h"
#include "../diagnostics/driver_log.h"

#ifdef __x86_64__

#define MSR_IA32_PAT  0x00000277U

/* Memory types in PAT entries (Intel SDM Vol. 3A). */
#define PAT_TYPE_UC   0x00U
#define PAT_TYPE_WC   0x01U
#define PAT_TYPE_WT   0x04U
#define PAT_TYPE_WP   0x05U
#define PAT_TYPE_WB   0x06U
#define PAT_TYPE_UC_MINUS 0x07U

/* 2 MiB PDE flag bits (PS already set by boot64). */
#define PDE_P     (1ULL << 0)
#define PDE_RW    (1ULL << 1)
#define PDE_PWT   (1ULL << 3)
#define PDE_PCD   (1ULL << 4)
#define PDE_PS    (1ULL << 7)
#define PDE_PAT   (1ULL << 12)  /* PAT bit for 2 MiB pages (not bit 7) */

#define PAGE_2MIB 0x200000ULL

/*
 * boot64.s page tables (identity map of low 4 GiB). Declared global there.
 * Each PD has 512 entries of 2 MiB; four PDs cover 4 GiB.
 */
extern uint64_t pd0[512];
extern uint64_t pd1[512];
extern uint64_t pd2[512];
extern uint64_t pd3[512];

static uint64_t* pd_for_gib(uint32_t gib) {
    switch (gib) {
        case 0: return pd0;
        case 1: return pd1;
        case 2: return pd2;
        case 3: return pd3;
        default: return 0;
    }
}

static void cpuid_u32(uint32_t leaf, uint32_t subleaf,
                      uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    if (a) *a = eax;
    if (b) *b = ebx;
    if (c) *c = ecx;
    if (d) *d = edx;
}

static uint64_t rdmsr_u64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr_u64(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static uint64_t read_cr0(void) {
    uintptr_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return (uint64_t)v;
}

static void write_cr0(uint64_t v) {
    uintptr_t r = (uintptr_t)v;
    __asm__ volatile("mov %0, %%cr0" :: "r"(r) : "memory");
}

static uint64_t read_cr3(void) {
    uintptr_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return (uint64_t)v;
}

static void write_cr3(uint64_t v) {
    uintptr_t r = (uintptr_t)v;
    __asm__ volatile("mov %0, %%cr3" :: "r"(r) : "memory");
}

static int cpu_has_pat(void) {
    uint32_t edx;
    cpuid_u32(1, 0, 0, 0, 0, &edx);
    return (edx & (1U << 16)) != 0; /* CPUID.01H:EDX.PAT */
}

/*
 * Program IA32_PAT so entry 1 (index = PWT=1, PCD=0, PAT=0) is WC.
 * Default reset value has PA1 = WT; replacing WT with WC is the common
 * Linux-style ioremap_wc arrangement and does not disturb PA0=WB.
 *
 * Sequence (Intel SDM, single-CPU): cli, CR0.CD=1, wbinvd, flush TLB,
 * wrmsr PAT, wbinvd, restore CR0, reload CR3, sti.
 */
static int pat_program_wc_slot1(void) {
    uint64_t pat;
    uint64_t old_cr0;
    uint64_t cr3;

    pat = rdmsr_u64(MSR_IA32_PAT);
    /* Clear PA1 (bits 15:8) and set WC. */
    pat = (pat & ~0xFF00ULL) | ((uint64_t)PAT_TYPE_WC << 8);

    __asm__ volatile("cli" ::: "memory");
    old_cr0 = read_cr0();
    /* CD=1, NW=0 */
    write_cr0((old_cr0 | (1ULL << 30)) & ~(1ULL << 29));
    __asm__ volatile("wbinvd" ::: "memory");
    cr3 = read_cr3();
    write_cr3(cr3); /* TLB flush */

    wrmsr_u64(MSR_IA32_PAT, pat);

    __asm__ volatile("wbinvd" ::: "memory");
    write_cr0(old_cr0);
    write_cr3(cr3);
    __asm__ volatile("sti" ::: "memory");

    return 1;
}

/*
 * Retarget every 2 MiB PDE covering [fb_phys, fb_phys+fb_bytes) to PAT
 * index 1 (PWT=1, PCD=0, PAT=0) so they pick up WC from PA1.
 */
static int pde_set_wc_range(uintptr_t fb_phys, uint32_t fb_bytes) {
    uintptr_t start = fb_phys & ~(uintptr_t)(PAGE_2MIB - 1ULL);
    uintptr_t end = (fb_phys + (uintptr_t)fb_bytes + (uintptr_t)PAGE_2MIB - 1ULL)
                    & ~(uintptr_t)(PAGE_2MIB - 1ULL);
    uintptr_t addr;
    int touched = 0;

    if (end <= start) return 0;
    /* Identity map only covers low 4 GiB. */
    if (start >= (1ULL << 32) || end > (1ULL << 32)) return 0;

    for (addr = start; addr < end; addr += (uintptr_t)PAGE_2MIB) {
        uint32_t gib = (uint32_t)(addr >> 30);
        uint32_t idx = (uint32_t)((addr >> 21) & 0x1FFU);
        uint64_t* pd = pd_for_gib(gib);
        uint64_t e;

        if (!pd) return 0;
        e = pd[idx];
        if ((e & PDE_P) == 0 || (e & PDE_PS) == 0) return 0;

        /* PAT index 1: PWT=1, PCD=0, PAT=0 */
        e |= PDE_PWT;
        e &= ~PDE_PCD;
        e &= ~PDE_PAT;
        pd[idx] = e;
        touched++;
    }

    if (touched > 0) {
        write_cr3(read_cr3());
        __asm__ volatile("sfence" ::: "memory");
    }
    return touched > 0;
}

int fb_pat_set_wc(uintptr_t fb_phys, uint32_t fb_bytes) {
    if (fb_phys == 0 || fb_bytes < 0x1000U) return 0;

    if (!cpu_has_pat()) {
        driver_log_line("[display] PAT: CPU does not report PAT support.");
        return 0;
    }

    if (!pat_program_wc_slot1()) {
        driver_log_line("[display] PAT: failed to program WC in PA1.");
        return 0;
    }

    if (!pde_set_wc_range(fb_phys, fb_bytes)) {
        driver_log_line("[display] PAT: failed to retarget FB PDEs to WC.");
        return 0;
    }

    driver_log("[display] PAT WC enabled for framebuffer @");
    driver_log_hex32((uint32_t)fb_phys);
    driver_log(" bytes=");
    driver_log_u32(fb_bytes);
    driver_log_line(".");
    return 1;
}

#else /* !__x86_64__ */

/*
 * The PAT write-combining path is implemented against the x64 long-mode page
 * tables (boot64.s pd0..pd3) and 64-bit MSR/CR handling, so it does not apply to
 * the 32-bit build. Report "not applied" and let the caller fall back to
 * uncached scanout (kernel.c treats a 0 return that way).
 */
int fb_pat_set_wc(uintptr_t fb_phys, uint32_t fb_bytes) {
    (void)fb_phys;
    (void)fb_bytes;
    return 0;
}

#endif /* __x86_64__ */
