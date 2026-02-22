# PRP (Peacock Recovery Project)

Standalone recovery build system, intentionally independent from Soong/manifests and the Peacock package graph.

## Goals
- lk2nd-like local workflow (`make`, shell scripts)
- deterministic artifacts in `prp/out/<target>`
- no direct dependency on Android build system

## Layout
- `configs/*.env`: device-specific boot image parameters and flash method
- `scripts/build-initramfs.sh`: packs `initramfs/rootfs` into `initramfs.cpio.gz`
- `scripts/build-bootimg.sh`: builds Android boot/recovery image with `mkbootimg`
- `scripts/backup-recovery.sh`: dumps current recovery partition
- `scripts/flash-recovery.sh`: flashes built image and verifies byte-for-byte

## Quick Start
```bash
cd prp
make sync-assets TARGET=jflte
make check-kernel TARGET=jflte
make initramfs TARGET=jflte
make bootimg TARGET=jflte
make backup-recovery TARGET=jflte
make flash-recovery TARGET=jflte
```

For Xiaomi Mi A2 Lite:
```bash
cd prp
make initramfs TARGET=xiaomi-daisy
make bootimg TARGET=xiaomi-daisy
make flash-recovery TARGET=xiaomi-daisy
```

For A/B devices using lk2nd split-boot layout, set these in the device config:
- `FASTBOOT_AB_LK2ND_SPLIT_BOOT=1`
- `FASTBOOT_BOOT_PARTITION=boot`
- `FASTBOOT_LK2ND_PARTITION=lk2nd`

In this mode `flash-recovery` requires lk2nd fastboot (must expose `partition-size:lk2nd`).

Headless debug boot (skip display/UI, bring up RNDIS shell early):
```bash
cd prp
make initramfs TARGET=xiaomi-daisy DEBUG_BOOT=1
make bootimg TARGET=xiaomi-daisy DEBUG_BOOT=1
```

## PRP SSH/SCP
- Start SSH from the PRP GUI (`Start SSH`) or run `/usr/bin/prp-svc-ssh` on device.
- For file transfer from host, use:
```bash
prp/scripts/prp-scp.sh ./local.file /tmp/remote.file
prp/scripts/prp-scp.sh root@172.16.42.1:/tmp/remote.file ./local.file
```
- The wrapper forces legacy SCP mode (`-O`), which is required when PRP uses Dropbear.

## Notes
- This flow builds a unique PRP ramdisk from `initramfs/rootfs`, not Peacock's initramfs.
- `make sync-assets` pulls:
  - `adbd` runtime from connected TWRP (`/sbin/adbd` and required bionic pieces)
  - partition-management tools (`dmsetup`, `partx`, `e2fs*`, etc.) from the local jflte rootfs image.
- `make check-kernel` validates key options for nested subpartition handling:
  - `CONFIG_EFI_PARTITION`, `CONFIG_BLK_DEV_LOOP`, `CONFIG_BLK_DEV_DM`, `CONFIG_EXT4_FS`.
