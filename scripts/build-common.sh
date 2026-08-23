#!/bin/bash
# -----------------------------------------------------------------------------
# scripts/build-common.sh
#
# Shared helpers used by both scripts/build-x86.sh and scripts/build-x64.sh.
#
# Source this file; do not run it directly. The sourcing script is expected
# to define:
#   BUILD_DIR        e.g. "build"
#   ISO_DIR          e.g. "iso"
#   ISO_OUTPUT       e.g. "GooberOSx86.iso"
#   REPO_ROOT        absolute path to the repo root
# -----------------------------------------------------------------------------

# Format a byte count as a human-readable size (B/KiB/MiB/GiB/TiB).
format_bytes() {
  local bytes="$1"
  local unit=0
  local units=(B KiB MiB GiB TiB)

  while [ "${bytes}" -ge 1024 ] && [ "${unit}" -lt 4 ]; do
    bytes=$((bytes / 1024))
    unit=$((unit + 1))
  done

  printf "%s%s" "${bytes}" "${units[${unit}]}"
}

# Read a model/vendor string from a /sys/block/* device, falling back politely.
read_block_model() {
  local sysdev="$1"
  local candidate

  for candidate in \
    "${sysdev}/device/model" \
    "${sysdev}/device/name" \
    "${sysdev}/device/vendor"; do
    if [ -r "${candidate}" ]; then
      tr -s ' ' ' ' < "${candidate}" | tr -d '\n'
      return
    fi
  done

  printf "Unknown"
}

# Classify a block device as NVMe / eMMC/SD / USB / SSD / HDD.
classify_block_device() {
  local sysdev="$1"
  local name
  local path
  local rotational=0

  name="$(basename "${sysdev}")"
  path="$(readlink -f "${sysdev}/device" 2>/dev/null || true)"

  if [[ "${name}" == nvme* ]]; then
    printf "NVMe SSD"
  elif [[ "${name}" == mmcblk* ]]; then
    printf "eMMC/SD"
  elif [[ "${path}" == *"/usb"* ]]; then
    printf "USB storage"
  else
    if [ -r "${sysdev}/queue/rotational" ]; then
      rotational="$(cat "${sysdev}/queue/rotational")"
    fi
    if [ "${rotational}" = "1" ]; then
      printf "HDD"
    else
      printf "SSD"
    fi
  fi
}

