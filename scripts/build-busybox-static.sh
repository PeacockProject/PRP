#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 3 ]] || die "usage: $0 <target_arch> <zig_target> <out_binary>"
TARGET_ARCH="$1"
ZIG_TARGET="$2"
OUT_BIN="$3"

require_cmd curl
require_cmd tar
require_cmd make
require_cmd zig
require_cmd python3
require_cmd sha256sum
require_cmd file

case "$TARGET_ARCH" in
  aarch64)                BB_ARCH="arm64"; BB_EXPECT="ARM aarch64" ;;
  x86_64|amd64)           BB_ARCH="x86";   BB_EXPECT="x86-64" ;;
  arm|armv7|armv7h|armhf) BB_ARCH="arm";   BB_EXPECT="ARM" ;;
  *)
    die "busybox static builder: unsupported arch $TARGET_ARCH"
    ;;
esac

BB_VER="1.36.1"
BB_TARBALL="busybox-${BB_VER}.tar.bz2"
BB_URL="https://busybox.net/downloads/${BB_TARBALL}"
BB_SHA256="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"

CACHE_DIR="$PRP_ROOT/out/.cache/busybox"
SRC_DIR="$CACHE_DIR/src/busybox-${BB_VER}"
WORK_DIR="$CACHE_DIR/work-${TARGET_ARCH}"
mkdir -p "$CACHE_DIR/src" "$WORK_DIR"

if [[ ! -f "$CACHE_DIR/$BB_TARBALL" ]]; then
  curl -L --fail -o "$CACHE_DIR/$BB_TARBALL" "$BB_URL"
fi

calc="$(sha256sum "$CACHE_DIR/$BB_TARBALL" | awk '{print $1}')"
[[ "$calc" == "$BB_SHA256" ]] || die "busybox tarball checksum mismatch ($calc)"

if [[ ! -d "$SRC_DIR" ]]; then
  tar -C "$CACHE_DIR/src" -xf "$CACHE_DIR/$BB_TARBALL"
fi

BUILD_DIR="$WORK_DIR/build"
rm -rf "$BUILD_DIR"
cp -a "$SRC_DIR" "$BUILD_DIR"

pushd "$BUILD_DIR" >/dev/null

make defconfig >/dev/null
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
# Avoid tc build breakage against modern musl/netlink headers.
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
set +o pipefail
yes '' | make oldconfig >/dev/null
set -o pipefail

# BusyBox trylink script uses GNU ld flags unsupported by zig/lld.
python3 - <<'PY'
from pathlib import Path
p = Path("scripts/trylink")
s = p.read_text()
s = s.replace("-Wl,--warn-common ", "")
s = s.replace("-Wl,--verbose", "")
s = s.replace("-Wl,-Map,$EXE.map", "")
p.write_text(s)
PY

make -j"$(nproc)" \
  ARCH="$BB_ARCH" \
  CROSS_COMPILE="" \
  CC="zig cc -target ${ZIG_TARGET}" \
  LD="zig cc -target ${ZIG_TARGET}" \
  busybox_unstripped >/dev/null

mkdir -p "$(dirname "$OUT_BIN")"
if command -v llvm-strip >/dev/null 2>&1; then
  llvm-strip -s -o "$OUT_BIN" busybox_unstripped
else
  cp -a busybox_unstripped "$OUT_BIN"
fi
chmod +x "$OUT_BIN"

popd >/dev/null

file_out="$(file "$OUT_BIN")"
echo "$file_out"
echo "$file_out" | grep -q "$BB_EXPECT" || die "built busybox is not $TARGET_ARCH ($BB_EXPECT)"
echo "built busybox: $OUT_BIN"
