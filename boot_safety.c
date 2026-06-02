#include "boot_safety.h"
#include "kernel.h"
#include "drivers/io/io.h"
#include "lib/string.h"
#include "drivers/pci/pci.h"
#include "drivers/timer/timer.h"

/*
 * Boot safety floor implementation: the CPU-fault guard, the per-stage results
 * log, and the read-only pre-bind hardware summary.
 *
 * Logging goes to both the VGA text console (via print()) and the COM/Bochs
 * debug port 0xE9 (matching the rest of the kernel's serial boot log).
 */

#define BOOT_MAX_STAGES 16

/* Guard state. Not nestable; one guard armed at a time. */
static goober_jmp_buf g_guard_env;
static volatile int   g_guard_active = 0;
static const char*    g_guard_stage  = "(none)";

/* Per-stage results log. */
typedef struct {
    const char* name;
    int         status;
} boot_stage_record_t;

static boot_stage_record_t g_stage_log[BOOT_MAX_STAGES];
static int g_stage_count = 0;

/* ---- Serial helpers (0xE9 debug port, like kernel.c/usb.c) ---- */
static void bs_serial(const char* s) {
    while (*s) outb(0xE9, (uint8_t)*s++);
}

static void bs_serial_hex(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
    bs_serial(buf);
}

/* Print to both sinks. */
static void bs_out(const char* s) {
    print(s);
    bs_serial(s);
}

static void bs_out_dec(int v) {
    char buf[16];
    itoa(v, buf, 10);
    bs_out(buf);
}

/* ---- Guard ---- */

int boot_guard_active(void) { return g_guard_active; }
const char* boot_guard_stage_name(void) { return g_guard_stage; }

void boot_guard_longjmp(void) {
    /* Disarm first so a (pathological) fault during the unwind halts instead
     * of recursing back into the same guard. */
    g_guard_active = 0;
    gj_longjmp(g_guard_env, 1);
    /* not reached */
}

/* ---- Stage watchdog ---- */

static volatile int      g_wd_armed    = 0;
static volatile uint32_t g_wd_deadline = 0;

void boot_watchdog_arm(uint32_t timeout_ticks) {
    if (timeout_ticks == 0) { g_wd_armed = 0; return; }
    g_wd_deadline = timer_ticks() + timeout_ticks;
    g_wd_armed = 1;
}

void boot_watchdog_disarm(void) {
    g_wd_armed = 0;
}

void boot_watchdog_tick(void) {
    if (!g_wd_armed) return;
    /* Only meaningful while a stage is actually running under the guard. */
    if (!g_guard_active) { g_wd_armed = 0; return; }
    if ((int32_t)(timer_ticks() - g_wd_deadline) < 0) return;

    /* Budget exceeded inside a guarded stage: force-abort it. Serial only --
     * we are in interrupt context, so avoid the VGA console path here. The
     * EOI was already sent by timer_interrupt_handler() before this call. */
    g_wd_armed = 0;
    bs_serial("[watchdog] stage '");
    bs_serial(g_guard_stage);
    bs_serial("' exceeded its time budget -- forcing abort; boot continues.\n");
    boot_guard_longjmp();   /* does not return */
}

int boot_guarded_run(const char* stage_name, void (*fn)(void)) {
    /*
     * Capture the interrupt-enable flag on entry. The CPU-fault stubs disable
     * interrupts before dispatching, so if we return here via longjmp we must
     * restore IF to the pre-stage state. Kept volatile so it lives on the
     * stack (which longjmp restores) rather than in a register that could be
     * stale across the second return.
     */
    volatile uint32_t eflags;
#ifdef __x86_64__
    {
        uint64_t rflags;
        __asm__ volatile ("pushfq\n\t pop %0" : "=r"(rflags));
        eflags = (uint32_t)rflags;
    }
#else
    __asm__ volatile ("pushf\n\t pop %0" : "=r"(eflags));
#endif
    volatile int if_was_set = (int)((eflags >> 9) & 1u);
    int result;

    g_guard_stage  = stage_name ? stage_name : "(unnamed)";
    g_guard_active = 1;

    if (gj_setjmp(g_guard_env) == 0) {
        fn();
        result = BOOT_GUARD_OK;
    } else {
        /* Resumed here from cpu_exception_handler after a contained fault. */
        result = BOOT_GUARD_FAILED;
    }

    g_guard_active = 0;
    g_guard_stage  = "(none)";

    if (if_was_set) __asm__ volatile ("sti");
    else            __asm__ volatile ("cli");

    return result;
}