# Walk /sys/block and print a human-friendly table of installable targets.
list_host_devices() {
  local sysdev
  local name
  local blocks
  local bytes
  local dtype
  local model
  local size_human

  printf "%-14s %-12s %-10s %s\n" "Device" "Type" "Size" "Model"
  for sysdev in /sys/block/*; do
    [ -e "${sysdev}" ] || continue
    name="$(basename "${sysdev}")"
    case "${name}" in
      loop*|ram*|fd*)
        continue
        ;;
    esac

    blocks="$(cat "${sysdev}/size")"
    bytes=$((blocks * 512))
    dtype="$(classify_block_device "${sysdev}")"
    model="$(read_block_model "${sysdev}")"
    size_human="$(format_bytes "${bytes}")"

    printf "%-14s %-12s %-10s %s\n" "/dev/${name}" "${dtype}" "${size_human}" "${model}"
  done
}

# Stage the install root (kernel + grub.cfg) under ${BUILD_DIR}/install-root.
prepare_install_root() {
  local install_root="${BUILD_DIR}/install-root"
  mkdir -p "${install_root}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${install_root}/boot/kernel.bin"
  cp grub/grub.cfg "${install_root}/boot/grub/grub.cfg"
  patch_grub_arch_labels "${install_root}/boot/grub/grub.cfg"
  echo "[+] Install root staged at ${install_root}"
}

# Build a HYBRID BIOS + UEFI ISO via grub-mkrescue. Used by both the x86 and
# x64 builders so that a single ISO boots on legacy BIOS *and* UEFI firmware
# (the latter via grub-efi-amd64 + GOP framebuffer -- this is the Phase 0
# de-risk step of the UEFI/GOP/x64 migration plan).
#
# Notes:
#   - We deliberately do NOT pass --directory=/usr/lib/grub/i386-pc/. With both
#     grub-pc-bin and grub-efi-amd64-bin installed, grub-mkrescue auto-emits
#     an El Torito + GPT/UEFI hybrid image bootable from either firmware.
#   - --modules is applied to ALL targets, so it can only list modules that
#     exist on both i386-pc and x86_64-efi. `biosdisk` is BIOS-only and is
#     already embedded in GRUB's BIOS core image regardless, so it does not
#     belong here.
create_iso_hybrid() {
  mkdir -p "${ISO_DIR}/boot/grub"
  cp "${BUILD_DIR}/kernel.bin" "${ISO_DIR}/boot/"
  cp grub/grub.cfg "${ISO_DIR}/boot/grub/"
  patch_grub_arch_labels "${ISO_DIR}/boot/grub/grub.cfg"
  prepare_install_root

  grub-mkrescue \
    -o "${ISO_OUTPUT}" \
    "${ISO_DIR}/" \
    --modules="part_msdos iso9660 multiboot multiboot2 all_video gfxterm font"

  echo "[+] Hybrid BIOS+UEFI ISO created: ${ISO_OUTPUT}"
}

# BIOS-safe grub.cfg for FAT32-installed media (minimal core.img, no ISO fonts).
write_installed_grub_cfg() {
  local dest="$1"
  cp grub/grub.installed.cfg "${dest}"
  patch_grub_arch_labels "${dest}"
  echo "[+] Wrote installed grub.cfg -> ${dest}"
}

# Stamp ISO/install GRUB menus with the target arch (x86 vs x64).
patch_grub_arch_labels() {
  local cfg="$1"
  local arch="${GOOBEROS_GRUB_ARCH:-x86}"
  if [ ! -f "${cfg}" ]; then
    echo "[!] grub.cfg not found at ${cfg}"
    return 1
  fi
  sed -i 's/GooberOSx86/GooberOS x86/g' "${cfg}"
  if [ "${arch}" = "x64" ]; then
    sed -i 's/GooberOS x86/GooberOS x64/g' "${cfg}"
    patch_grub_x64_normal_boot "${cfg}"
    echo "[+] grub arch labels -> x64 (${cfg})"
  else
    echo "[+] grub arch labels -> x86 (${cfg})"
  fi
}

# x64: normalize any leftover storage=sdhci cmdline to storage=ata.
patch_grub_x64_normal_boot() {
  local cfg="$1"
  if [ ! -f "${cfg}" ]; then
    echo "[!] grub.cfg not found at ${cfg}"
    return 1
  fi
  sed -i 's/gooberos\.storage=sdhci/gooberos.storage=ata/g' "${cfg}"
  echo "[+] x64 storage=ata normalize (${cfg})"
}

# Stage install payload files on the ISO (FAT template read at install time).
stage_iso_install_files() {
  local payload_dir="${BUILD_DIR}/install"
  mkdir -p "${ISO_DIR}/boot/install"
  rm -f "${ISO_DIR}/boot/install/"*
  if [ -f "${payload_dir}/fat-partition.img" ]; then
    cp "${payload_dir}/fat-partition.img" "${ISO_DIR}/boot/install/FAT_PART.IMG"
    echo "[+] Staged FAT template on ISO -> ${ISO_DIR}/boot/install/FAT_PART.IMG"
  fi
}

# Stage files embedded into the kernel for in-OS `install fat32` deployment.
# Builds BIOS (boot.img + core.img) and UEFI (BOOTX64.EFI) GRUB blobs.
# BOOTX64.EFI is copied into the FAT template at EFI/BOOT/ for eMMC/UEFI.
prepare_install_payload() {
  local payload_dir="${BUILD_DIR}/install"
  local memdisk_dir="${payload_dir}/memdisk"
  mkdir -p "${payload_dir}"
  cp "${BUILD_DIR}/kernel.bin" "${payload_dir}/kernel_payload.bin"
  write_installed_grub_cfg "${payload_dir}/grub.cfg"

  mkdir -p "${memdisk_dir}/boot/grub"
  cp grub/grub.installed.cfg "${memdisk_dir}/boot/grub/grub.cfg"
  patch_grub_arch_labels "${memdisk_dir}/boot/grub/grub.cfg"
  tar -C "${memdisk_dir}" -cf "${payload_dir}/memdisk.tar" boot

  if [ -r /usr/lib/grub/i386-pc/boot.img ] && command -v grub-mkimage >/dev/null 2>&1; then
    cp /usr/lib/grub/i386-pc/boot.img "${payload_dir}/boot.img"
    grub-mkimage -O i386-pc -o "${payload_dir}/core.img" \
      -m "${payload_dir}/memdisk.tar" -p '(memdisk)/boot/grub' \
      -c grub/grub.early.cfg \
      biosdisk part_msdos part_gpt fat search search_label configfile normal \
      multiboot multiboot2 tar memdisk minicmd cat echo linux halt reboot
    echo "[+] Prepared GRUB BIOS payload (boot.img + core.img + memdisk)"
  else
    echo "[!] grub-mkimage or i386-pc boot.img not found; install fat32 may not be BIOS-bootable"
    : > "${payload_dir}/boot.img"
    : > "${payload_dir}/core.img"
  fi

  # UEFI removable-media paths on the ESP (x64 and IA32 for Bay Trail).
  local efi_files=()
  if [ -d /usr/lib/grub/x86_64-efi ] && command -v grub-mkimage >/dev/null 2>&1; then
    grub-mkimage -O x86_64-efi -o "${payload_dir}/BOOTX64.EFI" \
      -p /boot/grub \
      -c grub/grub.efi.early.cfg \
      part_msdos part_gpt fat search search_label configfile normal \
      multiboot multiboot2 echo linux halt reboot all_video gfxterm
    efi_files+=("${payload_dir}/BOOTX64.EFI")
    echo "[+] Prepared GRUB UEFI payload (BOOTX64.EFI)"
  else
    echo "[!] x86_64-efi GRUB modules missing; eMMC install will not be UEFI-bootable"
    echo "    (install grub-efi-amd64-bin / run ./InstallDep.sh x64)"
    rm -f "${payload_dir}/BOOTX64.EFI"
  fi
  if [ -d /usr/lib/grub/i386-efi ] && command -v grub-mkimage >/dev/null 2>&1; then
    grub-mkimage -O i386-efi -o "${payload_dir}/BOOTIA32.EFI" \
      -p /boot/grub \
      -c grub/grub.efi.early.cfg \
      part_msdos part_gpt fat search search_label configfile normal \
      multiboot multiboot2 echo linux halt reboot all_video gfxterm
    efi_files+=("${payload_dir}/BOOTIA32.EFI")
    echo "[+] Prepared GRUB IA32 UEFI payload (BOOTIA32.EFI) for Bay Trail firmware"
  else
    echo "[!] i386-efi GRUB modules missing; 32-bit UEFI eMMC boot may fail"
    echo "    (install grub-efi-ia32-bin if Lenovo Setup is 32-bit UEFI)"
    rm -f "${payload_dir}/BOOTIA32.EFI"
  fi

  bash "${REPO_ROOT}/scripts/prepare-fat-template.sh" \
    "${payload_dir}" "${BUILD_DIR}/kernel.bin" "${payload_dir}/grub.cfg" \
    "${efi_files[@]+"${efi_files[@]}"}"

  echo "[+] Install payload staged at ${payload_dir}"
}

# Link GRUB install blobs. The FAT template stays on the live ISO
# (boot/install/FAT_PART.IMG) — embedding it made kernel.bin ~37 MiB and
# GRUB on slow eMMC appeared to hang on a bare cursor after menu select.
link_install_payload_objects() {
  local payload_dir="${BUILD_DIR}/install"
  ${LD} -m elf_i386 -r -b binary "${payload_dir}/kernel_payload.bin" \
    -o "${BUILD_DIR}/install_kernel_payload.o"
  ${LD} -m elf_i386 -r -b binary "${payload_dir}/grub.cfg" \
    -o "${BUILD_DIR}/install_grub_cfg.o"
  ${LD} -m elf_i386 -r -b binary "${payload_dir}/boot.img" \
    -o "${BUILD_DIR}/install_boot_img.o"
  ${LD} -m elf_i386 -r -b binary "${payload_dir}/core.img" \
    -o "${BUILD_DIR}/install_core_img.o"
  echo "[+] Linked install payload objects (i386; FAT template is ISO-only)"
}

link_install_payload_objects_x64() {
  local payload_dir="${BUILD_DIR}/install"
  ${LD} -m elf_x86_64 -r -b binary "${payload_dir}/kernel_payload.bin" \
    -o "${BUILD_DIR}/install_kernel_payload.o"
  ${LD} -m elf_x86_64 -r -b binary "${payload_dir}/grub.cfg" \
    -o "${BUILD_DIR}/install_grub_cfg.o"
  ${LD} -m elf_x86_64 -r -b binary "${payload_dir}/boot.img" \
    -o "${BUILD_DIR}/install_boot_img.o"
  ${LD} -m elf_x86_64 -r -b binary "${payload_dir}/core.img" \
    -o "${BUILD_DIR}/install_core_img.o"
  echo "[+] Linked install payload objects (x86_64; FAT template is ISO-only)"
}

# Rebuild the ESP FAT image with the *final* kernel.bin (after payload link).
refresh_fat_template_with_final_kernel() {
  local payload_dir="${BUILD_DIR}/install"
  local efi_files=()
  if [ -s "${payload_dir}/BOOTX64.EFI" ]; then
    efi_files+=("${payload_dir}/BOOTX64.EFI")
  fi
  if [ -s "${payload_dir}/BOOTIA32.EFI" ]; then
    efi_files+=("${payload_dir}/BOOTIA32.EFI")
  fi
  write_installed_grub_cfg "${payload_dir}/grub.cfg"
  bash "${REPO_ROOT}/scripts/prepare-fat-template.sh" \
    "${payload_dir}" "${BUILD_DIR}/kernel.bin" "${payload_dir}/grub.cfg" \
    "${efi_files[@]+"${efi_files[@]}"}"
  echo "[+] Refreshed FAT template with final kernel.bin"
}

# Patch multiboot lines in an installed grub.cfg to include gooberos.root=auto.
patch_installed_grub_cfg() {
  local cfg="$1"
  local tmp="${cfg}.tmp"
  if [ ! -f "${cfg}" ]; then
    echo "[!] grub.cfg not found at ${cfg}"
    return 1
  fi
  awk '
    /multiboot2 / {
      if ($0 !~ /gooberos\.root=/) {
        sub(/[ \t]*$/, "", $0)
        print $0 " gooberos.root=auto"
        next
      }
    }
    /multiboot / && $0 !~ /multiboot2/ {
      if ($0 !~ /gooberos\.root=/) {
        sub(/[ \t]*$/, "", $0)
        print $0 " gooberos.root=auto"
        next
      }
    }
    { print }
  ' "${cfg}" > "${tmp}" && mv "${tmp}" "${cfg}"
  echo "[+] Patched ${cfg} with gooberos.root=auto"
}

# Install the freshly built kernel + grub config onto a mounted target device.
# Shared between x86 and x64 builds; arch-specific grub-install target is
# passed in as $1 (e.g. "i386-pc" or "x86_64-efi").
install_to_device_with_target() {
  local grub_target="$1"; shift
  local device=""
  local mountpoint=""

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --device)
        device="${2:-}"
        shift 2
        ;;
      --mount)
        mountpoint="${2:-}"
        shift 2
        ;;
      *)
        echo "Unknown install option: $1"
        return 1
        ;;
    esac
  done

  if [ -z "${device}" ] || [ -z "${mountpoint}" ]; then
    echo "install requires --device and --mount"
    return 1
  fi

  if [ ! -b "${device}" ]; then
    echo "Target device does not exist: ${device}"
    return 1
  fi

  if [ ! -d "${mountpoint}" ]; then
    echo "Mount point does not exist: ${mountpoint}"
    return 1
  fi

  if command -v findmnt >/dev/null 2>&1; then
    local fstype
    fstype="$(findmnt -n -o FSTYPE --target "${mountpoint}" 2>/dev/null || true)"
    if [ -n "${fstype}" ] && [ "${fstype}" != "vfat" ] && [ "${fstype}" != "fat" ] && [ "${fstype}" != "msdos" ]; then
      echo "[!] Warning: mount ${mountpoint} fstype=${fstype} (expected FAT32/vfat)"
    fi
  fi

  if ! command -v grub-install >/dev/null 2>&1; then
    echo "grub-install is not available on this host."
    echo "Use ./scripts/make-installed-disk.sh or in-OS 'install fat32 <id> YES'."
    return 1
  fi

  mkdir -p "${mountpoint}/boot/grub"
  mkdir -p "${mountpoint}/Desktop" "${mountpoint}/home" "${mountpoint}/usr/bin" "${mountpoint}/tmp"

  cp "${BUILD_DIR}/kernel.bin" "${mountpoint}/boot/kernel.bin"
  write_installed_grub_cfg "${mountpoint}/boot/grub/grub.cfg"

  if command -v fatlabel >/dev/null 2>&1; then
    fatlabel "${mountpoint}" GOOBEROS 2>/dev/null || \
      echo "[!] fatlabel failed (non-fatal; set label manually if needed)"
  elif command -v mlabel >/dev/null 2>&1; then
    mlabel -i "${device}" ::GOOBEROS 2>/dev/null || \
      echo "[!] mlabel failed (non-fatal)"
  else
    echo "[!] fatlabel/mlabel not found; volume label GOOBEROS not set"
  fi

  grub-install --target="${grub_target}" --boot-directory="${mountpoint}/boot" "${device}"

  echo "[+] Installed GooberOS boot files to ${mountpoint}/boot"
  echo "[+] Created /Desktop /home /usr/bin /tmp on FAT32 root"
  echo "[+] GRUB (${grub_target}) installed to ${device}"
}
