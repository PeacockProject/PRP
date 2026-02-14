# PRP (Peacock Recovery Project)

Standalone recovery build system for `jflte`, intentionally independent from Soong/manifests and the Peacock package graph.

## Goals
- lk2nd-like local workflow (`make`, shell scripts)
- deterministic artifacts in `prp/out/<target>`
- no direct dependency on Android build system

## Layout
- `configs/jflte.env`: device-specific boot image parameters and flash block paths
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

## Notes
- This flow builds a unique PRP ramdisk from `initramfs/rootfs`, not Peacock's initramfs.
- `make sync-assets` pulls:
  - `adbd` runtime from connected TWRP (`/sbin/adbd` and required bionic pieces)
  - partition-management tools (`dmsetup`, `partx`, `e2fs*`, etc.) from the local jflte rootfs image.
- `make check-kernel` validates key options for nested subpartition handling:
  - `CONFIG_EFI_PARTITION`, `CONFIG_BLK_DEV_LOOP`, `CONFIG_BLK_DEV_DM`, `CONFIG_EXT4_FS`.
