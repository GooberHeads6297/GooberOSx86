#!/bin/bash
# Verify BIOS-installed GooberOS FAT32 layout and emit NDJSON debug logs.
# Usage: scripts/verify-installed-disk.sh <disk-image-or-block-dev> [grub_cfg_size]

set -euo pipefail

LOG_PATH="/home/gooberheads/CodeEnv/GooberOSx86/.cursor/debug-c77991.log"
SESSION_ID="c77991"
RUN_ID="${VERIFY_RUN_ID:-pre-fix}"
DISK="${1:-}"
GRUB_CFG_SIZE="${2:-0}"
PART_START=2048

if [ -z "${DISK}" ] || [ ! -e "${DISK}" ]; then
  echo "Usage: $0 <disk-image-or-block-dev> [grub_cfg_size]" >&2
  exit 1
fi

emit() {
  local hyp="$1" msg="$2" a="$3" b="$4" c="$5"
  local ts
  ts=$(date +%s%3N)
  printf '{"sessionId":"%s","runId":"%s","hypothesisId":"%s","location":"verify-installed-disk.sh","message":"%s","data":{"a":%s,"b":%s,"c":%s},"timestamp":%s}\n' \
    "${SESSION_ID}" "${RUN_ID}" "${hyp}" "${msg}" "${a}" "${b}" "${c}" "${ts}" >> "${LOG_PATH}"
}

read_sector() {
  local lba="$1"
  dd if="${DISK}" bs=512 skip="${lba}" count=1 status=none 2>/dev/null
}

# H3: check build payload core.img for embedded early config strings
CORE_IMG="/home/gooberheads/CodeEnv/GooberOSx86/build/install/core.img"
if [ ! -r "${CORE_IMG}" ]; then
  CORE_IMG="/home/gooberheads/CodeEnv/GooberOSx86/build64/install/core.img"
fi
if [ -r "${CORE_IMG}" ]; then
  if strings "${CORE_IMG}" | grep -q 'search.*GOOBEROS\|search\.fs_label\|configfile'; then
    emit "H3" "core_img_has_early_config" 1 0 0
  else
    emit "H3" "core_img_missing_early_config" 0 0 0
  fi
else
  emit "H3" "core_img_not_found" 0 0 0
fi

mbr=$(read_sector 0)
core0=$(read_sector 1)
bpb=$(read_sector "${PART_START}")

sig_hex=$(echo -n "${mbr}" | dd bs=1 skip=510 count=2 status=none 2>/dev/null | xxd -p | tr -d '\n')
sig=$((16#${sig_hex:-0}))

kern_lba_hex=$(echo -n "${mbr}" | dd bs=1 skip=92 count=4 status=none 2>/dev/null | xxd -p | tr -d '\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
kern_lba=$((16#${kern_lba_hex:-0}))

part_type=$(echo -n "${mbr}" | dd bs=1 skip=450 count=1 status=none 2>/dev/null | xxd -p)
part_lba_hex=$(echo -n "${mbr}" | dd bs=1 skip=454 count=4 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
part_lba=$((16#${part_lba_hex:-0}))

emit "H4" "mbr_sig_and_kern_lba" "${sig}" "0x${part_type}" "${kern_lba}"

bl_start_hex=$(echo -n "${core0}" | dd bs=1 skip=500 count=4 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
bl_len_hex=$(echo -n "${core0}" | dd bs=1 skip=508 count=2 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)/\2\1/')
bl_seg_hex=$(echo -n "${core0}" | dd bs=1 skip=510 count=2 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)/\2\1/')
bl_start=$((16#${bl_start_hex:-0}))
bl_len=$((16#${bl_len_hex:-0}))
bl_seg=$((16#${bl_seg_hex:-0}))
emit "H4" "core_blocklist" "${bl_start}" "${bl_len}" "${bl_seg}"

hidden_hex=$(echo -n "${bpb}" | dd bs=1 skip=28 count=4 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
hidden=$((16#${hidden_hex:-0}))
spc=$(echo -n "${bpb}" | dd bs=1 skip=13 count=1 status=none 2>/dev/null | xxd -p)
spc=$((16#${spc:-0}))
emit "H2" "bpb_hidden_sectors" "${hidden}" "${spc}" "${PART_START}"

label=$(echo -n "${bpb}" | dd bs=1 skip=71 count=11 status=none 2>/dev/null | tr -d '\0' | tr -d ' ')
label_ok=0
[ "${label}" = "GOOBEROS" ] && label_ok=1
emit "H5" "volume_label_ok" "${label_ok}" "0" "0"

reserved=$((16#$(echo -n "${bpb}" | dd bs=1 skip=14 count=2 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)/\2\1/')))
fats=$((16#$(echo -n "${bpb}" | dd bs=1 skip=16 count=1 status=none 2>/dev/null | xxd -p)))
fat_sz=$((16#$(echo -n "${bpb}" | dd bs=1 skip=36 count=4 status=none 2>/dev/null | xxd -p | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')))
data_rel=$((reserved + fats * fat_sz))
root_lba=$((PART_START + data_rel))
root=$(read_sector "${root_lba}")

found_boot=0
found_grub=0
found_grubcfg=0
for off in $(seq 0 32 480); do
  entry=$(echo -n "${root}" | dd bs=1 skip="${off}" count=32 status=none 2>/dev/null)
  first=$(echo -n "${entry}" | dd bs=1 count=1 status=none 2>/dev/null | xxd -p)
  [ "${first}" = "00" ] && break
  [ "${first}" = "e5" ] && continue
  name=$(echo -n "${entry}" | dd bs=1 count=11 status=none 2>/dev/null)
  case "${name}" in
    "BOOT        ") found_boot=1 ;;
    "GRUB        ") found_grub=1 ;;
    "GRUB    CFG ") found_grubcfg=1 ;;
  esac
done
emit "H1" "root_dir_boot_grub" "${found_boot}" "${found_grub}" "${GRUB_CFG_SIZE}"
emit "H1" "root_dir_grub_cfg_entry" "${found_grubcfg}" "${root_lba}" "0"

emit "H3" "mbr_part_entry" "0x${part_type}" "${part_lba}" "${PART_START}"

echo "[verify] Wrote debug logs to ${LOG_PATH}"
echo "[verify] H1 boot=${found_boot} grub=${found_grub} grub.cfg_entry=${found_grubcfg}"
echo "[verify] H2 hidden_sectors=${hidden} (expect ${PART_START})"
echo "[verify] H4 blocklist start=${bl_start} len=${bl_len} seg=0x$(printf '%x' "${bl_seg}") (expect 0x820)"
echo "[verify] H5 label='${label}' ok=${label_ok}"
