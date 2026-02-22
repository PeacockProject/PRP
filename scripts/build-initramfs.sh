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
require_cmd find
require_cmd sed
require_cmd sha256sum
require_cmd file
require_cmd python3

load_config "$CFG"
mkdir -p "$OUT_DIR"

ROOTFS_SRC="$PRP_ROOT/$INITRAMFS_ROOTFS"
[[ -d "$ROOTFS_SRC" ]] || die "initramfs rootfs source not found: $ROOTFS_SRC"

STAGE_DIR="$OUT_DIR/initramfs-stage"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

cp -a "$ROOTFS_SRC"/. "$STAGE_DIR"/
INIT_SCRIPT="$STAGE_DIR/init"

# Keep /init as native aarch64 ELF on 64-bit targets and delegate to /init.sh.
# Some boot chains are strict about the init binary format.
if [[ "$TARGET_ARCH" == "aarch64" ]]; then
  require_cmd zig
  [[ -f "$PRP_ROOT/initramfs/init_stub.c" ]] || die "missing init stub source"
  [[ -f "$STAGE_DIR/init" ]] || die "missing init script in stage"
  mv "$STAGE_DIR/init" "$STAGE_DIR/init.sh"
  chmod +x "$STAGE_DIR/init.sh"
  INIT_SCRIPT="$STAGE_DIR/init.sh"
  zig cc -target aarch64-linux-musl -static -Os -s \
    -o "$STAGE_DIR/init" "$PRP_ROOT/initramfs/init_stub.c"
  chmod +x "$STAGE_DIR/init"
fi

# Bake a build tag into /init so on-device logs can confirm the exact image.
build_tag="prp-$(date -u +%Y%m%d)-$(sha256sum "$ROOTFS_SRC/init" | awk '{print substr($1,1,8)}')"
sed -i "s/@PRP_BUILD_TAG@/${build_tag}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_USB_SERIAL@/${USB_SERIAL}/g" "$INIT_SCRIPT"
sed -i "s#@PRP_USB_GADGET_PATH@#${USB_GADGET_PATH}#g" "$INIT_SCRIPT"
sed -i "s#@PRP_USB_UDC_NAME@#${USB_UDC_NAME}#g" "$INIT_SCRIPT"
sed -i "s/@PRP_DO_MOUNT_SUBPARTS@/${DO_MOUNT_SUBPARTS}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_SUBPARTS_ASYNC@/${SUBPARTS_ASYNC:-0}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_SUBPARTS_DELAY_SECS@/${SUBPARTS_DELAY_SECS:-0}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_MOUNT_PRP_ROOTFS@/${MOUNT_PRP_ROOTFS}/g" "$INIT_SCRIPT"
sed -i "s#@PRP_ROOTFS_DEV_HINT@#${ROOTFS_DEV_HINT}#g" "$INIT_SCRIPT"
sed -i "s#@PRP_USERDATA_DEV_HINT@#${USERDATA_DEV_HINT:-}#g" "$INIT_SCRIPT"
sed -i "s/@PRP_ENABLE_FB_IO@/${ENABLE_FB_IO}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_USE_FB_REFRESHER@/${USE_FB_REFRESHER}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_DEBUG_BOOT@/${DEBUG_BOOT}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_START_TTY_SHELLS@/${START_TTY_SHELLS}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_TEXT_CONSOLE_LOG@/${TEXT_CONSOLE_LOG}/g" "$INIT_SCRIPT"
sed -i "s/@PRP_SMOKE_ONLY@/${SMOKE_ONLY}/g" "$INIT_SCRIPT"

# Enforce a unique PRP ramdisk by default.
if [[ -n "${RAMDISK_PREBUILT:-}" ]]; then
  die "RAMDISK_PREBUILT is set in config; clear it for unique PRP ramdisk builds"
fi

VENDOR_DIR="$PRP_ROOT/vendor/$TARGET_NAME"
ROOTFS_RUNTIME_DIR="$VENDOR_DIR/rootfs-runtime"
TWRP_SBIN_DIR="$VENDOR_DIR/twrp-sbin"
if [[ "${REQUIRE_TWRP_SBIN:-1}" == "1" ]]; then
  [[ -d "$TWRP_SBIN_DIR" ]] || die "missing twrp adb assets: $TWRP_SBIN_DIR (run 'make sync-assets')"
fi

