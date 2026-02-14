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

DROPBEAR_VER="${DROPBEAR_VER:-2024.86}"
TARBALL_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2"

TOOLS_DIR="$OUT_DIR/tools"
SRC_ROOT="$TOOLS_DIR/dropbear-src"
BUILD_ROOT="$TOOLS_DIR/dropbear-build"
OUT_ROOT="$TOOLS_DIR/dropbear-out"

TARBALL="$SRC_ROOT/dropbear-${DROPBEAR_VER}.tar.bz2"
SRC_DIR="$SRC_ROOT/dropbear-${DROPBEAR_VER}"
BUILD_DIR="$BUILD_ROOT/dropbear-${DROPBEAR_VER}-armv7"
BIN_DIR="$OUT_ROOT/armv7"

mkdir -p "$SRC_ROOT" "$BUILD_ROOT" "$OUT_ROOT" "$BIN_DIR"

if [[ ! -f "$TARBALL" ]]; then
  echo "dropbear: downloading $TARBALL_URL"
  curl -L --fail -o "$TARBALL" "$TARBALL_URL"
fi

if [[ ! -d "$SRC_DIR" ]]; then
  echo "dropbear: extracting source"
  (cd "$SRC_ROOT" && tar -xjf "$TARBALL")
fi

# Dropbear builds in-tree, so copy to a clean build dir each time we need to rebuild.
need_build=0
for f in dropbear dropbearkey scp; do
  [[ -f "$BIN_DIR/$f" ]] || need_build=1
done

if [[ "$need_build" -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
  cp -a "$SRC_DIR" "$BUILD_DIR"

  echo "dropbear: building static ARMv7 (musl) in $BUILD_DIR"
  (
    cd "$BUILD_DIR"

    export CC="zig cc -target arm-linux-musleabihf"
    export AR="zig ar"
    export RANLIB="zig ranlib"
    export CFLAGS="-Os -ffunction-sections -fdata-sections"
    export LDFLAGS="-static -Wl,--gc-sections"

    ./configure \
      --host=arm-linux-musleabihf \
      --enable-static \
      --disable-zlib \
      --disable-syslog \
      --disable-shadow \
      --disable-lastlog \
      --disable-utmp \
      --disable-utmpx \
      --disable-wtmp \
      --disable-wtmpx

    # Include the standalone scp (from OpenSSH) so host-side scp works.
    make -j"$(nproc)" PROGRAMS="dropbear dropbearkey scp"

    cp -a dropbear dropbearkey scp "$BIN_DIR/"
  )

  # Strip if possible (best-effort).
  if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/scp" 2>/dev/null || true
  elif command -v strip >/dev/null 2>&1; then
    strip "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/scp" 2>/dev/null || true
  fi
fi

echo "dropbear: outputs"
ls -lh "$BIN_DIR/dropbear" "$BIN_DIR/dropbearkey" "$BIN_DIR/scp"

