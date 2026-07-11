#!/bin/bash
# -----------------------------------------------------------------------------
# scripts/build-x64.sh
#
# 64-bit x86_64 (long-mode) build target. This is the source-side counterpart
# to the toolchain infrastructure that was already in place; with Phase 1 of
# the UEFI/GOP/x86_64 migration plan complete it now produces a working
# long-mode kernel binary + hybrid BIOS + UEFI ISO.
#
# Phase 1 deliverable: a minimal long-mode kernel (kernel_x64.c) that
# initializes COM1 and prints proof-of-life over serial under QEMU + OVMF.
# Phases 2-3 will widen the build to compile the full driver stack again
# (IDT/IRQ/setjmp 64-bit port; framebuffer + driver bring-up). Until then
# this script DELIBERATELY only assembles boot64.s + gdt64.s and compiles
# kernel_x64.c so the milestone is independently testable from the rest
# of the kernel.
#
# Produces:
#   - build64/kernel.bin    ELF64 multiboot1+2 kernel (loaded at 1 MiB,
#                            identity-mapped low 4 GiB by boot64.s).
#   - GooberOSx86-x64.iso   Hybrid BIOS + UEFI ISO (independent from x86 ISO).
#
# Sub-commands (positional, defaults to "build"):
#   build           Compile + link + ISO (with embedded installer image).
#   list-devices    Print host block devices (shared with x86 path).
#   install         Install to a mounted target device (grub --target=x86_64-efi).
#
# Environment:
#   X64_CC          Override the C compiler (default: gcc; cross-compilers like
#                   x86_64-elf-gcc also work if installed).
#   X64_LD          Override the linker (default: ld).
#   X64_ALLOW_FAIL  If "1", treat compile/link failures as a non-fatal warning
#                   so CI can still produce the hybrid ISO from a previously
#                   built kernel.bin (handy during phase rollout).
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR=build64
ISO_DIR=iso64
ISO_OUTPUT="GooberOSx86-x64.iso"
EMBED_INSTALL_ISO="${EMBED_INSTALL_ISO:-0}"
EMBED_INSTALL_PAYLOAD="${EMBED_INSTALL_PAYLOAD:-1}"
CC="${X64_CC:-gcc}"
LD="${X64_LD:-ld}"
NASM="nasm"
ALLOW_FAIL="${X64_ALLOW_FAIL:-0}"

# x86_64 freestanding flags. -mno-red-zone is required for kernels (interrupts
# clobber the red zone). -mcmodel=kernel keeps RIP-relative addressing valid
# for kernels loaded in the upper canonical address space; we still load at
# 1 MiB today (identity-mapped) so this is forward-compatible. -mno-mmx/sse
# avoids the FPU/SSE state being touched before we set up CR0/CR4 for it.
CFLAGS_BASE="-ffreestanding -m64 -O2 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
-fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
-mcmodel=kernel -ffunction-sections -fdata-sections \
-Wall -Wextra -Wno-unused-parameter -Wno-unused-function"
EXTRA_DEFS=""

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/build-common.sh"

usage() {
  cat <<'EOF'
Usage:
  ./scripts/build-x64.sh                       Build x64 kernel + hybrid ISO
  ./scripts/build-x64.sh build                 Same as above
  ./scripts/build-x64.sh list-devices          List installable block devices
  ./scripts/build-x64.sh install --device /dev/sdX --mount /mnt/goober

Status:
  Phase 1 (toolchain + 64-bit boot bring-up) is COMPLETE and produces a
  minimal long-mode kernel that prints to serial under QEMU+OVMF.
  Phases 2 and 3 of the UEFI/GOP/x64 migration plan will reintroduce the
  IDT/IRQ/setjmp 64-bit port and the full driver stack respectively.

Environment:
  X64_CC, X64_LD       Override toolchain.
  X64_ALLOW_FAIL=1     Treat compile/link errors as non-fatal warnings.
EOF
}

set_embed_defs() {
  if [ "${1}" = "1" ]; then
    EXTRA_DEFS="-DEMBED_INSTALL_PAYLOAD=1"
    if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
      EXTRA_DEFS="${EXTRA_DEFS} -DEMBED_INSTALL_ISO=1"
    fi
  else
    EXTRA_DEFS=""
  fi
}

compile_c() {
  ${CC} ${CFLAGS_BASE} ${EXTRA_DEFS} "$@"
}

