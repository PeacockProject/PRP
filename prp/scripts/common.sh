#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

die() {
  echo "error: $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

load_config() {
  local cfg="$1"
  [[ -f "$cfg" ]] || die "config not found: $cfg"
  # shellcheck disable=SC1090
  source "$cfg"

  TARGET_NAME="${TARGET_NAME:-unknown}"
  BOARD_NAME="${BOARD_NAME:-$TARGET_NAME}"
  CMDLINE="${CMDLINE:-}"
  BASE="${BASE:-0x80200000}"
  KERNEL_OFFSET="${KERNEL_OFFSET:-0x00008000}"
  RAMDISK_OFFSET="${RAMDISK_OFFSET:-0x02000000}"
  SECOND_OFFSET="${SECOND_OFFSET:-0x00f00000}"
  TAGS_OFFSET="${TAGS_OFFSET:-0x00000100}"
  PAGESIZE="${PAGESIZE:-2048}"
  HEADER_VERSION="${HEADER_VERSION:-0}"
  KERNEL_IMAGE="${KERNEL_IMAGE:-}"
  DTB_IMAGE="${DTB_IMAGE:-}"
  RAMDISK_PREBUILT="${RAMDISK_PREBUILT:-}"
  INITRAMFS_ROOTFS="${INITRAMFS_ROOTFS:-initramfs/rootfs}"
  ROOTFS_IMAGE="${ROOTFS_IMAGE:-$HOME/.local/var/peacock/samsung-jflte.img}"
  ROOTFS_OFFSET_SECTORS="${ROOTFS_OFFSET_SECTORS:-1050624}"
  ROOTFS_SIZE_SECTORS="${ROOTFS_SIZE_SECTORS:-6811648}"
  BUSYBOX_STATIC="${BUSYBOX_STATIC:-$HOME/.local/var/peacock/busybox-cache/busybox-1.36.1-1-armv7h.pkg.tar.gz/busybox}"
  KERNEL_CONFIG="${KERNEL_CONFIG:-$HOME/.local/var/peacock/build-chroot/x86_64/build/linux-samsung-jflte-3.4.113.8-armv7/.config}"
  ADB_PREFIX="${ADB_PREFIX:-adb}"
  RECOVERY_BLOCK="${RECOVERY_BLOCK:-/dev/block/mmcblk0p21}"
}

resolve_kernel_image() {
  if [[ -n "${KERNEL_IMAGE:-}" && -f "$KERNEL_IMAGE" ]]; then
    echo "$KERNEL_IMAGE"
    return 0
  fi

  local candidates=(
    "$HOME/.local/var/peacock/build-chroot/x86_64/build/linux-samsung-jflte-3.4.113.8-armv7/arch/arm/boot/zImage"
    "$HOME/.local/var/peacock/build-chroot/x86_64/build/linux-samsung-jflte-3.4.113.8-armv7/zImage"
    "$HOME/.local/var/peacock/boot-p1.img"
  )
  local c
  for c in "${candidates[@]}"; do
    if [[ -f "$c" ]]; then
      echo "$c"
      return 0
    fi
  done

  die "could not resolve kernel image; set KERNEL_IMAGE in config"
}

resolve_ramdisk_image() {
  local out_dir="$1"
  if [[ -n "${RAMDISK_PREBUILT:-}" && -f "$RAMDISK_PREBUILT" ]]; then
    echo "$RAMDISK_PREBUILT"
    return 0
  fi

  local generated="$out_dir/initramfs.cpio.gz"
  local generated_lzma="$out_dir/initramfs.cpio.lzma"
  if [[ -f "$generated_lzma" ]]; then
    echo "$generated_lzma"
    return 0
  fi

  if [[ -f "$generated" ]]; then
    echo "$generated"
    return 0
  fi

  die "could not resolve ramdisk image; run 'make initramfs' or set RAMDISK_PREBUILT"
}
