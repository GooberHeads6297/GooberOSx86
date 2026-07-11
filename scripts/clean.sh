#!/bin/bash
# Remove generated build artifacts and ISO images from the repo tree.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

echo "[+] Removing build directories..."
rm -rf build build64

echo "[+] Removing ISO images..."
rm -f GooberOSx86.iso GooberOSx86-x64.iso GooberOSx86-x64-dark.iso

echo "[+] Removing staged iso trees (kernel.bin copies)..."
rm -rf iso/boot/kernel.bin iso64/boot/kernel.bin

echo "[+] Done. Rebuild with: ./build.sh x86"