print_migration_banner() {
  cat <<'EOF'

============================================================================
GooberOSx86  x86_64 long-mode build
----------------------------------------------------------------------------
Phase 1    (toolchain + 32->64 trampoline + identity paging)  -- COMPLETE.
Phase 2    (64-bit IDT/IRQ/setjmp + PIC + PIT + PS/2 + guard) -- COMPLETE.
Phase 3a   (GOP framebuffer tag walk + RGB test pattern +     -- COMPLETE.
            VGA-text floor + lib/string + lib/memory +
            drivers/timer compile-clean under -m64)
Phase 3a.1 (channel-aware 16/24/32 bpp test pattern +         -- COMPLETE.
            embedded 8x16 font + on-panel proof-of-life
            via fb_putc/fb_puts/fb_print_at)
Phase 3b.0 (boot_safety.c linked; real cpu_exception_handler  -- COMPLETE.
            + timer_init(100) + timer_calibrate_tsc() +
            timer_interrupt_handler() driving the IRQ0 ISR;
            isr32_stub64.s no longer double-acks the master PIC)
Phase 3b   (drivers/video/{vga,display,native_fb,vesa,         -- COMPLETE.
            intel_gfx} compile + link under -m64;
            display_register_framebuffer / kernel_fb_addr /
            native_fb_set_inherited / display_framebuffer_t::
            framebuffer_addr widened to uintptr_t end-to-end;
            unified kernel.c::kernel_main runs on x64 driving
            boot_run_stages_table(k_boot_stages_x64) through
            boot_safety.c's guard + watchdog; the framebuffer
            test pattern + 8x16 proof-of-life are preserved
            via the new kernel_x64.c::x64_arch_walk_and_draw_
            framebuffer() helper called from kernel_main)
Phase 3c   (drivers/input/input.c, drivers/keyboard/keyboard.c, -- COMPLETE.
            drivers/mouse/mouse.c linked under -m64; PS/2 IRQ1
            calls real keyboard_interrupt_handler; PS/2 IRQ12
            calls the real mouse_handler_main; on-panel
            confirm-or-revert gate runs on x64; on-panel line
            editor with help/clear/halt commands; fault +
            watchdog probes promoted to gooberos.selftest=1
            so the main results table is all-green by default)
Phase 3d   (drivers/pci/pci.c + USB host stack +              -- COMPLETE.
            drivers/usb/host/{uhci,ohci,ehci,xhci}.c +
            drivers/usb/host/host.c +
            drivers/usb/core/enumeration.c +
            drivers/usb/hid/hid.c + drivers/usb/usb.c +
            drivers/usb/storage/msc.c (stub-only, no storage
            stack pulled in) all linked under -m64; the placeholder
            pci_find_display_controllers / pci_find_usb_controllers
            stubs in kernel_x64.c are gone -- the real PCI scan
            now drives boot_print_hardware_summary's lspci-style
            output. A new `USB host stack` boot stage placed between
            Hardware summary and Display, bounded by a 12-second
            watchdog (WD_USB = 1200 ticks @ 100 Hz) so a wedged
            controller can never wedge the boot. Every existing
            x86 USB hardening property is preserved verbatim:
            UHCI USBLEGSUP clear / OHCI HcControl.IR force-release
            / EHCI extended-cap handoff / xHCI USBLEGCTLSTS clear;
            Intel xHCI XUSB2PR + USB3_PSSEN port-routing writes
            gated on EHCI-companion presence so Bay Trail's xHCI-
            only routing is skipped; TSC-bounded waits everywhere;
            per-port + per-controller + per-scan budgets in the
            enumeration loop; host_faulted per-controller fault
            isolation; gooberos.usb=off + gooberos.usb=safe
            cmdline switches both honored. USB-HID events flow
            into the existing drivers/input/input.c queue so the
            goober> REPL reads keystrokes from a USB keyboard
            transparently.)
Phase 3e   (fs/filesystem.c + shell/shell.c + gui/window.c +  -- COMPLETE.
            gui/vesa_window.c + gui/desktop_vesa.c all linked
            under -m64; new Filesystem + Shell / desktop boot
            stages slotted into k_boot_stages_x64[] after
            Display / framebuffer; the desktop event pump is
            the final landing surface, with the Phase 3c REPL
            kept as a fallback when the desktop init stage
            faults or its 30-second watchdog overruns. The
            shell parser still recognises every command name
            but every storage / editor / taskmgr / games target
            on x64 prints `[shell] <cmd>: deferred to phase 3f`
            via a shell_deferred_3f() stub so 3f is a drop-in.
            shell.c::reboot() now uses the 8042 keyboard-
            controller reset on x64 (outb(0x64, 0xFE)) instead
            of the 32-bit invalid-IDT triple-fault path. The
            full taskmgr/process.c surface (process_table /
            process_count / create_process / terminate_process
            / process_is_protected / get_kernel_process_table /
            get_kernel_process_count / update_process_runtime_
            metrics / kill_process) is stubbed in kernel_x64.c
            so the desktop's Task Manager window shows the
            vesa-* pseudo-processes registered by the
            open_*_window helpers without dragging in taskmgr/
            (deferred to 3f).)
Phase 3f   (drivers/storage/{storage,sdhci}.c + editor/       -- COMPLETE.
            editor.c + taskmgr/{taskmgr,process}.c + games/
            {snake,cubeDip,pong,doom}.c all linked under -m64;
            the Phase 3e shell_deferred_3f() stub is gone, the
            shell command parser is identical between x86 and
            x64. drivers/storage/bios_disk.c stays #ifdef
            __i386__-gated (BIOS int 13h is real-mode only)
            and drivers/storage/bios_int13.s is excluded from
            this build script entirely (real-mode opcodes can
            not link into long mode). Storage stack picks up
            ATA PIO / AHCI / NVMe / SDHCI / USB-MSC controllers
            via the PCI scan brought up in stage_x64_hwsummary;
            the new `Storage` boot stage runs storage_init()
            under the WD_STORAGE 5 s watchdog. The kernel_x64.c
            Phase 3e taskmgr/process.c stub block has been
            DELETED -- taskmgr/process.c is the source of
            truth on both arches, and kernel.c::register_
            kernel_process_x64() seeds pid=1 = "kernel.bin"
            so process_is_protected(1) keeps the kernel entry
            unkillable. The 1 MiB g_x64_kernel_heap[] BSS bump
            allocator is replaced by a 4 MiB free-list
            allocator with coalescing in lib/memory.c (kfree
            actually frees, krealloc grows in place when the
            next phys neighbour is free, alloc + copy + free
            otherwise). x86 still uses its bump allocator;
            both arches see the same kmalloc/kfree/krealloc
            surface. shell.c::reboot() on x64 now tries the
            ACPI 5.0 RESET_REG GAS first (locating the FADT
            via RSDP -> XSDT/RSDT, parsing flags + GAS +
            RESET_VALUE), and falls back to the 8042 keyboard-
            controller reset if ACPI is unavailable / FADT
            does not advertise RESET_REG_SUP / the GAS
            address-space-id is unsupported. WD_DESKTOP bumped
            from 30 s to 60 s so storage-driven icon enumera-
            tion doesn't trip the watchdog on a slow USB MSC.
            New gooberos.display.confirm=skip|force|default
            cmdline switch overrides the run_confirm default
            (x64 always-on -> CI smoke tests can pass `skip`
            to bypass the prompt without touching code). The
            Phase 3 umbrella is now COMPLETE: the x64 build
            has full feature parity with x86 except for the
            BIOS int 13h installer path which is fundamentally
            real-mode-only.)
Phase 4    (real-hardware validation)                         -- pending.

Phase 2 links in cpu_exceptions64.s, isr32_stub64.s, irq1/12_wrapper64.s,
setjmp64.s, and idt_load64.s alongside the Phase 1 trampoline.

Phase 3a added three pointer-width-clean C translation units to the link:
lib/string.c, lib/memory.c, drivers/timer/timer.c.

Phase 3b.0 brought boot_safety.c into the x64 link with kernel.h + lib/string.h
+ drivers/pci/pci.h + drivers/timer/timer.h pulled in as-is.

Phase 3b lifts the unified staged-boot orchestrator (kernel.c) into the x64
link, replacing the kernel_x64.c Phase 1-3a.1 entry path. kernel_x64.c shrinks
to the x64-only helpers the orchestrator calls (serial init, IDT install, PIC
remap, multiboot dump, framebuffer test pattern + 8x16 proof-of-life, legacy
0xB8000 line). drivers/video/{vga,display,native_fb,vesa,intel_gfx} all link
under -m64; the framebuffer base traveled through display_register_framebuffer
/ native_fb_set_inherited / display_framebuffer_t::framebuffer_addr / kernel_
fb_addr is now `uintptr_t` so a 64-bit GOP base survives end-to-end. The
kernel.c-only symbols boot_safety.c uses on its boot-summary path (pci_find_
display_controllers / pci_find_usb_controllers) remain stubbed in kernel_x64.c
(Phase 3d wires up the real drivers/pci/pci.c). The deliberate-#BP test from
3b.0 and the watchdog overrun probe are now stages in k_boot_stages_x64[] so
the orchestrator runs (and verifies) the contain-and-continue path itself.
============================================================================

EOF
}

