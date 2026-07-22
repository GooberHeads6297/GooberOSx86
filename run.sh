#!/bin/bash

# Prefer xHCI + usb-mouse for USB HID verification (matches Lenovo 80M4 path).
# Fallback OHCI line kept for older QEMU / companion testing.
#
# Success looks like: "USB HID pointer ready." (not merely PS/2 fallback).

set -e
ISO="${1:-GooberOSx86-x64.iso}"
ARCH_QEMU=qemu-system-x86_64
if [[ "$ISO" == *x86* ]] && [[ "$ISO" != *x64* ]]; then
  ARCH_QEMU=qemu-system-i386
fi

exec "$ARCH_QEMU" -cdrom "$ISO" -m 512 \
  -machine q35 \
  -device nec-usb-xhci,id=xhci \
  -device usb-mouse,bus=xhci.0 \
  "$@"
