# GooberOSx86
Simple OS with a Kernel and a Shell (W.I.P)

<img width="397" height="138" alt="imageStart" src="https://github.com/user-attachments/assets/7598db4d-5243-41c1-84ba-0f8388f031cc" />


________________________________

### Built-in Task Manager 
(At the moment not neccesary becasue the only thing runnnig is the Kernel itself.)


<img width="3840" height="2160" alt="TaskManagerUpdate" src="https://github.com/user-attachments/assets/7856bcc9-8228-492b-a3ce-ba765fc7cafb" />

________________________________

### Memory-Based Filesystem With Directory Traversal


<img width="693" height="270" alt="ImageMemfilesystem" src="https://github.com/user-attachments/assets/4e2f3cc1-fcf3-4ec6-a14c-ce4ea1cce921" />

________________________________

### Integrated Text Editor (Currenntly In Development)

<img width="835" height="354" alt="ImageEditor" src="https://github.com/user-attachments/assets/e8d2dc58-3b94-4217-b09f-59b8ca868698" />

________________________________

### Built-in Display Manager using VGA + File Explorer

<img width="3840" height="2160" alt="screenExplorer" src="https://github.com/user-attachments/assets/0c9d12ea-2870-4c57-aace-c748b706aaa0" />

________________________________

### Text Editor Integration with GUI

<img width="3840" height="2160" alt="TextEdit" src="https://github.com/user-attachments/assets/67997362-36f6-4fe4-ab9e-f5244c3c8498" />

________________________________


<img width="128" height="21" alt="Screenshot 2025-07-12 212951" src="https://github.com/user-attachments/assets/2c69725e-ff7d-45ca-b3e6-30fc4e05b50a" />

Release: 0.11.5 (sha256 only shows the one for the latest release)
802df6ae35109f057163d37c072072695bbc9177e7f033b48b491a6bce6e5180

  Install with GRUB on a storage device:

  Portable install from live ISO (recommended — like a Linux live USB):

  1. Build: `./build.sh x86` or `./build.sh x64`
  2. Boot the ISO from USB or attach as IDE optical drive.
  3. Attach an empty virtual/real HDD.
  4. In the shell:
       `install list`
       `install fat32 <target-id> YES MBR`   (BIOS/legacy)
       `install fat32 <target-id> YES GPT`   (UEFI)
  5. Reboot from the HDD (remove ISO/USB or change boot order).

  No host partitioning tools are required — `install fat32` creates the
  partition table, boot track, and filesystem on the target disk.

  Developer / host install (optional):
  1. `./build.sh x86`
  2. `./scripts/make-installed-disk.sh build/GooberOS-installed.img`
  3. Attach as IDE HDD and boot (skips the live ISO installer).

  Or install to a mounted FAT32 partition on the build host:

  1. `./build.sh list-devices`
  2. Mount the target partition (FAT32).
  3. `./build.sh install --device /dev/sdX --mount /mnt/goober`

  Other commands:

  1. Run `devices` to list detected storage hardware.
  2. Run `install info <id>` for details about one target.

  Current direct-install scope:

  1. ATA, AHCI/SATA, and eMMC (SDHCI) installs.
  2. NVMe / USB MSC install paths still need dedicated write support.

### Lenovo 80M4 pointer (USB mouse + I2C touchpad)

Target laptop: Lenovo S21e-20 (Type 80M4), Bay Trail LPSS I2C + ELAN HID-I2C
touchpad at address `0x15`, plus external USB mice through xHCI/EHCI.

**USB mouse (gray USB2 ports) — first priority:**

Boot markers to grep:
```
USB2ROUTE: unlocking UPRWC, clearing PDO, writing PRM then PR
USB2ROUTE: XUSB2PR=... LOCKED=0 (USB2 on xHCI)   # success path
USB HID pointer ready.
[usb-hid] first LEFT click confirmed.
[usb-hid] first RIGHT click confirmed.
[usb-hid] first WHEEL scroll confirmed
```
If `LOCKED=1`, USB2 stayed on EHCI; FS/LS mice need the xHCI route (classic
EHCI cannot speak FS without UHCI, which Bay Trail usually lacks).

**QEMU regression (USB mouse):**

```bash
./build.sh x64 build
./run.sh GooberOSx86-x64.iso -serial file:build/capture.txt -display none
```

Expect `USB HID pointer ready.` and later movement reports. `devices` should
show `HID pointer: yes` and `I2C touchpad: no`.

**Lenovo hardware checks (touchpad — deferred after USB mouse works):**

1. Boot the x64 ISO; confirm serial/`driverlog` lines:
   - `[touchpad] I2C HID touchpad ready`
   - `USB2ROUTE:` (Bay Trail USB2 ownership)
2. Move / click / two-finger scroll on the built-in pad.
3. Shell `devices` shows `I2C touchpad: yes ... decoder=...`.

Disable probes if needed: `gooberos.touchpad=off` or `gooberos.i2c=off`.