build_image() {
  local payload_requested="${EMBED_INSTALL_PAYLOAD}"

  print_migration_banner

  if ! build_kernel 0 ""; then
    if [ "${ALLOW_FAIL}" = "1" ]; then
      echo "[!] x64 build failed; X64_ALLOW_FAIL=1, skipping ISO step." >&2
      return 0
    fi
    echo "[x] x64 build failed. Re-run with X64_ALLOW_FAIL=1 to scaffold an ISO without the kernel." >&2
    exit 1
  fi

  if [ "${payload_requested}" = "1" ]; then
    prepare_install_payload
    link_install_payload_objects_x64
    if ! build_kernel 1 ""; then
      echo "[x] x64 build failed while linking install payload." >&2
      exit 1
    fi
    # ESP image must carry the final (payload-linked) kernel, not pass-1.
    refresh_fat_template_with_final_kernel
  fi

  if [ "${EMBED_INSTALL_ISO}" = "1" ]; then
    rm -f "${BUILD_DIR}/osimage.o"
    ${LD} -m elf_x86_64 -r -b binary "${ISO_OUTPUT}" -o "${BUILD_DIR}/osimage.o"
    if ! build_kernel 1 "${BUILD_DIR}/osimage.o"; then
      echo "[x] x64 build failed while embedding installer ISO." >&2
      exit 1
    fi
    refresh_fat_template_with_final_kernel
  fi

  stage_iso_install_files
  create_iso_hybrid
}

