#ifndef CPU_EXCEPTION_NAMES_H
#define CPU_EXCEPTION_NAMES_H

/*
 * Shared CPU exception name table for vectors 0..31.
 *
 * Defined here as a `static const char* const` so both the 32-bit
 * cpu_exception_handler in kernel.c and the 64-bit one in kernel_x64.c
 * can use the same source-of-truth without duplicating the table by hand.
 *
 * Lifted out by Phase 3b.0 of the UEFI/GOP/x86_64 migration; prior to
 * 3b.0 each translation unit had its own copy and they could drift.
 *
 * Each translation unit that includes this header gets its own .rodata
 * copy at link time; --gc-sections drops the unused one if it isn't
 * referenced. The marginal duplication is preferable to threading a
 * separate cpu_exception_names.c through both build scripts just to
 * coalesce ~256 bytes of strings.
 */
static const char* const exception_names[32] = {
    "#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint",
    "#OF Overflow", "#BR BOUND", "#UD Invalid Opcode", "#NM Device N/A",
    "#DF Double Fault", "Coproc Seg Overrun", "#TS Invalid TSS", "#NP Segment Not Present",
    "#SS Stack-Segment", "#GP General Protection", "#PF Page Fault", "Reserved",
    "#MF x87 FPU", "#AC Alignment Check", "#MC Machine Check", "#XM SIMD FP",
    "#VE Virtualization", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "#SX Security", "Reserved",
};

#endif
