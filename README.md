# GooberOS
Small OS with a handful of applications and its own language.  (W.I.P)

<img width="397" height="138" alt="imageStart" src="https://github.com/user-attachments/assets/7598db4d-5243-41c1-84ba-0f8388f031cc" />


________________________________

### Built-in Task Manager 
(At the moment not neccesary becasue the only thing runnnig is the Kernel itself.)


<img width="442" height="363" alt="image" src="https://github.com/user-attachments/assets/8d464279-8f1c-425a-8ca8-ceb60359c5d7" />


________________________________

### Memory-Based Filesystem With Directory Traversal


<img width="845" height="500" alt="image" src="https://github.com/user-attachments/assets/11ef35c6-1087-4d78-923b-6c7c33dcdbd5" />


________________________________

### Integrated Text Editor + GooberIDE (Currenntly In Development)

<img width="911" height="503" alt="image" src="https://github.com/user-attachments/assets/09e3c7da-e128-4e24-94d1-12397caf0456" />


________________________________

### Built-in Display Manager with display settings + themes and resolution toggle

<img width="911" height="503" alt="image" src="https://github.com/user-attachments/assets/1b0bb6a0-ab04-4016-9499-908155ff88ad" />


________________________________


### GUI-based installer for live-ISO 
(This currently works on real HDD, eMMC cards, and currently any VHD. 

<img width="911" height="503" alt="image" src="https://github.com/user-attachments/assets/e763309a-5424-4506-972f-2422e6f462ef" />

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

Mouse and Touchpad are currently stubs in the drivers since they aren't currently implemented properly

Disable probes if needed: `gooberos.touchpad=off` or `gooberos.i2c=off`.

