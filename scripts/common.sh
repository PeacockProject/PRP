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
  KERNEL_PACKAGE="${KERNEL_PACKAGE:-linux-${TARGET_NAME}}"
  BOARD_NAME="${BOARD_NAME-$TARGET_NAME}"
  TARGET_ARCH="${TARGET_ARCH:-armv7}"
  case "$TARGET_ARCH" in
    arm64) TARGET_ARCH="aarch64" ;;
  esac
  case "$TARGET_ARCH" in
    armv7|armv7h|armhf)
      ZIG_TARGET_DEFAULT="arm-linux-musleabihf"
      ;;
    aarch64)
      ZIG_TARGET_DEFAULT="aarch64-linux-musl"
      ;;
    *)
      ZIG_TARGET_DEFAULT="${TARGET_ARCH}-linux-musl"
      ;;
  esac
  ZIG_TARGET="${ZIG_TARGET:-$ZIG_TARGET_DEFAULT}"
  DROPBEAR_HOST="${DROPBEAR_HOST:-$ZIG_TARGET}"
  CMDLINE="${CMDLINE:-}"
  BASE="${BASE:-0x80200000}"
  KERNEL_OFFSET="${KERNEL_OFFSET:-0x00008000}"
  RAMDISK_OFFSET="${RAMDISK_OFFSET:-0x02000000}"
  SECOND_OFFSET="${SECOND_OFFSET:-0x00f00000}"
  TAGS_OFFSET="${TAGS_OFFSET:-0x00000100}"
  PAGESIZE="${PAGESIZE:-2048}"
  HEADER_VERSION="${HEADER_VERSION:-0}"
  OS_VERSION="${OS_VERSION:-}"
  OS_PATCH_LEVEL="${OS_PATCH_LEVEL:-}"
  APPEND_SEANDROIDENFORCE="${APPEND_SEANDROIDENFORCE:-1}"
  KERNEL_IMAGE="${KERNEL_IMAGE:-}"
  DTB_IMAGE="${DTB_IMAGE:-}"
  RAMDISK_PREBUILT="${RAMDISK_PREBUILT:-}"
  RAMDISK_FORMAT="${RAMDISK_FORMAT:-auto}"
  INITRAMFS_ROOTFS="${INITRAMFS_ROOTFS:-initramfs/rootfs}"
  ROOTFS_IMAGE="${ROOTFS_IMAGE:-$HOME/.local/var/peacock/samsung-jflte.img}"
  ROOTFS_OFFSET_SECTORS="${ROOTFS_OFFSET_SECTORS:-1050624}"
  ROOTFS_SIZE_SECTORS="${ROOTFS_SIZE_SECTORS:-6811648}"
  BUSYBOX_STATIC="${BUSYBOX_STATIC:-$HOME/.local/var/peacock/busybox-cache/busybox-1.36.1-1-armv7h.pkg.tar.gz/busybox}"
  KERNEL_CONFIG="${KERNEL_CONFIG:-$HOME/.local/var/peacock/build-chroot/x86_64/build/linux-samsung-jflte-3.4.113.8-armv7/.config}"
  REQUIRE_TWRP_SBIN="${REQUIRE_TWRP_SBIN:-1}"
  ADB_PREFIX="${ADB_PREFIX:-adb}"
  FLASH_METHOD="${FLASH_METHOD:-adb-dd}"
  FASTBOOT_PREFIX="${FASTBOOT_PREFIX:-fastboot}"
  FASTBOOT_PARTITIONS="${FASTBOOT_PARTITIONS:-boot}"
  FASTBOOT_SET_ACTIVE="${FASTBOOT_SET_ACTIVE:-}"
  IS_MSM="${IS_MSM:-0}"
  IS_MSM_FB_REFRESHER="${IS_MSM_FB_REFRESHER:-0}"
  IS_MSM_LK2ND_SPLIT_BOOT="${IS_MSM_LK2ND_SPLIT_BOOT:-0}"
  # Generic A/B + lk2nd split-boot layout:
  # - lk2nd lives in the first slice of "boot"
  # - actual boot image is flashed via lk2nd fastboot to "boot" (offset payload)
  FASTBOOT_AB_LK2ND_SPLIT_BOOT="${FASTBOOT_AB_LK2ND_SPLIT_BOOT:-$IS_MSM_LK2ND_SPLIT_BOOT}"
  FASTBOOT_LK2ND_PARTITION="${FASTBOOT_LK2ND_PARTITION:-lk2nd}"
  FASTBOOT_BOOT_PARTITION="${FASTBOOT_BOOT_PARTITION:-boot}"
  FASTBOOT_LK2ND_IMAGE="${FASTBOOT_LK2ND_IMAGE:-}"
  RECOVERY_BLOCK="${RECOVERY_BLOCK:-/dev/block/mmcblk0p21}"
  USB_SERIAL="${USB_SERIAL:-PRP-$TARGET_NAME}"
  USB_GADGET_PATH="${USB_GADGET_PATH:-}"
  USB_UDC_NAME="${USB_UDC_NAME:-}"
  GUI_POWER_INPUT="${GUI_POWER_INPUT:-}"
  GUI_POWER_HINT="${GUI_POWER_HINT:-}"
  GUI_POWER_CODE="${GUI_POWER_CODE:-}"
  DO_MOUNT_SUBPARTS="${DO_MOUNT_SUBPARTS:-1}"
  MOUNT_PRP_ROOTFS="${MOUNT_PRP_ROOTFS:-1}"
  ROOTFS_DEV_HINT="${ROOTFS_DEV_HINT:-}"
  PRP_MONOLITHIC_INITRAMFS="${PRP_MONOLITHIC_INITRAMFS:-0}"
  ENABLE_FB_IO="${ENABLE_FB_IO:-1}"
  USE_FB_REFRESHER_DEFAULT="${IS_MSM_FB_REFRESHER}"
  USE_FB_REFRESHER="${USE_FB_REFRESHER:-$USE_FB_REFRESHER_DEFAULT}"
  DEBUG_BOOT="${DEBUG_BOOT:-0}"
  START_TTY_SHELLS="${START_TTY_SHELLS:-1}"
  TEXT_CONSOLE_LOG="${TEXT_CONSOLE_LOG:-1}"
  SMOKE_ONLY="${SMOKE_ONLY:-0}"
}

