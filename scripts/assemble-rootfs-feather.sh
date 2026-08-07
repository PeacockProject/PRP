#!/usr/bin/env bash
# assemble-rootfs-feather.sh — build the PRP rootfs from feather packages.
#
# Replaces the old scavenging model (sync-runtime-assets.sh + the inline
# busybox/dropbear builds). Instead PRP's rootfs is a declared package set
# (`prp-base`) installed with `ftr` into a staging tree — dependency-resolved,
# signature-verified, same artifacts as the system.
#
# Flow:
#   1. peacock build-packages --arch <ARCH> the shared ports (busybox, dropbear,
#      util-linux, peacock-splash, e2fsprogs, wpa_supplicant, …) → cached .feathers
#   2. build the PRP GUI (build-gui.sh) → pack prp-gui.feather
#   3. pack prp-runtime.feather (the PRP init + /usr/lib/prp tree, incl. prp-net)
#   4. generate prp-base.feather (layout=meta, depends on all of the above)
#   5. ftr index + sign (build-local key) + ftr sync (file://) + ftr install --root
#
# Prints the staged rootfs path on stdout (last line) for build-initramfs.sh.
#
# Usage: assemble-rootfs-feather.sh <config.env> <out_dir>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"; OUT_DIR="$2"
load_config "$CFG"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"   # absolute — the tar/cd subshells below need it

ARCH="${TARGET_ARCH:-aarch64}"

# Tool locations (override via env for non-standard checkouts).
PROJECT_ROOT="$(cd "$PRP_ROOT/.." && pwd)"
# Use the working-tree ports (incl. util-linux-prp), not Peacock's stale
# auto-clone, when resolving + building ports.
export PEACOCK_PORTS_DIR="${PEACOCK_PORTS_DIR:-$PROJECT_ROOT/peacock-ports}"
PEACOCK="${PEACOCK_BIN:-$PROJECT_ROOT/Peacock/peacock}"
FEATHER_DIR="${FEATHER_DIR:-$PROJECT_ROOT/feather}"
FTR="${FTR_BIN:-$FEATHER_DIR/ftr}"
GENKEY="${GENKEY_BIN:-$FEATHER_DIR/tools/gen-keypair}"
SIGN="${SIGN_BIN:-$FEATHER_DIR/tools/ftr-sign}"
PKG_STORE="${PEACOCK_PKG_STORE:-$HOME/.local/var/peacock/packages/$ARCH}"

for t in "$PEACOCK" "$FTR" "$GENKEY" "$SIGN"; do
  [[ -x "$t" ]] || die "required tool not found/executable: $t"
done

# Shared ports built by Peacock (already feather packages). util-linux-prp is the
# lean static-musl subset (fdisk/sfdisk/blkid/partx/losetup) — self-contained, so
# PRP needs no glibc/ncurses/readline. wpa_supplicant + e2fsprogs land later;
# absent ports are skipped with a warn.
SHARED_PORTS=(busybox dropbear util-linux-prp e2fsprogs-prp peacock-splash wpa_supplicant peacock-mkinitfs peacock-init-wrapper mkbootimg)
# The dep list baked into the prp-base metapackage (names only). `feather` ships
# ftr itself so the on-device installer (prp-install) can sync + install + manage
# the keyring — without it the installer has no package manager.
PRP_BASE_DEPS=(busybox dropbear util-linux-prp e2fsprogs-prp wpa_supplicant peacock-splash feather peacock-mkinitfs peacock-init-wrapper mkbootimg prp-gui prp-runtime)

WORK="$OUT_DIR/feather-assembly"
REPO="$WORK/repo"; STAGE="$WORK/stage"; KEYS="$WORK/keys"
rm -rf "$WORK"; mkdir -p "$REPO" "$STAGE" "$KEYS"

