#!/bin/bash
# -----------------------------------------------------------------------------
# InstallDep.sh
#
# GooberOSx86 developer-dependency installer.
#
# Supports cross-compiling for x86 (32-bit, BIOS) and x64 (64-bit, UEFI).
# Resumable: every step uses `apt install -y`, which is itself idempotent --
# already-installed packages are skipped silently, so re-running this script
# after a partial setup just picks up where it left off.
#
# Usage:
#   ./InstallDep.sh                Interactive: prompt for arch (defaults to both)
#   ./InstallDep.sh both           Install x86 + x64 toolchains + QEMU + OVMF
#   ./InstallDep.sh x86            Install x86 (32-bit BIOS) toolchain only
#   ./InstallDep.sh x64            Install x64 (64-bit UEFI) toolchain only
#   ./InstallDep.sh status         Report what is installed vs missing (no sudo)
#   ./InstallDep.sh check x86|x64  Report missing packages for that arch (no sudo)
#   ./InstallDep.sh --help         This help text
#
# Notes:
#   * 'common' packages (build-essential, nasm, make, xorriso, mtools, gdb,
#     grub-common) are installed for either choice -- they are required to
#     build the kernel and assemble the hybrid ISO.
#   * x86 adds:  grub-pc-bin (BIOS GRUB modules), gcc-multilib (-m32 support).
#   * x64 adds:  grub-efi-amd64-bin (UEFI GRUB modules), ovmf (UEFI firmware
#                for QEMU testing under OVMF, used by the migration plan).
#   * qemu-system-x86 covers BOTH 32-bit and 64-bit emulation and is always
#     installed.
# -----------------------------------------------------------------------------
set -euo pipefail

# ---- Package lists ----------------------------------------------------------
PKGS_COMMON=(
  build-essential
  binutils
  nasm
  make
  grub-common
  xorriso
  mtools
  qemu-system-x86
  gdb
)

PKGS_X86=(
  grub-pc-bin
  gcc-multilib
)

PKGS_X64=(
  grub-efi-amd64-bin
  grub-efi-ia32-bin
  ovmf
)

# ---- Helpers ----------------------------------------------------------------
print_help() {
  sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
}

is_installed() {
  dpkg -s "$1" >/dev/null 2>&1
}

require_dpkg() {
  if ! command -v dpkg >/dev/null 2>&1; then
    echo "Error: dpkg not found. This installer targets Debian/Ubuntu hosts." >&2
    exit 1
  fi
}

report_pkgs() {
  # $1 = label, rest = package list
  local label="$1"; shift
  local installed_count=0
  local missing=()
  local p
  for p in "$@"; do
    if is_installed "$p"; then
      installed_count=$((installed_count + 1))
    else
      missing+=("$p")
    fi
  done

  printf "  %-12s  %d/%d installed" "${label}" "${installed_count}" "$#"
  if [ "${#missing[@]}" -gt 0 ]; then
    printf "    missing: %s" "${missing[*]}"
  fi
  printf "\n"
}

apt_install() {
  # Install only the packages that are not yet installed. apt-get itself is
  # already idempotent for "-y install", but filtering ahead of time keeps
  # the output and the sudo prompt clean on rerun.
  local missing=()
  local p
  for p in "$@"; do
    is_installed "$p" || missing+=("$p")
  done

  if [ "${#missing[@]}" -eq 0 ]; then
    echo "[=] All requested packages already installed: $*"
    return 0
  fi

  echo "[+] apt install -y ${missing[*]}"
  if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    sudo apt-get install -y "${missing[@]}"
  else
    apt-get install -y "${missing[@]}"
  fi
}

apt_update_once() {
  # Refresh package lists exactly once per script invocation.
  if [ "${_APT_UPDATED:-0}" = "1" ]; then
    return
  fi
  if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    sudo apt-get update
  else
    apt-get update
  fi
  _APT_UPDATED=1
}

# ---- Sub-commands -----------------------------------------------------------
do_install() {
  local arch="$1"
  require_dpkg
  apt_update_once

  case "${arch}" in
    x86)
      echo "==> Installing common + x86 (BIOS) developer dependencies"
      apt_install "${PKGS_COMMON[@]}"
      apt_install "${PKGS_X86[@]}"
      ;;
    x64)
      echo "==> Installing common + x64 (UEFI) developer dependencies"
      apt_install "${PKGS_COMMON[@]}"
      apt_install "${PKGS_X64[@]}"
      ;;
    both)
      echo "==> Installing common + x86 + x64 developer dependencies"
      apt_install "${PKGS_COMMON[@]}"
      apt_install "${PKGS_X86[@]}"
      apt_install "${PKGS_X64[@]}"
      ;;
    *)
      echo "Unknown architecture: ${arch}" >&2
      print_help
      exit 1
      ;;
  esac

  echo
  echo "[+] Done. Resume status:"
  do_status
}

do_status() {
  require_dpkg
  echo "GooberOSx86 dependency status:"
  report_pkgs "common"    "${PKGS_COMMON[@]}"
  report_pkgs "x86 extra" "${PKGS_X86[@]}"
  report_pkgs "x64 extra" "${PKGS_X64[@]}"
  echo
  if is_installed ovmf; then
    echo "  OVMF firmware: present (try: qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE.fd)"
  else
    echo "  OVMF firmware: not installed (UEFI/QEMU testing requires it)"
  fi
}

do_check() {
  local arch="$1"
  require_dpkg
  case "${arch}" in
    x86)
      report_pkgs "common"    "${PKGS_COMMON[@]}"
      report_pkgs "x86 extra" "${PKGS_X86[@]}"
      ;;
    x64)
      report_pkgs "common"    "${PKGS_COMMON[@]}"
      report_pkgs "x64 extra" "${PKGS_X64[@]}"
      ;;
    both)
      do_status
      ;;
    *)
      echo "Unknown architecture: ${arch}" >&2
      exit 1
      ;;
  esac
}

prompt_arch() {
  echo "GooberOSx86 dependency installer"
  echo "Which build target(s) do you want to set up?"
  echo "  1) both  (x86 BIOS + x64 UEFI)   [default]"
  echo "  2) x86   (32-bit, BIOS only)"
  echo "  3) x64   (64-bit, UEFI only)"
  echo "  q) quit"
  read -r -p "Choice [1]: " choice
  case "${choice:-1}" in
    1|both) echo "both" ;;
    2|x86)  echo "x86" ;;
    3|x64)  echo "x64" ;;
    q|Q)    return 1 ;;
    *)      echo "both" ;;
  esac
  return 0
}

# ---- Top-level dispatch -----------------------------------------------------
cmd="${1:-}"

case "${cmd}" in
  ""|interactive)
    if arch="$(prompt_arch)"; then
      do_install "${arch}"
    else
      echo "Aborted."
      exit 0
    fi
    ;;
  x86|x64|both)
    do_install "${cmd}"
    ;;
  status)
    do_status
    ;;
  check)
    do_check "${2:-both}"
    ;;
  help|--help|-h)
    print_help
    ;;
  *)
    echo "Unknown command: ${cmd}" >&2
    print_help
    exit 1
    ;;
esac
