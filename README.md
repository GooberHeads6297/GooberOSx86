# GooberOSx86 | GooberOSx64
Simple OS with a Kernel and a Shell (W.I.P)

<img width="181" height="37" alt="image" src="https://github.com/user-attachments/assets/bb6f39ea-2ec5-4dac-89eb-c1a8875393e1" />



________________________________

### Built-in Task Manager 

<img width="715" height="474" alt="image" src="https://github.com/user-attachments/assets/8917a1fe-123a-429d-b3de-ad280b7e5266" />

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

<img width="676" height="411" alt="image" src="https://github.com/user-attachments/assets/d7aeb5e4-41ed-4210-9a49-d9f37341e183" />

________________________________

### Built-in Display Manager using VGA + File Explorer

<img width="3840" height="2160" alt="screenExplorer" src="https://github.com/user-attachments/assets/0c9d12ea-2870-4c57-aace-c748b706aaa0" />

________________________________

### Device and simple Storage ennumeration for hybrid ISO installation (Works on Both x86 and x64)

<img width="1380" height="676" alt="image" src="https://github.com/user-attachments/assets/3205fecb-9024-42ab-9579-6459fcf1756f" />

## UEFI supported (x64)

<img width="1492" height="445" alt="image" src="https://github.com/user-attachments/assets/c8e9f50b-5aa7-42d4-a086-a5858521dbeb" />

_________________________________

### Light and Dark Mode for VESA supported Display Manager (Both x86 MBR and x64 EFI)

## Light Mode

<img width="1519" height="1140" alt="image" src="https://github.com/user-attachments/assets/4ca8c8b5-b087-4f2b-a8e0-f0360f0438ff" />

## Dark Mode

<img width="1519" height="1140" alt="image" src="https://github.com/user-attachments/assets/717ff848-d25b-4cf9-81df-8d27b0ce17b7" />

## Original Mode

<img width="1519" height="1140" alt="image" src="https://github.com/user-attachments/assets/a4346273-481f-4b3d-9410-b6f40f0d453e" />

_________________________________


<img width="128" height="21" alt="Screenshot 2025-07-12 212951" src="https://github.com/user-attachments/assets/2c69725e-ff7d-45ca-b3e6-30fc4e05b50a" />

Release x86: 0.11.7 (sha256 only shows the one for the latest release)
sha256:2310513132d9e153ef87760cf2c4fab4c9a33f37f74f8c09dae0930584daffd4

Release x86_x64: 0.11.7 (sha256 only shows the one for the latest release)
sha256:179402601724598310ee12bf65efa831cf24b83ace3a8f264d796ce3797d016d

__________________________________
 

###  How to install:

## x86 (MBR/CSM Legacy)
1. Flash with Rufus or Balena Etcher using MBR
2. Ensure Secure Boot is disabled
3. Boot off of flashed storage device

## x64 (EFI)
1. Flash using EFI
2. Ensure boot config in BIOS is set to UEFI or "EFI" boot

  Current direct-install scope:

  1. Intended first target: BIOS/legacy VirtualBox HDD installs.
  2. The direct in-kernel write path currently targets ATA disks.
  3. NVMe/AHCI installation work still needs dedicated write support. (eMMC support now works 🥳)


      