echo "assemble: building shared ports for $ARCH …"
build_list=()
for p in "${SHARED_PORTS[@]}"; do
  # Probe the SAME tree that was exported above and that `peacock` will resolve
  # — not a second hardcoded guess. These coincide only when the checkout sits
  # at $PROJECT_ROOT/peacock-ports; with PEACOCK_PORTS_DIR pointed anywhere else
  # every port silently "wasn't found" and the build list came out empty.
  if [[ -d "$PEACOCK_PORTS_DIR/base/$p" || -d "$PEACOCK_PORTS_DIR/device/$p" ]]; then
    build_list+=(-p "$p")
  else
    echo "assemble: WARN port '$p' not found, skipping" >&2
  fi
done
"$PEACOCK" build-packages --arch "$ARCH" "${build_list[@]}"

echo "assemble: collecting shared .feathers …"
for p in "${SHARED_PORTS[@]}"; do
  f=$(ls -1 "$PKG_STORE/$p-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
  [[ -n "$f" ]] && cp "$f" "$REPO/" || echo "assemble: WARN no built feather for '$p'" >&2
done

# `feather` (ftr) is packed ad-hoc, not a peacock port: cross-build + pack it here
# if it isn't already in the store, then collect it. Ships ftr + the trust keyring
# into PRP so prp-install can run the package manager on-device.
echo "assemble: ensuring feather (ftr) package for $ARCH …"
feather_pkg=$(ls -1 "$PKG_STORE/feather-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
if [[ -z "$feather_pkg" ]]; then
  case "$ARCH" in
    aarch64) ZT="aarch64-linux-musl" ;;
    armv7|armv7h|armhf) ZT="arm-linux-musleabihf" ;;
    *) ZT="$ARCH-linux-musl" ;;
  esac
  echo "assemble: cross-building ftr ($ZT) …"
  ( cd "$FEATHER_DIR" \
    && ZIG_GLOBAL_CACHE_DIR=/tmp/zig-cache ZIG_LOCAL_CACHE_DIR=/tmp/zig-cache \
       make clean >/dev/null 2>&1 \
    && ZIG_GLOBAL_CACHE_DIR=/tmp/zig-cache ZIG_LOCAL_CACHE_DIR=/tmp/zig-cache \
       make build CC="zig cc -target $ZT" >/dev/null 2>&1 )
  fs=$(mktemp -d); mkdir -p "$fs/files/usr/bin"
  cp "$FEATHER_DIR/ftr" "$fs/files/usr/bin/ftr"
  (strip "$fs/files/usr/bin/ftr" 2>/dev/null || llvm-strip "$fs/files/usr/bin/ftr" 2>/dev/null || true)
  printf '[package]\nname = "feather"\nversion = "0.1.0"\ndescription = "feather (ftr) package manager + trust keyring."\n\n[install]\nlayout = "system"\n' >"$fs/manifest.toml"
  mkdir -p "$PKG_STORE"
  ( cd "$fs" && tar -czf "$PKG_STORE/feather-0.1.0-1-$ARCH.feather" --numeric-owner manifest.toml files )
  rm -rf "$fs"
  # restore the host ftr for the rest of this script's signing/index/install steps
  ( cd "$FEATHER_DIR" && make clean >/dev/null 2>&1 && make build tools/ftr-sign tools/gen-keypair >/dev/null 2>&1 )
  feather_pkg=$(ls -1 "$PKG_STORE/feather-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
fi
[[ -n "$feather_pkg" ]] && cp "$feather_pkg" "$REPO/" || echo "assemble: WARN feather (ftr) package missing" >&2

echo "assemble: packing prp-gui …"
"$SCRIPT_DIR/build-gui.sh" "$CFG" "$OUT_DIR"
GUI_BIN="$OUT_DIR/tools/gui-out/$ARCH/prp-gui"
[[ -f "$GUI_BIN" ]] || die "prp-gui binary missing: $GUI_BIN"
g="$WORK/pkg-gui"; mkdir -p "$g/files/usr/bin"
cp "$GUI_BIN" "$g/files/usr/bin/prp-gui"
printf '[package]\nname = "prp-gui"\nversion = "0.1.0"\n\n[install]\nlayout = "system"\n' >"$g/manifest.toml"
( cd "$g" && tar -czf "$REPO/prp-gui-0.1.0-$ARCH.feather" manifest.toml files )

echo "assemble: packing prp-runtime …"
r="$WORK/pkg-runtime"; mkdir -p "$r/files"
cp -a "$PRP_ROOT/initramfs/rootfs/." "$r/files/"
printf '[package]\nname = "prp-runtime"\nversion = "0.1.0"\n\n[install]\nlayout = "system"\n' >"$r/manifest.toml"
( cd "$r" && tar -czf "$REPO/prp-runtime-0.1.0-$ARCH.feather" manifest.toml files )

echo "assemble: generating prp-base metapackage …"
m="$WORK/pkg-base"; mkdir -p "$m"
deps=$(printf '"%s", ' "${PRP_BASE_DEPS[@]}"); deps="${deps%, }"
printf '[package]\nname = "prp-base"\nversion = "0.1.0"\ndepends = [%s]\n\n[install]\nlayout = "meta"\n' "$deps" >"$m/manifest.toml"
( cd "$m" && tar -czf "$REPO/prp-base-0.1.0-$ARCH.feather" manifest.toml )

echo "assemble: signing + indexing …"
"$GENKEY" "prp-build-${ARCH}" "$KEYS/k.pub" "$KEYS/k.sec" "prp build key" >/dev/null
export FTR_PUBKEY="$KEYS/k.pub"
for f in "$REPO"/*.feather; do "$SIGN" "$KEYS/k.sec" "$f" "$f.sig" "prp" >/dev/null; done
"$FTR" index --arch "$ARCH" --name prp "$REPO" >/dev/null
"$SIGN" "$KEYS/k.sec" "$REPO/index.toml" "$REPO/index.toml.sig" "prp index" >/dev/null

echo "assemble: installing prp-base into the stage …"
export FTR_DB_ROOT="$STAGE/var/lib/feather"
export FTR_CONFIG="$WORK/feather.conf"
printf '[[repos]]\nname = "prp"\nurl = "file://%s"\npubkey = "%s"\n' "$REPO" "$KEYS/k.pub" >"$WORK/feather.conf"
"$FTR" sync
"$FTR" install --root "$STAGE" --arch "$ARCH" prp-base

# Kernel modules + firmware for on-device wifi (and other modular drivers). The
# boot kernel ships NO modules — they live here on the rootfs and are modprobe'd
# after it mounts (the boot part only boots into prp-rootfs). The PRP kernel's
# modules ride in the linux-<dev>-prp feather under usr/lib/modules; firmware
# (wcnss/prima + GPU) in firmware-<dev>.
echo "assemble: staging kernel modules + firmware …"
# `|| true`: with set -e + pipefail a no-match `ls` exits 2 and kills the script,
# and 2>/dev/null swallows the reason — a silent abort with no diagnostic. These
# two lookups are optional by design (a modules-free PRP kernel, or firmware that
# ships as a tarball rather than a local feather), so absence must not be fatal.
KMOD_FEATHER=$(ls -1 "$PKG_STORE/linux-${TARGET_NAME}-prp-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
if [[ -n "$KMOD_FEATHER" ]]; then
  tar -xzf "$KMOD_FEATHER" -C "$STAGE" --strip-components=1 'files/usr/lib/modules' 2>/dev/null \
    && echo "assemble: modules <- $(basename "$KMOD_FEATHER")" \
    || echo "assemble: WARN no usr/lib/modules in $(basename "$KMOD_FEATHER")" >&2
else
  echo "assemble: WARN no linux-${TARGET_NAME}-prp feather (no kernel modules)" >&2
fi
FW_FEATHER=$(ls -1 "$PKG_STORE/firmware-${TARGET_NAME}-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
if [[ -n "$FW_FEATHER" ]]; then
  tar -xzf "$FW_FEATHER" -C "$STAGE" --strip-components=1 'files/lib/firmware' 2>/dev/null \
    && echo "assemble: firmware <- $(basename "$FW_FEATHER")" \
    || echo "assemble: WARN no lib/firmware in $(basename "$FW_FEATHER")" >&2
fi
# Some devices' firmware package deliberately ships NO blobs — they aren't
# redistributable, so the package is an extractor that runs against the phone's
# own partitions at install time. That works for an installed system but not for
# building a recovery ramdisk on a host, so those profiles point at a
# /lib/firmware-shaped tarball instead and name the few paths PRP must carry.
# The bulk stays on the device and is mounted at runtime (FW_RUNTIME_PART).
if [[ -n "${FIRMWARE_TARBALL_URL:-}" ]]; then
  FW_CACHE="${PEACOCK_FW_CACHE:-$HOME/.local/var/peacock/firmware-cache}"
  mkdir -p "$FW_CACHE"
  FW_TAR="$FW_CACHE/${TARGET_NAME}-$(basename "$FIRMWARE_TARBALL_URL")"
  if [[ ! -f "$FW_TAR" ]]; then
    echo "assemble: fetching vendor firmware tarball …"
    if command -v curl >/dev/null 2>&1; then
      curl -fSL -o "$FW_TAR.part" "$FIRMWARE_TARBALL_URL" || die "firmware tarball fetch failed: $FIRMWARE_TARBALL_URL"
    elif command -v wget >/dev/null 2>&1; then
      wget -qO "$FW_TAR.part" "$FIRMWARE_TARBALL_URL" || die "firmware tarball fetch failed: $FIRMWARE_TARBALL_URL"
    else
      die "need curl or wget to fetch FIRMWARE_TARBALL_URL"
    fi
    mv "$FW_TAR.part" "$FW_TAR"
  fi
  # The tarball is served over plain HTTPS with no detached signature, unlike the
  # signed .feather packages — so the digest in the device profile is the only
  # integrity anchor there is. Refuse to bake in unverified firmware.
  if [[ -n "${FIRMWARE_TARBALL_SHA256:-}" ]]; then
    _got="$(sha256sum "$FW_TAR" | awk '{print $1}')"
    [[ "$_got" == "$FIRMWARE_TARBALL_SHA256" ]] || {
      rm -f "$FW_TAR"
      die "firmware tarball digest mismatch: got $_got want $FIRMWARE_TARBALL_SHA256"
    }
    echo "assemble: firmware tarball digest OK"
  else
    echo "assemble: WARN FIRMWARE_TARBALL_SHA256 unset — firmware is unverified" >&2
  fi
  _fwtmp="$OUT_DIR/firmware-unpack"
  rm -rf "$_fwtmp"; mkdir -p "$_fwtmp"
  tar -xzf "$FW_TAR" -C "$_fwtmp"
  mkdir -p "$STAGE/lib/firmware"
  if [[ -n "${FIRMWARE_INCLUDE:-}" ]]; then
    for _inc in $FIRMWARE_INCLUDE; do
      if [[ -e "$_fwtmp/$_inc" ]]; then
        mkdir -p "$STAGE/lib/firmware/$(dirname "$_inc")"
        cp -a "$_fwtmp/$_inc" "$STAGE/lib/firmware/$_inc"
      else
        echo "assemble: WARN FIRMWARE_INCLUDE path not in tarball: $_inc" >&2
      fi
    done
    echo "assemble: firmware <- tarball subset ($(du -sh "$STAGE/lib/firmware" | cut -f1))"
  else
    cp -a "$_fwtmp/." "$STAGE/lib/firmware/"
    echo "assemble: firmware <- tarball, whole ($(du -sh "$STAGE/lib/firmware" | cut -f1))"
  fi
  rm -rf "$_fwtmp"
fi
# Generic wireless-regdb (regulatory.db) so 5GHz/DFS channels become usable once
# a country is set (prp-net WIFI_COUNTRY). Vendored in-repo; the kernel verifies
# the .p7s signature against its built-in regdb keys (CFG80211_USE_KERNEL_REGDB_KEYS).
if [[ -f "$PRP_ROOT/firmware/wireless-regdb/regulatory.db" ]]; then
  mkdir -p "$STAGE/lib/firmware"
  cp "$PRP_ROOT/firmware/wireless-regdb/regulatory.db" \
     "$PRP_ROOT/firmware/wireless-regdb/regulatory.db.p7s" "$STAGE/lib/firmware/"
  echo "assemble: regulatory.db <- vendored wireless-regdb"
fi
# Regenerate modules.dep so on-device modprobe can resolve the wifi stack.
# `|| true` for the same reason as the feather lookups above: a PRP kernel built
# with CONFIG_MODULES=n stages no usr/lib/modules at all, and the bare `ls` would
# otherwise exit 2 and abort the whole assembly one line before it finishes.
KVER=$(ls "$STAGE/usr/lib/modules/" 2>/dev/null | head -1 || true)
if [[ -n "$KVER" ]] && command -v depmod >/dev/null 2>&1; then
  depmod -b "$STAGE" "$KVER" 2>/dev/null && echo "assemble: depmod $KVER" || true
fi

# On-device feather.conf: point the installer's `ftr` at genmirror. Templated
# from the GENMIRROR_* config entries (overridable per-device/env) so the
# address is never hardcoded here. The trust anchor (genmirror.pub) ships in the
# prp-runtime tree and is now installed at $STAGE$GENMIRROR_PUBKEY.
echo "assemble: writing on-device genmirror feather.conf …"
mkdir -p "$STAGE/etc/feather"
if [[ ! -f "$STAGE$GENMIRROR_PUBKEY" ]]; then
  echo "assemble: WARN genmirror trust anchor missing at $STAGE$GENMIRROR_PUBKEY" >&2
fi
printf '[[repos]]\nname = "%s"\nurl = "%s/%s/%s"\npubkey = "%s"\n' \
  "$GENMIRROR_REPO_NAME" "$GENMIRROR_URL" "$GENMIRROR_CHANNEL" "$ARCH" "$GENMIRROR_PUBKEY" \
  >"$STAGE/etc/feather/feather.conf"
echo "assemble: feather.conf -> $GENMIRROR_URL/$GENMIRROR_CHANNEL/$ARCH"

# Blueprint base URL for the installer wizard + first-boot OOBE to fetch per-flavor blueprints
# (separate from the package repo; not a feather concern). Same genmirror, blueprints/ folder.
mkdir -p "$STAGE/etc/peacock"
printf '%s/blueprints/%s\n' "$GENMIRROR_URL" "$GENMIRROR_CHANNEL" >"$STAGE/etc/peacock/blueprints-base"
echo "assemble: blueprints-base -> $GENMIRROR_URL/blueprints/$GENMIRROR_CHANNEL"

# On-device /etc/prp/device.conf: boot params for prp-install-bootloader,
# templated from the device profile so the flasher isn't hardcoded.
echo "assemble: writing on-device /etc/prp/device.conf (boot params) …"
mkdir -p "$STAGE/etc/prp"
_split="${FASTBOOT_AB_LK2ND_SPLIT_BOOT:-${IS_MSM_LK2ND_SPLIT_BOOT:-0}}"
_slot="${FASTBOOT_SET_ACTIVE:-}"
_bootpart="${FASTBOOT_BOOT_PARTITION:-boot}"
if [[ -n "$_slot" ]]; then
  _hint="/dev/block/bootdevice/by-name/${_bootpart}_${_slot} /dev/block/by-name/${_bootpart}_${_slot} /dev/block/bootdevice/by-name/${_bootpart} /dev/block/by-name/${_bootpart}"
else
  _hint="/dev/block/bootdevice/by-name/${_bootpart} /dev/block/by-name/${_bootpart}"
fi
{
  printf '# Generated by assemble-rootfs-feather.sh from the %s device profile.\n' "$TARGET_NAME"
  printf '# Consumed by prp-install-bootloader (on-device boot.img assembly + flash).\n'
  printf 'DEVICE_CODENAME=%s\n' "$TARGET_NAME"
  printf 'ROOT_LABEL=%s\n' "${ROOT_LABEL:-PEACOCK_ROOT}"
  printf '\n# --- flash target (where the assembled boot.img is written) ---\n'
  printf 'LK2ND_SPLIT_BOOT=%s\n' "$_split"
  printf 'LK2ND_OFFSET=%s\n' "${LK2ND_OFFSET:-524288}"
  printf 'BOOT_DEV_HINT="%s"\n' "$_hint"
  printf '\n# --- mkbootimg params (installed-system boot.img assembly) ---\n'
  printf 'KERNEL_BASE=%s\n' "${BASE:-0x80000000}"
  printf 'KERNEL_OFFSET=%s\n' "${KERNEL_OFFSET:-0x00008000}"
  printf 'RAMDISK_OFFSET=%s\n' "${RAMDISK_OFFSET:-0x01000000}"
  printf 'SECOND_OFFSET=%s\n' "${SECOND_OFFSET:-0x00f00000}"
  printf 'TAGS_OFFSET=%s\n' "${TAGS_OFFSET:-0x00000100}"
  printf 'PAGESIZE=%s\n' "${PAGESIZE:-2048}"
  printf 'HEADER_VERSION=%s\n' "${HEADER_VERSION:-0}"
  printf 'BOARD_NAME="%s"\n' "${BOARD_NAME:-}"
  printf 'BOOT_CMDLINE="%s"\n' "${CMDLINE:-}"
} >"$STAGE/etc/prp/device.conf"
echo "assemble: device.conf -> split=$_split slot=${_slot:-none} part=$_bootpart codename=$TARGET_NAME"

# On-device /etc/prp/firmware.conf: where the large vendor blobs live. Kept in
# its own file rather than device.conf because init sources it directly, and
# device.conf carries mkbootimg variables (PAGESIZE, BOARD_NAME, …) that would
# be reckless to pull into PID 1's namespace.
if [[ -n "${FW_RUNTIME_PART:-}" ]]; then
  {
    printf '# Generated by assemble-rootfs-feather.sh from the %s device profile.\n' "$TARGET_NAME"
    printf '# Consumed by init + prp-net via usr/lib/prp/firmware-runtime.sh.\n'
    printf 'FW_RUNTIME_PART=%s\n' "$FW_RUNTIME_PART"
    printf 'FW_RUNTIME_SUBDIR=%s\n' "${FW_RUNTIME_SUBDIR:-}"
    printf 'FW_RUNTIME_FSTYPE=%s\n' "${FW_RUNTIME_FSTYPE:-vfat}"
    printf 'FW_RUNTIME_RPROC_FW=%s\n' "${FW_RUNTIME_RPROC_FW:-}"
  } >"$STAGE/etc/prp/firmware.conf"
  echo "assemble: firmware.conf -> part=$FW_RUNTIME_PART subdir=${FW_RUNTIME_SUBDIR:-/} rproc=${FW_RUNTIME_RPROC_FW:-none}"
fi

# On-device prp-gui.conf: DPI scale for the panel (templated from GUI_SCALE).
echo "assemble: writing on-device /etc/prp-gui.conf (scale=${GUI_SCALE:-100}) …"
printf 'scale=%s\n' "${GUI_SCALE:-100}" >"$STAGE/etc/prp-gui.conf"

echo "assemble: done. staged rootfs:"
echo "$STAGE"
