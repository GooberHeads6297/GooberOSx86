#!/bin/bash
# -----------------------------------------------------------------------------
# build.sh
#
# Top-level cross-compile dispatcher for GooberOSx86.
#
# Selects an architecture (x86 / x64 / both) and delegates to the per-arch
# builder under scripts/. Backward-compatible: invocations that worked with
# the historical single-file build.sh continue to do exactly what they did,
# because the default architecture is x86.
#
# Usage:
#   ./build.sh                                Build x86 kernel + hybrid ISO
#   ./build.sh build                          Same (back-compat)
#   ./build.sh x86  [build|list-devices|install ...]
#   ./build.sh x64  [build|list-devices|install ...]
#   ./build.sh both [build]                   Build BOTH x86 and x64 in sequence
#   ./build.sh list-devices                   List installable block devices
#   ./build.sh install --device ... --mount ...
#                                             Install (x86; use x64 install for UEFI)
#
# Notes:
#   * Both arch scripts produce HYBRID BIOS + UEFI ISOs. On a UEFI machine the
#     same ISO boots via grub-efi-amd64 and exposes the GOP framebuffer via
#     multiboot2.
#   * x86 and x64 are both full-featured builds (desktop, GooberC, GooberDOS).
#     x64 adds long-mode + PAT WC; x86 keeps BIOS VBE real-mode modeset.
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"
cd "${REPO_ROOT}"

X86="${SCRIPT_DIR}/scripts/build-x86.sh"
X64="${SCRIPT_DIR}/scripts/build-x64.sh"

usage() {
  cat <<'EOF'
Usage:
  ./build.sh                            Build x86 (default; backward-compatible)
  ./build.sh build                      Same as above

  ./build.sh x86  [SUBCMD ...]          x86 build / install / list-devices
  ./build.sh x64  [SUBCMD ...]          x64 build / install / list-devices
  ./build.sh both [build]               Build BOTH x86 and x64

  ./build.sh list-devices               Print installable host block devices
  ./build.sh install --device /dev/sdX --mount /mnt/goober
                                        Install the x86 ISO to a target device
                                        (use './build.sh x64 install ...' for UEFI)

Architectures:
  x86  -- 32-bit i686 kernel, hybrid BIOS + UEFI ISO (the current path).
  x64  -- 64-bit long-mode kernel, hybrid BIOS + UEFI ISO (migration target;
          source-side phases 1-4 of the plan are pending).
EOF
}

run_x86() { "${X86}" "$@"; }
run_x64() { "${X64}" "$@"; }

cmd="${1:-build}"

case "${cmd}" in
  # Architecture selection shortcuts.
  x86)
    shift
    run_x86 "${@:-build}"
    ;;
  x64)
    shift
    run_x64 "${@:-build}"
    ;;
  both)
    shift
    sub="${1:-build}"
    if [ "${sub}" != "build" ]; then
      echo "'both' only supports the 'build' subcommand"
      usage
      exit 1
    fi
    echo "==> [both] x86"
    run_x86 build
    echo "==> [both] x64"
    run_x64 build
    ;;

  # Back-compat: bare subcommands target the default architecture (x86).
  build)
    shift || true
    if [ "$#" -ne 0 ]; then
      usage; exit 1
    fi
    run_x86 build
    ;;
  list-devices)
    run_x86 list-devices
    ;;
  install)
    run_x86 "install" "${@:2}"
    ;;

  help|-h|--help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