/* ---- Results log ---- */

void boot_results_reset(void) {
    g_stage_count = 0;
}

void boot_record_stage(const char* name, int status) {
    if (g_stage_count >= BOOT_MAX_STAGES) return;
    g_stage_log[g_stage_count].name   = name;
    g_stage_log[g_stage_count].status = status;
    g_stage_count++;
}

static const char* status_str(int status) {
    switch (status) {
        case BOOT_STAGE_OK:      return "OK";
        case BOOT_STAGE_FAILED:  return "FAILED";
        case BOOT_STAGE_SKIPPED: return "SKIPPED";
        default:                 return "PENDING";
    }
}

void boot_print_results_summary_titled(const char* title) {
    bs_out("\n=== ");
    bs_out(title ? title : "Boot stage results");
    bs_out(" ===\n");
    for (int i = 0; i < g_stage_count; i++) {
        bs_out("  [");
        bs_out(status_str(g_stage_log[i].status));
        bs_out("] ");
        bs_out(g_stage_log[i].name ? g_stage_log[i].name : "(unnamed)");
        bs_out("\n");
    }
}

void boot_print_results_summary(void) {
    boot_print_results_summary_titled(NULL);
}

/* ---- Hardware summary (read-only PCI scan via existing helpers) ---- */

static const char* usb_type_name(uint8_t prog_if) {
    switch (prog_if) {
        case 0x00: return "UHCI";
        case 0x10: return "OHCI";
        case 0x20: return "EHCI";
        case 0x30: return "xHCI";
        default:   return "USB?";
    }
}

static void bs_print_bdf(uint8_t bus, uint8_t slot, uint8_t func) {
    bs_out_dec((int)bus);  bs_out(":");
    bs_out_dec((int)slot); bs_out(":");
    bs_out_dec((int)func);
}

static void bs_print_ids(uint16_t vendor, uint16_t device) {
    print("[");
    bs_serial("[");
    char buf[16];
    itoa((int)vendor, buf, 16); print(buf); bs_serial(buf);
    print(":"); bs_serial(":");
    itoa((int)device, buf, 16); print(buf); bs_serial(buf);
    print("]");
    bs_serial("]");
}

void boot_print_hardware_summary(void) {
    bs_out("\n=== Hardware summary (PCI, lspci-style) ===\n");

    pci_display_device_t disp[8];
    int nd = pci_find_display_controllers(disp, 8);
    bs_out("Display controllers: ");
    bs_out_dec(nd);
    bs_out("\n");
    for (int i = 0; i < nd && i < 8; i++) {
        bs_out("  ");
        bs_print_bdf(disp[i].bus, disp[i].slot, disp[i].func);
        bs_out(" display ");
        bs_print_ids(disp[i].vendor_id, disp[i].device_id);
        bs_out("\n");
    }

    usb_pci_controller_t usbc[8];
    int nu = pci_find_usb_controllers(usbc, 8);
    bs_out("USB controllers: ");
    bs_out_dec(nu);
    bs_out("\n");
    for (int i = 0; i < nu && i < 8; i++) {
        bs_out("  ");
        bs_print_bdf(usbc[i].bus, usbc[i].slot, usbc[i].func);
        bs_out(" ");
        bs_out(usb_type_name(usbc[i].prog_if));
        bs_out(" ");
        bs_print_ids(usbc[i].vendor_id, usbc[i].device_id);
        bs_out("\n");
    }
}