# Substitute the @PRP_*@ template tokens in a copy of /init. Single-sourced so
# the initramfs build (native /init) and the overlay build (the recovery
# re-entry copy installed as /usr/lib/prp/prp-init) can never drift apart.
# Requires load_config to have run. Arg 2 (build tag) is optional; computed from
# the pristine source /init when omitted.
substitute_prp_init() {
  local init_file="$1"
  local build_tag="${2:-}"
  if [[ -z "$build_tag" ]]; then
    local src="$PRP_ROOT/${INITRAMFS_ROOTFS:-initramfs/rootfs}/init"
    build_tag="prp-$(date -u +%Y%m%d)-$(sha256sum "$src" | awk '{print substr($1,1,8)}')"
  fi
  sed -i \
    -e "s/@PRP_BUILD_TAG@/${build_tag}/g" \
    -e "s/@PRP_USB_SERIAL@/${USB_SERIAL}/g" \
    -e "s#@PRP_USB_GADGET_PATH@#${USB_GADGET_PATH}#g" \
    -e "s#@PRP_USB_UDC_NAME@#${USB_UDC_NAME}#g" \
    -e "s/@PRP_DO_MOUNT_SUBPARTS@/${DO_MOUNT_SUBPARTS}/g" \
    -e "s/@PRP_SUBPARTS_ASYNC@/${SUBPARTS_ASYNC:-0}/g" \
    -e "s/@PRP_SUBPARTS_DELAY_SECS@/${SUBPARTS_DELAY_SECS:-0}/g" \
    -e "s/@PRP_MOUNT_PRP_ROOTFS@/${MOUNT_PRP_ROOTFS}/g" \
    -e "s#@PRP_ROOTFS_DEV_HINT@#${ROOTFS_DEV_HINT}#g" \
    -e "s#@PRP_USERDATA_DEV_HINT@#${USERDATA_DEV_HINT:-}#g" \
    -e "s/@PRP_MONOLITHIC_INITRAMFS@/${PRP_MONOLITHIC_INITRAMFS}/g" \
    -e "s/@PRP_ENABLE_FB_IO@/${ENABLE_FB_IO}/g" \
    -e "s/@PRP_USE_FB_REFRESHER@/${USE_FB_REFRESHER}/g" \
    -e "s/@PRP_DEBUG_BOOT@/${DEBUG_BOOT}/g" \
    -e "s/@PRP_PROBE_DEBUG_PARTITION@/${PROBE_DEBUG_PARTITION:-1}/g" \
    -e "s/@PRP_START_TTY_SHELLS@/${START_TTY_SHELLS}/g" \
    -e "s/@PRP_TEXT_CONSOLE_LOG@/${TEXT_CONSOLE_LOG}/g" \
    -e "s/@PRP_START_SSH@/${START_SSH:-1}/g" \
    -e "s/@PRP_SMOKE_ONLY@/${SMOKE_ONLY}/g" \
    "$init_file"
}

