#!/bin/bash
# Build a raw BIOS-bootable GooberOS disk image for VirtualBox/QEMU.
# Usage: scripts/make-installed-disk.sh [output-path]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${1:-${REPO_ROOT}/build/GooberOS-installed.img}"

if [ ! -f "${REPO_ROOT}/build/install/core.img" ]; then
  echo "[!] Run ./build.sh x86 first (install payload missing)."
  exit 1
fi

python3 "${SCRIPT_DIR}/make-test-installed-disk.py" "${OUTPUT}"

echo "[+] Installed disk image: ${OUTPUT}"
echo "[+] VirtualBox: create/use an HDD, attach this file as a raw image, boot from HDD."
echo "[+] QEMU: qemu-system-i386 -hda ${OUTPUT} -boot c -m 256"
