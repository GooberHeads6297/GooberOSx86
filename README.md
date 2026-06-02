# GooberOSx86 | GooberOSx64
Simple OS with a Kernel and a Shell (W.I.P)

<img width="181" height="37" alt="image" src="https://github.com/user-attachments/assets/bb6f39ea-2ec5-4dac-89eb-c1a8875393e1" />



________________________________

### Built-in Task Manager 
(At the moment not neccesary becasue the only thing running is the Kernel itself.)


<img width="957" height="489" alt="image" src="https://github.com/user-attachments/assets/03ff41db-e101-4bfc-a215-05318dfee3bf" />


________________________________

### Memory-Based Filesystem With Directory Traversal


<img width="693" height="270" alt="ImageMemfilesystem" src="https://github.com/user-attachments/assets/4e2f3cc1-fcf3-4ec6-a14c-ce4ea1cce921" />

### VGA GUI Filesystem Support 

<img width="670" height="609" alt="image" src="https://github.com/user-attachments/assets/e21c76f2-51bb-4e28-b8e0-826d1e304bb7" />


### Vesa Driver Support with File Explorer

<img width="718" height="532" alt="image" src="https://github.com/user-attachments/assets/8a1e0d9c-0e7a-417a-a9b8-19e3e2b26072" />


________________________________

### Integrated Text Editor (Interactive with Memory-Based Filesystem)

<img width="835" height="354" alt="ImageEditor" src="https://github.com/user-attachments/assets/e8d2dc58-3b94-4217-b09f-59b8ca868698" />

### VGA GUI Support For Text Editor

### Vesa Driver Support With Text Editor

<img width="937" height="564" alt="image" src="https://github.com/user-attachments/assets/19c269da-e20e-4d07-8493-f31ae9650b83" />


________________________________

### Built-in Display Manager using VGA + File Explorer

<img width="3840" height="2160" alt="screenExplorer" src="https://github.com/user-attachments/assets/0c9d12ea-2870-4c57-aace-c748b706aaa0" />

________________________________

### Device and simple Storage ennumeration for hybrid ISO installation (Currently Works on x86 release)

<img width="1380" height="676" alt="image" src="https://github.com/user-attachments/assets/3205fecb-9024-42ab-9579-6459fcf1756f" />

_________________________________

### Light and Dark Mode for VESA supported Display Manager (Both x86 and x64)

## Light Mode

<img width="1315" height="988" alt="image" src="https://github.com/user-attachments/assets/4ea13897-5f76-41c9-ac27-835f03d99591" />

## Dark Mode

<img width="1315" height="988" alt="image" src="https://github.com/user-attachments/assets/711bc0a6-9a9b-44c6-8af6-5196121cc237" />


_________________________________


<img width="128" height="21" alt="Screenshot 2025-07-12 212951" src="https://github.com/user-attachments/assets/2c69725e-ff7d-45ca-b3e6-30fc4e05b50a" />

Release: 0.11.5 (sha256 only shows the one for the latest release)
802df6ae35109f057163d37c072072695bbc9177e7f033b48b491a6bce6e5180

  Install with GRUB on a storage device:

  1. Build the project: `./build.sh`
  2. Inspect host devices: `./build.sh list-devices`
  3. Mount the target partition or filesystem.
  4. Install kernel + GRUB:
     `./build.sh install --device /dev/sdX --mount /mnt/goober`
  5. Boot the selected disk in BIOS/Legacy mode.

  In-kernel storage visibility:

  1. Run `devices` to list detected storage hardware.
  2. Run `install list` to show storage targets the kernel can identify.
  3. Run `install info <id>` for details about one target.
  4. Run `install write <id> YES` to raw-write the hybrid boot image to an ATA HDD/SSD target.

  Current direct-install scope:

  1. Intended first target: BIOS/legacy VirtualBox HDD installs.
  2. The direct in-kernel write path currently targets ATA disks.
  3. NVMe/AHCI installation work still needs dedicated write support. (eMMC support now works 🥳)


      
