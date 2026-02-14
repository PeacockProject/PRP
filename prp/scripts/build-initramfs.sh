#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd cpio
require_cmd gzip
require_cmd xz
require_cmd find
require_cmd sed
require_cmd sha256sum

load_config "$CFG"
mkdir -p "$OUT_DIR"

ROOTFS_SRC="$PRP_ROOT/$INITRAMFS_ROOTFS"
[[ -d "$ROOTFS_SRC" ]] || die "initramfs rootfs source not found: $ROOTFS_SRC"

STAGE_DIR="$OUT_DIR/initramfs-stage"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

cp -a "$ROOTFS_SRC"/. "$STAGE_DIR"/

# Bake a build tag into /init so on-device logs can confirm the exact image.
build_tag="prp-$(date -u +%Y%m%d)-$(sha256sum "$ROOTFS_SRC/init" | awk '{print substr($1,1,8)}')"
sed -i "s/@PRP_BUILD_TAG@/${build_tag}/g" "$STAGE_DIR/init"

# Enforce a unique PRP ramdisk by default.
if [[ -n "${RAMDISK_PREBUILT:-}" ]]; then
  die "RAMDISK_PREBUILT is set in config; clear it for unique PRP ramdisk builds"
fi

VENDOR_DIR="$PRP_ROOT/vendor/$TARGET_NAME"
ROOTFS_RUNTIME_DIR="$VENDOR_DIR/rootfs-runtime"
TWRP_SBIN_DIR="$VENDOR_DIR/twrp-sbin"
[[ -d "$TWRP_SBIN_DIR" ]] || die "missing twrp adb assets: $TWRP_SBIN_DIR (run 'make sync-assets')"

# Install static busybox as the initramfs command backbone.
BB_PATH="${BUSYBOX_STATIC/#\~/$HOME}"
[[ -f "$BB_PATH" ]] || die "static busybox not found: $BB_PATH"
mkdir -p "$STAGE_DIR/sbin" "$STAGE_DIR/bin"
cp -a "$BB_PATH" "$STAGE_DIR/sbin/busybox"
chmod +x "$STAGE_DIR/sbin/busybox"
ln -snf /sbin/busybox "$STAGE_DIR/bin/sh"

# Provide a usable local shell toolbox even when adbd is unavailable.
busybox_bin_applets=(
  sh ash
  ls cat echo printf grep sed awk cut
  ps top dmesg logread
  mount umount df free
  blkid
  mountpoint
  mkdir rmdir mknod chmod chown ln mv cp rm
  uname env export id
  hexdump xxd
  vi less
  sleep kill killall
  setsid
)
for app in "${busybox_bin_applets[@]}"; do
  ln -snf /sbin/busybox "$STAGE_DIR/bin/$app"
done

busybox_sbin_applets=(
  cttyhack
  mdev
)
for app in "${busybox_sbin_applets[@]}"; do
  ln -snf /sbin/busybox "$STAGE_DIR/sbin/$app"
done

# Copy optional runtime payload from rootfs sync.
if [[ -d "$ROOTFS_RUNTIME_DIR" ]] && find "$ROOTFS_RUNTIME_DIR" -mindepth 1 -maxdepth 1 | read -r _; then
  cp -a "$ROOTFS_RUNTIME_DIR"/. "$STAGE_DIR"/
  # Keep initramfs lean enough for RECOVERY partition limits.
  # fdisk and its heavy readline/ncurses deps belong to PRP_ROOTFS overlay, not ramdisk.
  rm -f \
    "$STAGE_DIR/sbin/fdisk" \
    "$STAGE_DIR/lib/libfdisk.so"* \
    "$STAGE_DIR/lib/libreadline.so"* \
    "$STAGE_DIR/lib/libncursesw.so"* \
    "$STAGE_DIR/lib/libtinfo.so"* \
    "$STAGE_DIR/usr/lib/libfdisk.so"* \
    "$STAGE_DIR/usr/lib/libreadline.so"* \
    "$STAGE_DIR/usr/lib/libncursesw.so"* \
    "$STAGE_DIR/usr/lib/libtinfo.so"* 2>/dev/null || true
fi

# Copy only required adb runtime files from twrp sync.
twrp_runtime=(
  adbd
  linker
  libc.so
  libcutils.so
  libm.so
  libc++.so
  libdl.so
  liblog.so
  libminadbd.so
)
for f in "${twrp_runtime[@]}"; do
  if [[ -e "$TWRP_SBIN_DIR/$f" || -L "$TWRP_SBIN_DIR/$f" ]]; then
    cp -a "$TWRP_SBIN_DIR/$f" "$STAGE_DIR/sbin/$f"
  fi
done

# Ensure shebang interpreter for /init is executable and not overwritten.
cp -a "$BB_PATH" "$STAGE_DIR/sbin/busybox"
chmod +x "$STAGE_DIR/sbin/busybox"
ln -snf /sbin/busybox "$STAGE_DIR/bin/sh"

if [[ -f "$STAGE_DIR/init" ]]; then
  chmod +x "$STAGE_DIR/init"
fi

OUT_RAMDISK="$OUT_DIR/initramfs.cpio.gz"
OUT_RAMDISK_LZMA="$OUT_DIR/initramfs.cpio.lzma"
(
  cd "$STAGE_DIR"
  find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$OUT_RAMDISK"
)

# TWRP kernels on jflte expect an LZMA ramdisk blob for reliable boot.
gzip -dc "$OUT_RAMDISK" | xz --format=lzma -9e -c > "$OUT_RAMDISK_LZMA"

echo "initramfs: $OUT_RAMDISK"
ls -lh "$OUT_RAMDISK"
echo "initramfs (lzma): $OUT_RAMDISK_LZMA"
ls -lh "$OUT_RAMDISK_LZMA"