resolve_kernel_image() {
  local kernel_pkg="${KERNEL_PACKAGE:-linux-${TARGET_NAME}}"
  local default_kernel_pkg="linux-${TARGET_NAME}"

  _candidate_matches_kernel_pkg() {
    local path="$1"
    local pkg="$2"
    local -a _parts=()
    local component=""
    local rest=""

    IFS='/' read -r -a _parts <<<"$path"
    for component in "${_parts[@]}"; do
      [[ "$component" == "${pkg}-"* ]] || continue
      rest="${component#${pkg}-}"
      [[ "$rest" =~ ^[0-9] ]] && return 0
    done
    return 1
  }

  if [[ -n "${KERNEL_IMAGE:-}" && -f "$KERNEL_IMAGE" ]]; then
    echo "$KERNEL_IMAGE"
    return 0
  fi

  # Prefer a PRP-trimmed kernel (zImage-prp) when the kernel port built one
  # (peacock-ports linux-<dev> declaring prp_kernel_config stages it beside
  # the full zImage). Recovery images want the trimmed, no-modules kernel.
  # Falls through to the full kernel below when no zImage-prp exists.
  local prp_candidates=(
    "$HOME/.local/var/peacock/build-chroot/*/build/${kernel_pkg}-[0-9]*/zImage-prp"
    "$HOME/.local/var/peacock/kernel-cache/${kernel_pkg}-[0-9]*.pkg.tar.gz/zImage-prp"
    "$HOME/.local/var/peacock/packages/*/${kernel_pkg}-[0-9]*.pkg.tar.gz/zImage-prp"
  )
  local prp_best=""
  local pp=""
  local pc
  shopt -s nullglob
  for pp in "${prp_candidates[@]}"; do
    for pc in $pp; do
      [[ -f "$pc" ]] || continue
      _candidate_matches_kernel_pkg "$pc" "$kernel_pkg" || continue
      if [[ -z "$prp_best" || "$pc" -nt "$prp_best" ]]; then
        prp_best="$pc"
      fi
    done
  done
  shopt -u nullglob
  if [[ -n "$prp_best" ]]; then
    echo "$prp_best"
    return 0
  fi

  local candidates=(
    "$HOME/.local/var/peacock/build-chroot/*/build/${kernel_pkg}-[0-9]*/zImage"
    "$HOME/.local/var/peacock/build-chroot/*/build/${kernel_pkg}-[0-9]*/Image.gz"
    "$HOME/.local/var/peacock/build-chroot/*/build/${kernel_pkg}-[0-9]*/arch/*/boot/zImage"
    "$HOME/.local/var/peacock/build-chroot/*/build/${kernel_pkg}-[0-9]*/arch/*/boot/Image.gz"
    "$HOME/.local/var/peacock/kernel-cache/${kernel_pkg}-[0-9]*.pkg.tar.gz/zImage"
    "$HOME/.local/var/peacock/kernel-cache/${kernel_pkg}-[0-9]*.pkg.tar.gz/Image.gz"
    "$HOME/.local/var/peacock/packages/*/${kernel_pkg}-[0-9]*.pkg.tar.gz/zImage"
    "$HOME/.local/var/peacock/packages/*/${kernel_pkg}-[0-9]*.pkg.tar.gz/Image.gz"
  )
  if [[ "$kernel_pkg" == "$default_kernel_pkg" ]]; then
    candidates+=("$HOME/.local/var/peacock/boot-p1.img")
  fi
  local best=""
  local pat=""
  local c
  shopt -s nullglob
  for pat in "${candidates[@]}"; do
    for c in $pat; do
      [[ -f "$c" ]] || continue
      _candidate_matches_kernel_pkg "$c" "$kernel_pkg" || continue
      if [[ -z "$best" || "$c" -nt "$best" ]]; then
        best="$c"
      fi
    done
  done
  shopt -u nullglob

  if [[ -n "$best" ]]; then
    echo "$best"
    return 0
  fi

  if [[ -d "$HOME/.local/var/peacock/build-chroot" ]]; then
    while IFS= read -r c; do
      [[ -f "$c" ]] || continue
      _candidate_matches_kernel_pkg "$c" "$kernel_pkg" || continue
      best="$c"
      break
    done < <(find "$HOME/.local/var/peacock/build-chroot" -maxdepth 6 -type f \( -name zImage -o -name Image.gz \) -path "*/${kernel_pkg}-*/*" 2>/dev/null)
    if [[ -n "$best" ]]; then
      echo "$best"
      return 0
    fi
  fi

  die "could not resolve kernel image for package ${kernel_pkg}; set KERNEL_IMAGE in config or build ${kernel_pkg}"
}

resolve_ramdisk_image() {
  local out_dir="$1"
  if [[ -n "${RAMDISK_PREBUILT:-}" && -f "$RAMDISK_PREBUILT" ]]; then
    echo "$RAMDISK_PREBUILT"
    return 0
  fi

  local generated="$out_dir/initramfs.cpio.gz"
  local generated_lzma="$out_dir/initramfs.cpio.lzma"
  case "$RAMDISK_FORMAT" in
    lzma)
      [[ -f "$generated_lzma" ]] && { echo "$generated_lzma"; return 0; }
      [[ -f "$generated" ]] && { echo "$generated"; return 0; }
      ;;
    gzip|gz)
      [[ -f "$generated" ]] && { echo "$generated"; return 0; }
      [[ -f "$generated_lzma" ]] && { echo "$generated_lzma"; return 0; }
      ;;
    auto|*)
      [[ -f "$generated_lzma" ]] && { echo "$generated_lzma"; return 0; }
      [[ -f "$generated" ]] && { echo "$generated"; return 0; }
      ;;
  esac

  die "could not resolve ramdisk image; run 'make initramfs' or set RAMDISK_PREBUILT"
}
