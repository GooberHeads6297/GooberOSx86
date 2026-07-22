#!/usr/bin/env bash
# USB stack verification gates (host unit tests + optional QEMU grep).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "== Host unit: PORTSC neutralization / context helpers =="
gcc -Wall -Wextra -Werror -o /tmp/test_xhci_port \
  tests/usb/test_xhci_port.c \
  drivers/usb/host/xhci_port.c \
  drivers/usb/host/xhci_ctx.c
/tmp/test_xhci_port

echo
echo "== Lenovo real-hardware log greps (apply to devices/driverlog) =="
cat <<'EOF'
Required after USB mouse plug on Lenovo S21e-20 80M4:
  # Gate 0 — USB2 must sit on xHCI (otherwise EP0 times out forever):
  grep -E 'XUSB2PR=0x0000000[1-9A-Fa-f]|post-reset XUSB2PR=0x0000000[1-9A-Fa-f]|USB2 on xHCI' devices/driverlog
  # Must NOT win the boot:
  grep 'XUSB2PR stayed 0\|XUSB2PR=0 —\|post-reset XUSB2PR=0x00000000\|USB2ROUTE: FAILED' devices/driverlog
  # Gate 1 — Bay Trail PHY bring-up should run by default (not skipped):
  grep 'USB2ROUTE: Bay Trail xHCI MMIO/PCI bring-up' devices/driverlog
  # Must NOT win when PHY is needed:
  grep 'Bay Trail xHCI bring-up skipped' devices/driverlog
  grep -E 'USB HID pointer ready|USB-CORE: class driver' devices/driverlog
  grep -E 'PORTSC reset-exit' devices/driverlog | head
  # After PRC ack, PED must stay 1 (never PED=0 + PLS=7 from our write):
  #   flags ... PED=1 ... (reset-exit)
  # Must NOT see after successful reset:
  #   PED raced away before Address Device
  # Disable PHY only if a board misbehaves: gooberos.usb.byt.phy=off
EOF

if [[ "${VERIFY_USB_QEMU:-0}" == "1" ]]; then
  echo
  echo "== QEMU gate (nec-usb-xhci + usb-mouse) =="
  ISO="${VERIFY_USB_ISO:-$ROOT/GooberOSx86-x64.iso}"
  if [[ ! -f "$ISO" ]]; then
    echo "missing ISO: $ISO (run ./build.sh x64 first)" >&2
    exit 1
  fi
  CAP="$(mktemp)"
  cp -f "$ISO" /tmp/goober-usb-verify.iso
  timeout 40 qemu-system-x86_64 \
    -cdrom /tmp/goober-usb-verify.iso -m 512 -machine q35 \
    -device nec-usb-xhci,id=xhci -device usb-mouse,bus=xhci.0 \
    -serial "file:$CAP" -display none -no-reboot >/dev/null 2>&1 || true
  if grep -q 'USB HID pointer ready' "$CAP" && grep -q 'USB: stack=new' "$CAP"; then
    echo "QEMU: USB HID pointer ready (stack=new) OK"
  else
    echo "QEMU: FAILED — expected USB: stack=new + USB HID pointer ready." >&2
    grep -E 'USB:|USB-CORE|USB HID|PS/2 fallback|PED raced' "$CAP" | head -40 >&2 || true
    rm -f "$CAP"
    exit 1
  fi
  rm -f "$CAP"
else
  echo
  echo "== QEMU gate (set VERIFY_USB_QEMU=1 to run) =="
  echo "  Boot with nec-usb-xhci + usb-mouse; expect:"
  echo "    USB: stack=new"
  echo "    USB HID pointer ready."
  echo "  Recoverable fallback: gooberos.usb.stack=legacy"
fi

echo
echo "verify-usb-stack: OK"
