#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd readelf
require_cmd file
require_cmd mount
require_cmd umount

load_config "$CFG"
mkdir -p "$OUT_DIR"

ROOTFS_IMG_EXPANDED="${ROOTFS_IMAGE/#\~/$HOME}"
[[ -f "$ROOTFS_IMG_EXPANDED" ]] || die "rootfs image not found: $ROOTFS_IMG_EXPANDED"

VENDOR_DIR="$PRP_ROOT/vendor/$TARGET_NAME"
ROOTFS_RUNTIME_DIR="$VENDOR_DIR/rootfs-runtime"
TWRP_SBIN_DIR="$VENDOR_DIR/twrp-sbin"
mkdir -p "$ROOTFS_RUNTIME_DIR" "$TWRP_SBIN_DIR"
rm -rf "$ROOTFS_RUNTIME_DIR"/*

MNT="/tmp/prp-rootfs-sync-$TARGET_NAME"
sudo mkdir -p "$MNT"
if mountpoint -q "$MNT"; then
  sudo umount "$MNT"
fi

offset_bytes=$((ROOTFS_OFFSET_SECTORS * 512))
size_bytes=$((ROOTFS_SIZE_SECTORS * 512))
sudo mount -o ro,loop,offset="$offset_bytes",sizelimit="$size_bytes" "$ROOTFS_IMG_EXPANDED" "$MNT"

declare -A seen

strip_elf_if_possible() {
  local p="$1"
  [[ -f "$p" ]] || return 0
  if file "$p" | grep -q ELF; then
    if file "$p" | grep -q "not stripped"; then
      strip --strip-unneeded "$p" 2>/dev/null || true
    fi
  fi
}

copy_from_rootfs() {
  local rel="$1"
  local src="$MNT$rel"
  local dst="$ROOTFS_RUNTIME_DIR$rel"

  [[ -e "$src" || -L "$src" ]] || return 0
  [[ -n "${seen[$rel]:-}" ]] && return 0
  seen["$rel"]=1

  mkdir -p "$(dirname "$dst")"

  if [[ -L "$src" ]]; then
    local target
    target="$(readlink "$src")"
    ln -snf "$target" "$dst"
    if [[ "$target" == /* ]]; then
      copy_from_rootfs "$target"
    else
      local parent
      parent="$(dirname "$rel")"
      copy_from_rootfs "$parent/$target"
    fi
    return 0
  fi

  cp -a "$src" "$dst"
  strip_elf_if_possible "$dst"

  if file "$src" | grep -q ELF; then
    local interp
    interp="$(readelf -l "$src" 2>/dev/null | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p' | head -n1)"
    if [[ -n "$interp" ]]; then
      copy_from_rootfs "$interp"
    fi

    local lib
    while IFS= read -r lib; do
      [[ -n "$lib" ]] || continue
      local found=""
      local d
      for d in /lib /usr/lib /lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf; do
        if [[ -e "$MNT$d/$lib" || -L "$MNT$d/$lib" ]]; then
          copy_from_rootfs "$d/$lib"
          found="1"
          break
        fi
      done
      if [[ -z "$found" ]]; then
        echo "warning: missing dependency in rootfs for $rel: $lib" >&2
      fi
    done < <(readelf -d "$src" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)]/\1/p')
  fi
}

# Keep the ramdisk small enough for recovery partition constraints.
# Subpartition mounting uses busybox loop-offset support from initramfs/TWRP assets.
rootfs_bins=(
  /usr/bin/peacock-splash
  /usr/bin/msm-fb-refresher
  /usr/sbin/dropbear
  /usr/sbin/dropbearkey
  /usr/bin/dbclient
  /usr/bin/scp
  /sbin/dmsetup
  /sbin/partx
  /sbin/fdisk
  /sbin/blkid
)

for rel in "${rootfs_bins[@]}"; do
  copy_from_rootfs "$rel"
done

sudo umount "$MNT"

# Pull Android recovery adb runtime from currently booted TWRP.
$ADB_PREFIX devices -l >/dev/null
if $ADB_PREFIX get-state >/dev/null 2>&1; then
  rm -rf "$TWRP_SBIN_DIR"/*
else
  echo "warning: adb device unavailable, keeping existing twrp-sbin cache" >&2
fi

twrp_bins=(
  /sbin/adbd
  /sbin/linker
  /sbin/libc.so
  /sbin/libcutils.so
  /sbin/libm.so
  /sbin/libc++.so
  /sbin/libdl.so
  /sbin/liblog.so
  /sbin/libminadbd.so
)

for src in "${twrp_bins[@]}"; do
  if $ADB_PREFIX shell "[ -e $src ]" >/dev/null 2>&1; then
    $ADB_PREFIX pull "$src" "$TWRP_SBIN_DIR/$(basename "$src")" >/dev/null
    strip_elf_if_possible "$TWRP_SBIN_DIR/$(basename "$src")"
  else
    echo "warning: not found on recovery: $src" >&2
  fi
done

# When using "sudo adb", pulled files can be root-owned.
sudo chown -R "$(id -u):$(id -g)" "$TWRP_SBIN_DIR" 2>/dev/null || true

if [[ -f "$TWRP_SBIN_DIR/adbd" ]]; then
  chmod +x "$TWRP_SBIN_DIR/adbd"
fi

echo "synced assets:"
echo "  rootfs runtime: $ROOTFS_RUNTIME_DIR"
echo "  twrp sbin:      $TWRP_SBIN_DIR"
find "$VENDOR_DIR" -type f | wc -l | awk '{print "  files:          " $1}'
