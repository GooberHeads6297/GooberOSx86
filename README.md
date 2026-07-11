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

Release x86_x64: 0.12.0 (sha256 only shows the one for the latest release)
sha256:638d124c6c0658bd37888fd000d4dd93fbd06578e99dfd20e7488303176ab09a

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