# Install static busybox as the initramfs command backbone.
BB_PATH="${BUSYBOX_STATIC/#\~/$HOME}"
[[ -f "$BB_PATH" ]] || die "static busybox not found: $BB_PATH"

busybox_qemu_for() {
  local bb="$1"
  local f=""
  f="$(file "$bb" 2>/dev/null || true)"
  case "$f" in
    *"ARM aarch64"*)
      command -v qemu-aarch64 >/dev/null 2>&1 && { echo qemu-aarch64; return 0; }
      ;;
    *"ELF 32-bit LSB executable, ARM"*)
      command -v qemu-arm >/dev/null 2>&1 && { echo qemu-arm; return 0; }
      ;;
  esac
  echo ""
}

busybox_smoke_ok() {
  local bb="$1"
  local qemu=""
  local out=""
  qemu="$(busybox_qemu_for "$bb")"
  [[ -n "$qemu" ]] || return 1

  out="$(printf 'a\nb\na\n' | "$qemu" "$bb" awk '!seen[$0]++' 2>/dev/null || true)"
  [[ "$out" == $'a\nb' ]] || return 1
  printf 'x\n' | "$qemu" "$bb" grep -q x >/dev/null 2>&1 || return 1
  return 0
}

pick_working_busybox() {
  local c=""
  local -a cand=()

  cand+=("$BB_PATH")
  cand+=("$HOME/.local/var/peacock/busybox-cache/busybox-1.36.1-1-aarch64.pkg.tar.gz/busybox")
  cand+=("$HOME/.local/var/peacock/busybox-cache/busybox-1.36.1-1-armv7h.pkg.tar.gz/busybox")
  cand+=("$HOME/.local/var/peacock/build-chroot/x86_64/build/busybox-1.36.1-armv7/busybox")

  for c in "${cand[@]}"; do
    [[ -f "$c" ]] || continue
    file "$c" | grep -Eq "ELF (64-bit|32-bit).* ARM" || continue
    if busybox_smoke_ok "$c"; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

picked_bb="$(pick_working_busybox || true)"
if [[ -n "$picked_bb" ]]; then
  if [[ "$picked_bb" != "$BB_PATH" ]]; then
    echo "warning: configured busybox failed smoke test, using: $picked_bb" >&2
  fi
  BB_PATH="$picked_bb"
else
  if [[ "$TARGET_ARCH" != "aarch64" ]]; then
    die "no working busybox found for $TARGET_ARCH and no fallback builder available"
  fi
  echo "warning: no working cached busybox; rebuilding static busybox..." >&2
  BB_PATH="$OUT_DIR/busybox-aarch64-static"
  "$SCRIPT_DIR/build-busybox-static.sh" "$TARGET_ARCH" "$ZIG_TARGET" "$BB_PATH"
  busybox_smoke_ok "$BB_PATH" || die "rebuilt busybox failed smoke test (awk/grep)"
fi

mkdir -p "$STAGE_DIR/sbin" "$STAGE_DIR/bin"
cp -a "$BB_PATH" "$STAGE_DIR/sbin/busybox"
chmod +x "$STAGE_DIR/sbin/busybox"
ln -snf /sbin/busybox "$STAGE_DIR/bin/sh"

# Provide a usable local shell toolbox even when adbd is unavailable.
busybox_bin_applets=(
  sh ash
  ls cat echo printf grep sed awk cut tr
  basename dirname readlink
  head tail tee find xargs wc
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

# If rootfs runtime doesn't provide peacock-splash, copy a known-working binary
# from the current Peacock boot.img ramdisk.
if [[ ! -x "$STAGE_DIR/usr/bin/peacock-splash" ]]; then
  BOOTIMG_SRC="${BOOTIMG_ASSET_SOURCE:-$HOME/.local/var/peacock/boot.img}"
  if [[ -f "$BOOTIMG_SRC" ]]; then
    _tmp="$(mktemp -d)"
    if python3 - "$BOOTIMG_SRC" "$_tmp/rd.gz" <<'PY'
import struct
import sys

img = open(sys.argv[1], "rb").read()
if len(img) < 608 or img[:8] != b"ANDROID!":
    raise SystemExit(1)
page = struct.unpack_from("<I", img, 36)[0]
ksz = struct.unpack_from("<I", img, 8)[0]
rsz = struct.unpack_from("<I", img, 16)[0]
rstart = page * (((ksz + page - 1) // page) + 1)
ramdisk = img[rstart:rstart + rsz]
open(sys.argv[2], "wb").write(ramdisk)
PY
    then
      if gzip -dc "$_tmp/rd.gz" | (cd "$_tmp" && cpio -id --quiet 'bin/peacock-splash' './bin/peacock-splash' 2>/dev/null); then
        if [[ -f "$_tmp/bin/peacock-splash" ]]; then
          mkdir -p "$STAGE_DIR/usr/bin"
          cp -a "$_tmp/bin/peacock-splash" "$STAGE_DIR/usr/bin/peacock-splash"
          chmod +x "$STAGE_DIR/usr/bin/peacock-splash"
        fi
      fi
    fi
    rm -rf "$_tmp"
  fi
fi

# Ensure peacock-splash binary matches target architecture.
peacock_splash_ok=0
if [[ -x "$STAGE_DIR/usr/bin/peacock-splash" ]]; then
  case "$TARGET_ARCH" in
    aarch64)
      file "$STAGE_DIR/usr/bin/peacock-splash" 2>/dev/null | grep -q "ARM aarch64" && peacock_splash_ok=1 || true
      ;;
    *)
      file "$STAGE_DIR/usr/bin/peacock-splash" 2>/dev/null | grep -q " ARM" && peacock_splash_ok=1 || true
      ;;
  esac
fi

if [[ "$peacock_splash_ok" != "1" ]]; then
  require_cmd zig
  SPLASH_SRC="$PRP_ROOT/../peacock-ports/base/peacock-splash/splash.c"
  [[ -f "$SPLASH_SRC" ]] || die "missing peacock-splash source: $SPLASH_SRC"
  mkdir -p "$STAGE_DIR/usr/bin"
  zig cc -target "$ZIG_TARGET" -static -Os -s \
    -o "$STAGE_DIR/usr/bin/peacock-splash" "$SPLASH_SRC" -lm
  chmod +x "$STAGE_DIR/usr/bin/peacock-splash"
fi

# msm-fb-refresher is device-specific; keep it enabled only where configured.
if [[ "${USE_FB_REFRESHER:-0}" == "1" ]]; then
  if [[ ! -x "$STAGE_DIR/usr/bin/msm-fb-refresher" ]]; then
    FB_TOOL_DIR="$OUT_DIR/tools/fb-refresher/${TARGET_ARCH}"
    FB_TOOL_BIN="$FB_TOOL_DIR/msm-fb-refresher"
    mkdir -p "$FB_TOOL_DIR" "$STAGE_DIR/usr/bin"
    "$SCRIPT_DIR/build-fb-refresher.sh" "$CFG" "$OUT_DIR" "$FB_TOOL_BIN"
    cp -a "$FB_TOOL_BIN" "$STAGE_DIR/usr/bin/msm-fb-refresher"
    chmod +x "$STAGE_DIR/usr/bin/msm-fb-refresher"
  fi
  if [[ -x "$STAGE_DIR/usr/bin/msm-fb-refresher" ]]; then
    mkdir -p "$STAGE_DIR/sbin"
    ln -snf /usr/bin/msm-fb-refresher "$STAGE_DIR/sbin/msm-fb-refresher"
  fi
else
  rm -f "$STAGE_DIR/usr/bin/msm-fb-refresher" "$STAGE_DIR/sbin/msm-fb-refresher"
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

if [[ -f "$INIT_SCRIPT" ]]; then
  chmod +x "$INIT_SCRIPT"
fi

OUT_RAMDISK="$OUT_DIR/initramfs.cpio.gz"
OUT_RAMDISK_LZMA="$OUT_DIR/initramfs.cpio.lzma"
(
  cd "$STAGE_DIR"
  find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > "$OUT_RAMDISK"
)

echo "initramfs: $OUT_RAMDISK"
ls -lh "$OUT_RAMDISK"

if [[ "$RAMDISK_FORMAT" == "lzma" || "$RAMDISK_FORMAT" == "auto" ]]; then
  require_cmd xz
  # Some legacy kernels require an LZMA ramdisk blob.
  gzip -dc "$OUT_RAMDISK" | xz --format=lzma -9e -c > "$OUT_RAMDISK_LZMA"
  echo "initramfs (lzma): $OUT_RAMDISK_LZMA"
  ls -lh "$OUT_RAMDISK_LZMA"
fi