# Phase 3b kernel: the unified kernel.c orchestrator is now the long-mode
# kernel_main entry point (boot64.s calls `kernel_main`, kernel.c provides
# it under `#ifdef __x86_64__`). kernel_x64.c shrinks to the x64-only
# helpers kernel.c calls into (serial init, IDT install, PIC remap,
# multiboot dump, framebuffer test pattern + 8x16 proof-of-life, legacy
# 0xB8000 line) plus the long-mode IRQ handler bodies and the PCI scan
# stubs boot_safety.c references.
#
# Driver translation units linked under -m64 (Phase 3b):
#   drivers/video/vga.c       -- the 0xB8000 text console (legacy BIOS only
#                                under UEFI it's a write-to-nowhere) and the
#                                vga_put_char / vga_set_text_color / clear_
#                                screen that the print fall-through uses.
#   drivers/video/display.c   -- the display framework registry, the VGA
#                                text-mode restore path, and the framework's
#                                framebuffer descriptor (now uintptr_t-clean).
#   drivers/video/native_fb.c -- the "vesa" (inherited LFB) + "bochs" (BGA
#                                dispi) drivers.
#   drivers/video/vesa.c      -- the LFB pixel-writer + 8x16 string renderer
#                                + boot splash. vesa_init now stores the
#                                framebuffer as a uintptr_t so a 64-bit GOP
#                                base survives end-to-end.
#   drivers/video/intel_gfx.c -- the read-only scanout-health probe + the
#                                plane-repoint driver. Detect-only on x64
#                                for now (pci.c is still a stub, so the
#                                Intel rung never fires unless the user
#                                explicitly forces it via gooberos.display=
#                                intel AND a real pci.c is linked -- both
#                                Phase 3d).
#
# We deliberately still do NOT compile drivers/input, drivers/keyboard,
# drivers/mouse, drivers/pci/pci.c (a real one -- the stubs in kernel_x64.c
# stand in), drivers/usb, drivers/storage, fs, shell, editor, taskmgr,
# games, gui. Those are Phase 3c / 3d / 3e / 3f.
build_kernel() {
  local embed_flag="$1"
  local osimage_obj="${2:-}"
  local install_objs=""

  set_embed_defs "${embed_flag}"
  mkdir -p "${BUILD_DIR}"
  if [ "${embed_flag}" = "1" ]; then
    install_objs="${BUILD_DIR}/install_kernel_payload.o ${BUILD_DIR}/install_grub_cfg.o ${BUILD_DIR}/install_boot_img.o ${BUILD_DIR}/install_core_img.o"
  fi

  # osimage_obj is present on the second build pass when EMBED_INSTALL_ISO=1.
  # The shell's `install write <target-id> YES` command streams it to disk.

  echo "[x64] Assembling Phase 1 boot stubs (elf64)..."
  ${NASM} -f elf64 boot64.s -o "${BUILD_DIR}/boot64.o"
  ${NASM} -f elf64 gdt64.s  -o "${BUILD_DIR}/gdt64.o"

  echo "[x64] Assembling Phase 2 IDT/IRQ/setjmp stubs (elf64)..."
  ${NASM} -f elf64 cpu_exceptions64.s -o "${BUILD_DIR}/cpu_exceptions64.o"
  ${NASM} -f elf64 isr32_stub64.s     -o "${BUILD_DIR}/isr32_stub64.o"
  ${NASM} -f elf64 irq1_wrapper64.s   -o "${BUILD_DIR}/irq1_wrapper64.o"
  ${NASM} -f elf64 irq12_wrapper64.s  -o "${BUILD_DIR}/irq12_wrapper64.o"
  ${NASM} -f elf64 setjmp64.s         -o "${BUILD_DIR}/setjmp64.o"
  ${NASM} -f elf64 idt_load64.s       -o "${BUILD_DIR}/idt_load64.o"

  echo "[x64] Compiling Phase 3b unified orchestrator (kernel.c, x64 mode)..."
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/timer -Idrivers/video -Ilib \
    -c kernel.c -o "${BUILD_DIR}/kernel.o"

  echo "[x64] Compiling x64-only helpers (kernel_x64.c)..."
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/timer -Ilib \
    -c kernel_x64.c -o "${BUILD_DIR}/kernel_x64.o"

  echo "[x64] Compiling Phase 3a runtime libs (lib/string, lib/memory)..."
  compile_c -I. -Idrivers/io -c lib/string.c -o "${BUILD_DIR}/string.o"
  compile_c -I. -Idrivers/io -c lib/memory.c -o "${BUILD_DIR}/memory.o"

  echo "[x64] Compiling Phase 3a drivers/timer (PIT + rdtsc deadline clock)..."
  compile_c -I. -Idrivers/io -c drivers/timer/timer.c -o "${BUILD_DIR}/timer.o"

  echo "[x64] Compiling Phase 3b.0 boot_safety.c (real boot fault guard)..."
  compile_c -I. -Idrivers/io -Idrivers/pci -Ilib \
    -c boot_safety.c -o "${BUILD_DIR}/boot_safety.o"

  echo "[x64] Compiling Phase 3b drivers/video (vga, display, native_fb,
     vesa, intel_gfx) under -m64..."
  compile_c -I. -Idrivers/io \
    -c drivers/video/vga.c -o "${BUILD_DIR}/vga.o"
  compile_c -I. -Ifs \
    -c drivers/diagnostics/driver_log.c -o "${BUILD_DIR}/driver_log.o"
  compile_c -I. -Idrivers/diagnostics \
    -c drivers/video/edid.c -o "${BUILD_DIR}/edid.o"
  compile_c -I. -Idrivers/diagnostics -Idrivers/video -Idrivers/io \
    -c drivers/video/connector.c -o "${BUILD_DIR}/connector.o"
  compile_c -I. -Idrivers/diagnostics \
    -c drivers/video/fb_cache.c -o "${BUILD_DIR}/fb_cache.o"
  compile_c -I. -Idrivers/io \
    -c drivers/video/display.c -o "${BUILD_DIR}/display.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/bios -Idrivers/diagnostics \
    -c drivers/video/basic_display.c -o "${BUILD_DIR}/basic_display.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/bios -Idrivers/diagnostics \
    -c drivers/video/bios_vbe.c -o "${BUILD_DIR}/bios_vbe.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/timer \
    -c drivers/video/native_fb.c -o "${BUILD_DIR}/native_fb.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/mouse -Ilib \
    -c drivers/video/vesa.c -o "${BUILD_DIR}/vesa.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/pci -Idrivers/diagnostics \
    -c drivers/video/intel_gfx.c -o "${BUILD_DIR}/intel_gfx.o"

  # textcon: 80x25 text-console abstraction with VGA (0xB8000) and
  # framebuffer (8x16 font into the top-left of the GOP LFB) backends.
  # Required to give the x64 VGA-compatibility GRUB entries a visible,
  # interactive shell on both legacy BIOS and UEFI hardware.
  echo "[x64] Compiling drivers/video/textcon.c (text-console backends) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video \
    -c drivers/video/textcon.c -o "${BUILD_DIR}/textcon.o"

  echo "[x64] Compiling Phase 3c drivers (input, keyboard, mouse) under -m64..."
  compile_c -I. -Idrivers/io \
    -c drivers/input/input.c -o "${BUILD_DIR}/input.o"
  compile_c -I. -Idrivers/io -Idrivers/input -Idrivers/acpi -Idrivers/hid -Idrivers/i2c -Ilib \
    -c drivers/input/touchpad.c -o "${BUILD_DIR}/touchpad.o"
  compile_c -I. -Idrivers/io \
    -c drivers/keyboard/keyboard.c -o "${BUILD_DIR}/keyboard.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/input \
    -c drivers/mouse/mouse.c -o "${BUILD_DIR}/mouse.o"

  echo "[x64] Compiling ACPI + I2C HID touchpad support under -m64..."
  compile_c -I. -Idrivers/io -Ilib \
    -c drivers/acpi/acpi.c -o "${BUILD_DIR}/acpi.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/timer -Ilib \
    -c drivers/i2c/designware.c -o "${BUILD_DIR}/i2c_designware.o"
  compile_c -I. -Idrivers/io -Idrivers/i2c -Ilib \
    -c drivers/hid/i2c_hid.c -o "${BUILD_DIR}/i2c_hid.o"

  echo "[x64] Compiling Phase 3d drivers/pci/pci.c (real PCI scan) under -m64..."
  compile_c -I. -Idrivers/io -Ilib \
    -c drivers/pci/pci.c -o "${BUILD_DIR}/pci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Ilib \
    -c drivers/pci/iosf_mbi.c -o "${BUILD_DIR}/iosf_mbi.o"

  echo "[x64] Compiling Phase 3d USB host stack (uhci/ohci/ehci/xhci/host) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci \
    -c drivers/usb/host/uhci.c -o "${BUILD_DIR}/usb_uhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci \
    -c drivers/usb/host/ohci.c -o "${BUILD_DIR}/usb_ohci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci \
    -c drivers/usb/host/ehci.c -o "${BUILD_DIR}/usb_ehci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci \
    -c drivers/usb/host/xhci.c -o "${BUILD_DIR}/usb_xhci.o"
  compile_c -I. -Idrivers/io -Idrivers/pci \
    -c drivers/usb/host/host.c -o "${BUILD_DIR}/usb_host.o"

  echo "[x64] Compiling Phase 3d USB enumeration + HID under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/usb \
    -c drivers/usb/core/enumeration.c -o "${BUILD_DIR}/usb_enum.o"
  compile_c -I. -Idrivers/io -Idrivers/input \
    -c drivers/usb/hid/hid.c -o "${BUILD_DIR}/usb_hid.o"
  compile_c -I. -Idrivers/io -Idrivers/usb \
    -c drivers/usb/usb.c -o "${BUILD_DIR}/usb.o"

  # USB mass-storage class driver: header + body are freestanding (no storage
  # stack dependency on x64; the storage-stack call sites in drivers/storage/
  # storage.c are not compiled here). Compile + link it so the symbol is
  # available to a future drivers/storage/storage.c port without churn,
  # and so the link table mirrors the x86 path. --gc-sections drops it
  # if no x64 caller materializes.
  echo "[x64] Compiling Phase 3d USB MSC stub (no storage stack) under -m64..."
  compile_c -I. -Idrivers/io \
    -c drivers/usb/storage/msc.c -o "${BUILD_DIR}/usb_msc.o"

  # ---- Phase 3e: filesystem, shell, GUI ------------------------------------
  # The in-memory filesystem is the same fs_init / fs_open / fs_create /
  # fs_read / fs_write stack the x86 build uses. The desktop calls
  # fs_get_desktop_dir() during init so this must compile clean before the
  # shell or desktop translation units.
  echo "[x64] Compiling Phase 3e fs/vfs.c + memfs.c + fat32.c under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Ilib -Ifs \
    -c fs/vfs.c -o "${BUILD_DIR}/vfs.o"
  compile_c -I. -Idrivers/io -Ilib -Ifs \
    -c fs/memfs.c -o "${BUILD_DIR}/memfs.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Ilib -Ifs \
    -c fs/fat32.c -o "${BUILD_DIR}/fat32.o"

  echo "[x64] Compiling install payload + FAT32 deploy module under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib -Ifs \
    -c install/install_payload.c -o "${BUILD_DIR}/install_payload.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/iso9660.c -o "${BUILD_DIR}/iso9660.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/grub_bios_embed.c -o "${BUILD_DIR}/install_grub_bios_embed.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/gpt_embed.c -o "${BUILD_DIR}/install_gpt_embed.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/fat32_mkfs.c -o "${BUILD_DIR}/install_fat32_mkfs.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/install_debug.c -o "${BUILD_DIR}/install_debug.o"
  compile_c -I. -Idrivers/io -Idrivers/pci -Idrivers/storage -Iinstall -Ilib \
    -c install/install.c -o "${BUILD_DIR}/install.o"

  # The command shell parser.
  echo "[x64] Compiling Phase 3e shell/shell.c (deferred-stub launchers) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard \
    -Idrivers/timer -Igui -Ifs -Iinstall -Ilib \
    -c shell/shell.c -o "${BUILD_DIR}/shell.o"

  # gui/window.c is the legacy text-mode window manager invoked by the
  # shell's `gui` command. It is x86-text-mode-only in practice but compiles
  # clean under -m64 once gated. We link it so the shell command surface
  # stays identical between x86 and x64.
  echo "[x64] Compiling Phase 3e gui/window.c (text-mode window primitive) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard \
    -Idrivers/timer -Idrivers/input -Ifs -Ishell -Ilib \
    -c gui/window.c -o "${BUILD_DIR}/window.o"

  # The VESA-rendered window decoration code, dirty-region tracker, and
  # per-window backbuffer used by the desktop.
  echo "[x64] Compiling Phase 3e gui/vesa_window.c (VESA decorations + dirty rects) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard \
    -Idrivers/mouse -Idrivers/input -Idrivers/timer -Idrivers/usb/host \
    -Ifs -Ilib \
    -c gui/vesa_window.c -o "${BUILD_DIR}/vesa_window.o"

  # The full VESA desktop window manager. Phase 3f: taskmgr/process.c is
  # now linked into this build so the Task Manager window backs onto the
  # real registry; the kernel_x64.c stub block is gone.
  echo "[x64] Compiling Phase 3e gui/desktop_vesa.c (desktop WM + apps) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard \
    -Idrivers/mouse -Idrivers/input -Idrivers/timer -Idrivers/usb/host \
    -Ifs -Ishell -Itaskmgr -Ilib \
    -c gui/desktop_vesa.c -o "${BUILD_DIR}/desktop_vesa.o"

  # Phase 4 (display polish, item 5): VGA-text application passthrough shim.
  # Owns an 80x25 virtual cell grid + an 8x16 font blit routine. Lets
  # editor / games (which historically wrote to 0xB8000) render into a
  # VESA window on x64 / UEFI where the legacy text plane is dead.
  echo "[x64] Compiling Phase 4 gui/vga_passthrough.c (VGA-text shim) under -m64..."
  compile_c -I. -Idrivers/video -Ilib \
    -c gui/vga_passthrough.c -o "${BUILD_DIR}/vga_passthrough.o"

  # ---- Phase 3f: storage stack, editor, taskmgr, games -------------------
  #
  # drivers/storage/{storage,sdhci}.c are pointer-clean under -m64 (BARs
  # are uint32_t for sub-4 GiB Bay-Trail-class hardware; MMIO derefs go
  # through (volatile uint*)(uintptr_t)(base + offset) so the cast is
  # already widened).
  #
  # drivers/storage/bios_disk.c is GATED under #ifdef __i386__ -- the
  # body is BIOS int 13h placeholder code that fundamentally needs a
  # real-mode thunk. drivers/storage/bios_int13.s is real-mode opcodes
  # and is NOT compiled here.
  #
  # editor/editor.c is a VGA-text editor; on UEFI long mode it links and
  # runs but only its serial mirror is visible. The visible editor on
  # x64 is the desktop's open_editor_window() VESA app.
  #
  # taskmgr/{taskmgr,process}.c provide the real process_table.
  # gui/desktop_vesa.c picks up the same registry the Task Manager
  # paints to VGA text.
  #
  # games/{snake,cubeDip,pong,doom}.c are framebuffer-direct rendering
  # plus PS/2 + USB-HID input. Each game's .c reaches the LFB through
  # vga_put_char_at; on UEFI x64 vga.c writes to a 0xB8000 region that
  # is silently dropped (no panel side-effect), but the game logic +
  # serial mirror still run cleanly.
  echo "[x64] Compiling Phase 3f drivers/storage/storage.c (controller scan) under -m64..."
  compile_c -I. -Idrivers/io -c drivers/storage/storage.c -o "${BUILD_DIR}/storage.o"
  echo "[x64] Compiling Phase 3f drivers/storage/sdhci.c (SDHCI/eMMC bring-up) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/sdhci.c -o "${BUILD_DIR}/sdhci.o"
  echo "[x64] Compiling Phase 3f drivers/storage/ahci.c (AHCI/SATA block I/O) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/ahci.c -o "${BUILD_DIR}/ahci.o"
  echo "[x64] Compiling Phase 3f drivers/storage/partition.c under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/pci -c drivers/storage/partition.c -o "${BUILD_DIR}/partition.o"
  # bios_disk.c is intentionally compiled in: the body is fully gated
  # under #ifdef __i386__, the resulting .o is empty for x64, and
  # --gc-sections drops it from the link. Keeps the build rule symmetry
  # with x86 so a future un-gating drops in cleanly.
  echo "[x64] Compiling Phase 3f drivers/storage/bios_disk.c (gated empty under __x86_64__) under -m64..."
  compile_c -I. -Idrivers/io -c drivers/storage/bios_disk.c -o "${BUILD_DIR}/bios_disk.o"

  echo "[x64] Compiling Phase 3f editor/editor.c (VGA-text editor) under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -Ifs -c editor/editor.c -o "${BUILD_DIR}/editor.o"

  echo "[x64] Compiling Phase 3f taskmgr/{taskmgr,process}.c under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -Itaskmgr -c taskmgr/taskmgr.c -o "${BUILD_DIR}/taskmgr.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Itaskmgr \
    -c taskmgr/process.c -o "${BUILD_DIR}/process.o"

  echo "[x64] Compiling Phase 3f games/{snake,cubeDip,pong,doom}.c under -m64..."
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -c games/snake.c -o "${BUILD_DIR}/snake.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -c games/cubeDip.c -o "${BUILD_DIR}/cubeDip.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -c games/pong.c -o "${BUILD_DIR}/pong.o"
  compile_c -I. -Idrivers/io -Idrivers/video -Idrivers/keyboard -Idrivers/timer \
    -c games/doom.c -o "${BUILD_DIR}/doom.o"

  echo "[x64] Linking ELF64 (linker64.ld)..."
  ${LD} -m elf_x86_64 -T linker64.ld --gc-sections -nostdlib -z noexecstack \
    -o "${BUILD_DIR}/kernel.bin" \
      "${BUILD_DIR}/boot64.o" \
      "${BUILD_DIR}/gdt64.o" \
      "${BUILD_DIR}/cpu_exceptions64.o" \
      "${BUILD_DIR}/isr32_stub64.o" \
      "${BUILD_DIR}/irq1_wrapper64.o" \
      "${BUILD_DIR}/irq12_wrapper64.o" \
      "${BUILD_DIR}/setjmp64.o" \
      "${BUILD_DIR}/idt_load64.o" \
      "${BUILD_DIR}/kernel.o" \
      "${BUILD_DIR}/kernel_x64.o" \
      "${BUILD_DIR}/string.o" \
      "${BUILD_DIR}/memory.o" \
      "${BUILD_DIR}/timer.o" \
      "${BUILD_DIR}/boot_safety.o" \
      "${BUILD_DIR}/vga.o" \
      "${BUILD_DIR}/driver_log.o" \
      "${BUILD_DIR}/edid.o" \
      "${BUILD_DIR}/connector.o" \
      "${BUILD_DIR}/fb_cache.o" \
      "${BUILD_DIR}/display.o" \
      "${BUILD_DIR}/bios_vbe.o" \
      "${BUILD_DIR}/basic_display.o" \
      "${BUILD_DIR}/native_fb.o" \
      "${BUILD_DIR}/vesa.o" \
      "${BUILD_DIR}/intel_gfx.o" \
      "${BUILD_DIR}/textcon.o" \
      "${BUILD_DIR}/input.o" \
      "${BUILD_DIR}/touchpad.o" \
      "${BUILD_DIR}/acpi.o" \
      "${BUILD_DIR}/i2c_designware.o" \
      "${BUILD_DIR}/i2c_hid.o" \
      "${BUILD_DIR}/keyboard.o" \
      "${BUILD_DIR}/mouse.o" \
      "${BUILD_DIR}/pci.o" \
      "${BUILD_DIR}/iosf_mbi.o" \
      "${BUILD_DIR}/usb_uhci.o" \
      "${BUILD_DIR}/usb_ohci.o" \
      "${BUILD_DIR}/usb_ehci.o" \
      "${BUILD_DIR}/usb_xhci.o" \
      "${BUILD_DIR}/usb_host.o" \
      "${BUILD_DIR}/usb_enum.o" \
      "${BUILD_DIR}/usb_hid.o" \
      "${BUILD_DIR}/usb.o" \
      "${BUILD_DIR}/usb_msc.o" \
      "${BUILD_DIR}/vfs.o" \
      "${BUILD_DIR}/memfs.o" \
      "${BUILD_DIR}/fat32.o" \
      "${BUILD_DIR}/install_payload.o" \
      "${BUILD_DIR}/iso9660.o" \
      "${BUILD_DIR}/install_grub_bios_embed.o" \
      "${BUILD_DIR}/install_gpt_embed.o" \
      "${BUILD_DIR}/install_fat32_mkfs.o" \
      "${BUILD_DIR}/install_debug.o" \
      "${BUILD_DIR}/install.o" \
      "${BUILD_DIR}/shell.o" \
      "${BUILD_DIR}/window.o" \
      "${BUILD_DIR}/vesa_window.o" \
      "${BUILD_DIR}/desktop_vesa.o" \
      "${BUILD_DIR}/vga_passthrough.o" \
      "${BUILD_DIR}/storage.o" \
      "${BUILD_DIR}/sdhci.o" \
      "${BUILD_DIR}/ahci.o" \
      "${BUILD_DIR}/partition.o" \
      "${BUILD_DIR}/bios_disk.o" \
      "${BUILD_DIR}/editor.o" \
      "${BUILD_DIR}/taskmgr.o" \
      "${BUILD_DIR}/process.o" \
      "${BUILD_DIR}/snake.o" \
      "${BUILD_DIR}/cubeDip.o" \
      "${BUILD_DIR}/pong.o" \
      "${BUILD_DIR}/doom.o" \
      ${osimage_obj} \
      ${install_objs}

  echo "[+] x64 Kernel built: ${BUILD_DIR}/kernel.bin"
  return 0
}

# ---- Top-level dispatch -----------------------------------------------------

command="${1:-build}"

case "${command}" in
  build)
    if [ "$#" -gt 0 ]; then shift; fi
    if [ "$#" -ne 0 ]; then usage; exit 1; fi
    build_image
    ;;
  list-devices)
    list_host_devices
    ;;
  install)
    shift
    build_image
    install_to_device_with_target "x86_64-efi" "$@"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
