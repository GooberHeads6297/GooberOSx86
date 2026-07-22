#!/bin/bash

# OHCI provides correct BAR assignment in QEMU 10.2. The current USB HID stack
# supports boot-protocol relative mice, so use usb-mouse instead of usb-tablet.
qemu-system-i386 -cdrom GooberOSx86.iso -device pci-ohci,id=ohci -device usb-mouse,bus=ohci.0
