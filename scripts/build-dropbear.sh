#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd curl
require_cmd tar
require_cmd make
require_cmd zig

load_config "$CFG"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

DROPBEAR_VER="${DROPBEAR_VER:-2024.86}"
DROPBEAR_STRIP="${DROPBEAR_STRIP:-0}"
TARBALL_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2"

TOOLS_DIR="$OUT_DIR/tools"
SRC_ROOT="$TOOLS_DIR/dropbear-src"
BUILD_ROOT="$TOOLS_DIR/dropbear-build"
OUT_ROOT="$TOOLS_DIR/dropbear-out"

TARBALL="$SRC_ROOT/dropbear-${DROPBEAR_VER}.tar.bz2"
SRC_DIR="$SRC_ROOT/dropbear-${DROPBEAR_VER}"
BUILD_DIR="$BUILD_ROOT/dropbear-${DROPBEAR_VER}-${TARGET_ARCH}"
BIN_DIR="$OUT_ROOT/${TARGET_ARCH}"

mkdir -p "$SRC_ROOT" "$BUILD_ROOT" "$OUT_ROOT" "$BIN_DIR"

if [[ ! -f "$TARBALL" ]]; then
  echo "dropbear: downloading $TARBALL_URL"
  curl -L --fail -o "$TARBALL" "$TARBALL_URL"
fi

if [[ ! -d "$SRC_DIR" ]]; then
  echo "dropbear: extracting source"
  (cd "$SRC_ROOT" && tar -xjf "$TARBALL")
fi

# Dropbear builds in-tree. Always do a clean rebuild to avoid stale/cross-target outputs.
rm -rf "$BUILD_DIR"
cp -a "$SRC_DIR" "$BUILD_DIR"
rm -f "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp"

echo "dropbear: building static ${TARGET_ARCH} (${ZIG_TARGET}) in $BUILD_DIR"
(
  cd "$BUILD_DIR"

  export CC="zig cc -target ${ZIG_TARGET}"
  export AR="zig ar"
  export RANLIB="zig ranlib"
  # Conservative flags to avoid optimizer/strip-induced instability on target.
  export CFLAGS="-O2 -fno-omit-frame-pointer"
  export LDFLAGS="-static"

  ./configure \
    --host="${DROPBEAR_HOST}" \
    --enable-static \
    --disable-harden \
    --disable-zlib \
    --disable-syslog \
    --disable-shadow \
    --disable-lastlog \
    --disable-utmp \
    --disable-utmpx \
    --disable-wtmp \
    --disable-wtmpx

  # Include the standalone scp (from OpenSSH) so host-side scp works.
  make -j"$(nproc)" PROGRAMS="dropbear dropbearkey dbclient scp"

  cp -a dropbear dropbearkey dbclient scp "$BIN_DIR/"
)

if [[ "$DROPBEAR_STRIP" = "1" ]]; then
  if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp" 2>/dev/null || true
  elif command -v strip >/dev/null 2>&1; then
    strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp" 2>/dev/null || true
  fi
fi

# Basic host-side smoke test under qemu-user when available.
qemu_cmd=""
case "$TARGET_ARCH" in
  aarch64)
    command -v qemu-aarch64 >/dev/null 2>&1 && qemu_cmd="qemu-aarch64"
    ;;
  armv7|armv7h|armhf)
    command -v qemu-arm >/dev/null 2>&1 && qemu_cmd="qemu-arm"
    ;;
esac
if [[ -n "$qemu_cmd" ]]; then
  "$qemu_cmd" "$BIN_DIR/dropbear" -V >/dev/null
  "$qemu_cmd" "$BIN_DIR/dropbearkey" -h >/dev/null
fi

echo "dropbear: outputs"
ls -lh "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/dbclient" "$BIN_DIR/scp"
