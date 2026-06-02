#ifndef BOOT_SAFETY_H
#define BOOT_SAFETY_H

#include <stdint.h>

/*
 * Boot safety floor.
 *
 * This is the cross-cutting framework that lets GooberOS degrade gracefully
 * like a Linux Mint Live CD: risky driver bring-up runs under a CPU-fault
 * guard so that a fault during ANY stage is contained (the stage is marked
 * FAILED, state is unwound, and the next stage still runs) instead of halting
 * the whole machine. See boot_safety.c and kernel.c (kernel_main).
 */

/* ---- Freestanding setjmp/longjmp for the boot fault guard ----
 *
 * Implemented by setjmp.s on the 32-bit build and setjmp64.s on the 64-bit
 * build. Layouts differ because the System V ABIs disagree on the
 * callee-saved set:
 *   x86 (i386):     [ebx, esi, edi, ebp, esp, eip]                  -> 6 dwords
 *   x86_64:         [rbx, rbp, rsp, r12, r13, r14, r15, rip]        -> 8 qwords
 * The function prototypes are ABI-stable across both arches because the
 * typedef carries the size; callers never index into goober_jmp_buf
 * directly. */
#ifdef __x86_64__
typedef uint64_t goober_jmp_buf[8];
#else
typedef uint32_t goober_jmp_buf[6];
#endif
int  gj_setjmp(goober_jmp_buf env);
void gj_longjmp(goober_jmp_buf env, int val);

/* ---- Guard API ---- */

/* boot_guarded_run() return codes. */
#define BOOT_GUARD_OK      0
#define BOOT_GUARD_FAILED  (-1)

/*
 * Run fn() under an active CPU-fault guard, identified by stage_name.
 *
 * Returns BOOT_GUARD_OK (0) if fn() ran to completion, or BOOT_GUARD_FAILED
 * if a CPU exception fired while the guard was active (in which case
 * cpu_exception_handler longjmp'd back here). The interrupt-enable state is
 * restored to whatever it was on entry, so the orchestrator can keep going.
 *
 * Guards are not nestable; each call arms exactly one guard for the duration
 * of fn().
 */
int boot_guarded_run(const char* stage_name, void (*fn)(void));

/* Queried by cpu_exception_handler (kernel.c) to decide contain-vs-halt. */
int         boot_guard_active(void);
const char* boot_guard_stage_name(void);
/* Disarm the guard and longjmp back into boot_guarded_run. Does not return. */
void        boot_guard_longjmp(void);

/* ---- Stage watchdog (timer-driven forced abort) ----
 *
 * The CPU-fault guard only contains *exceptions*. A driver that spins or
 * stalls (e.g. a wedged USB host controller on real hardware) would still
 * hang the whole boot. The watchdog closes that gap: arm it with a tick
 * budget before running a guarded stage, and the timer ISR will force-abort
 * the stage (via boot_guard_longjmp) if it overruns. This works as long as
 * IRQ0 keeps firing (interrupts are enabled for all risky stages), which is
 * the case on hardware without a legacy-USB SMM trap.
 *
 * Budgets are in 100 Hz ticks (1 tick = 10 ms). Arm 0 ticks to disable.
 */
void boot_watchdog_arm(uint32_t timeout_ticks);
void boot_watchdog_disarm(void);
/* Called from the timer ISR every tick. No-op unless a guarded stage has
 * overrun its watchdog budget, in which case it does not return (longjmp). */
void boot_watchdog_tick(void);

/* ---- Per-stage results log ---- */

#define BOOT_STAGE_PENDING 0
#define BOOT_STAGE_OK      1
#define BOOT_STAGE_FAILED  2
#define BOOT_STAGE_SKIPPED 3

void boot_results_reset(void);
void boot_record_stage(const char* name, int status);
void boot_print_results_summary(void);
/* Like boot_print_results_summary(), but with a caller-supplied header
 * (e.g. "Self-test results"). Used by the x64 orchestrator to print the
 * gooberos.selftest=1 fault/watchdog probes in their own clearly-labeled
 * section, leaving the main "Boot stage results" all-green on a healthy
 * boot. Pass NULL to keep the default "Boot stage results" header. */
void boot_print_results_summary_titled(const char* title);

/* ---- Pre-bind hardware summary (lspci-style, read-only) ---- */
void boot_print_hardware_summary(void);

#endif
