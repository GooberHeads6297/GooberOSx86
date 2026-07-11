#include "fb_cache.h"
#include "../diagnostics/driver_log.h"

#define MSR_IA32_MTRRCAP      0x000000FEU
#define MSR_IA32_MTRR_DEF_TYPE 0x000002FFU
#define MSR_IA32_MTRR_PHYSBASE0 0x00000200U
#define MSR_IA32_MTRR_PHYSMASK0 0x00000201U

#define MTRR_TYPE_WC          0x01U
#define MTRR_DEF_ENABLE       (1ULL << 11)
#define MTRR_MASK_VALID       (1ULL << 11)

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
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
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

static uint32_t phys_addr_bits(void) {
    uint32_t max_ext, eax;
    cpuid_u32(0x80000000U, 0, &max_ext, 0, 0, 0);
    if (max_ext >= 0x80000008U) {
        cpuid_u32(0x80000008U, 0, &eax, 0, 0, 0);
        if ((eax & 0xFFU) >= 32 && (eax & 0xFFU) <= 52)
            return eax & 0xFFU;
    }
    return 36;
}

static uint64_t phys_mask_all(void) {
    uint32_t bits = phys_addr_bits();
    if (bits >= 52) return 0x000FFFFFFFFFF000ULL;
    return (((1ULL << bits) - 1ULL) & 0xFFFFFFFFFFFFF000ULL);
}

static uint32_t largest_pow2_le(uint32_t v) {
    uint32_t p = 1U;
    while ((p << 1) && (p << 1) <= v) p <<= 1;
    return p;
}

static uint32_t choose_chunk(uintptr_t addr, uint32_t remaining) {
    uint32_t chunk = largest_pow2_le(remaining);
    while (chunk > 0x1000U && ((uint32_t)addr & (chunk - 1U)) != 0)
        chunk >>= 1;
    if (chunk < 0x1000U) chunk = 0x1000U;
    return chunk;
}

static int find_free_mtrr(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t mask = rdmsr_u64(MSR_IA32_MTRR_PHYSMASK0 + i * 2U);
        if ((mask & MTRR_MASK_VALID) == 0) return (int)i;
    }
    return -1;
}

static void program_wc_mtrr(int idx, uintptr_t base, uint32_t size, uint64_t phys_mask) {
    uint64_t b = ((uint64_t)base & phys_mask) | MTRR_TYPE_WC;
    uint64_t m = (phys_mask & ~((uint64_t)size - 1ULL)) | MTRR_MASK_VALID;
    wrmsr_u64(MSR_IA32_MTRR_PHYSBASE0 + (uint32_t)idx * 2U, b);
    wrmsr_u64(MSR_IA32_MTRR_PHYSMASK0 + (uint32_t)idx * 2U, m);
}

int fb_cache_enable_write_combining(uintptr_t fb_addr, uint32_t fb_bytes) {
    uint32_t eax, edx;
    uint64_t cap;
    uint32_t var_count;
    uint64_t def_type;
    uint64_t phys_mask;
    uint32_t programmed = 0;

    if (fb_addr == 0 || fb_bytes < 0x1000U) return 0;
    if ((fb_addr & 0xFFFU) != 0) {
        driver_log_line("[display] WC: framebuffer base is not 4KB aligned; skipping MTRR WC.");
        return 0;
    }

    cpuid_u32(1, 0, &eax, 0, 0, &edx);
    (void)eax;
    if ((edx & (1U << 12)) == 0) {
        driver_log_line("[display] WC: CPU does not report MTRR support.");
        return 0;
    }

    cap = rdmsr_u64(MSR_IA32_MTRRCAP);
    var_count = (uint32_t)(cap & 0xFFU);
    if (var_count == 0) {
        driver_log_line("[display] WC: CPU reports zero variable MTRRs.");
        return 0;
    }

    def_type = rdmsr_u64(MSR_IA32_MTRR_DEF_TYPE);
    if ((def_type & MTRR_DEF_ENABLE) == 0) {
        driver_log_line("[display] WC: MTRRs are disabled by firmware; leaving framebuffer uncached.");
        return 0;
    }

    phys_mask = phys_mask_all();
    uint32_t rounded = (fb_bytes + 0xFFFU) & ~0xFFFU;
    uintptr_t cur = fb_addr;
    uint32_t remaining = rounded;
    uint64_t old_cr0;

    __asm__ volatile("cli" ::: "memory");
    old_cr0 = read_cr0();
    write_cr0((old_cr0 | (1ULL << 30)) & ~(1ULL << 29)); /* CD=1, NW=0 */
    __asm__ volatile("wbinvd" ::: "memory");

    while (remaining > 0 && programmed < 6U) {
        int idx = find_free_mtrr(var_count);
        if (idx < 0) break;
        uint32_t chunk = choose_chunk(cur, remaining);
        program_wc_mtrr(idx, cur, chunk, phys_mask);
        cur += chunk;
        remaining -= chunk;
        programmed++;
    }

    __asm__ volatile("wbinvd" ::: "memory");
    write_cr0(old_cr0);
    __asm__ volatile("sti" ::: "memory");

    if (remaining != 0) {
        driver_log("[display] WC: only covered ");
        driver_log_u32(rounded - remaining);
        driver_log(" of ");
        driver_log_u32(rounded);
        driver_log_line(" framebuffer bytes; not enough free MTRRs.");
    } else if (programmed > 0) {
        driver_log("[display] WC: framebuffer write-combining enabled with ");
        driver_log_u32(programmed);
        driver_log_line(" MTRR range(s).");
    }

    return remaining == 0 && programmed > 0;
}
