#!/usr/bin/env bash
# x86 boot smoke gate: build (optional) + QEMU serial greps for heap/desktop markers.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ISO="${VERIFY_X86_ISO:-$ROOT/GooberOSx86.iso}"
RUN_SELFTEST="${VERIFY_X86_SELFTEST:-0}"
QEMU_TIMEOUT="${VERIFY_X86_TIMEOUT:-45}"

if [[ "${VERIFY_X86_BUILD:-1}" == "1" ]]; then
  echo "== Build x86 ISO =="
  ./build.sh x86 build
fi

if [[ ! -f "$ISO" ]]; then
  echo "missing ISO: $ISO" >&2
  exit 1
fi

echo
echo "== QEMU x86 boot smoke (serial capture) =="
CAP="$(mktemp)"
PIDFILE="$(mktemp)"
qemu-system-i386 \
  -cdrom "$ISO" -m 512 -machine q35 \
  -display none -serial "file:$CAP" -no-reboot \
  -pidfile "$PIDFILE" -daemonize
sleep "$QEMU_TIMEOUT"
if [[ -f "$PIDFILE" ]]; then kill "$(cat "$PIDFILE")" 2>/dev/null || true; fi
rm -f "$PIDFILE"
sleep 1

fail=0
check() {
  local label="$1" pattern="$2"
  if grep -qE "$pattern" "$CAP"; then
    echo "OK: $label"
  else
    echo "FAIL: $label (expected /$pattern/)" >&2
    fail=1
  fi
}

check "16 MiB free-list heap" '\[heap\] init: size=16777216'
check "VESA graphics boot" 'Boot: VESA graphics mode'
check "Userspace stage" '\[boot\] stage: Userspace'
check "Shell / desktop stage" '\[boot\] stage: Shell / desktop'

if [[ "$RUN_SELFTEST" == "1" ]]; then
  check "GooberDOS arena probe" 'GooberDOS arena kmalloc\(8 MiB\) OK'
fi

rm -f "$CAP"

if [[ "$fail" -ne 0 ]]; then
  echo "verify-x86-smoke: FAILED" >&2
  exit 1
fi

echo
echo "verify-x86-smoke: OK"
echo "Manual GooberDOS check: boot ISO, run 'rundos' or open the GooberDOS desktop icon."
echo "  Expect HELLO.COM / VER.COM under Dos/Apps on the live memfs image."
echo "  Optional: VERIFY_X86_SELFTEST=1 with gooberos.selftest=1 in grub.cfg for arena probe."
