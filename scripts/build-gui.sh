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
require_cmd zig
require_cmd find
require_cmd python3

load_config "$CFG"
mkdir -p "$OUT_DIR"

LVGL_TAG="${LVGL_TAG:-v8.3.11}"
LVGL_URL="https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/${LVGL_TAG}"
LVD_REF="${LVD_REF:-release/v8.3}"
LVD_URL="https://codeload.github.com/lvgl/lv_drivers/tar.gz/refs/heads/${LVD_REF}"

TOOLS_DIR="$OUT_DIR/tools"
SRC_DIR="$TOOLS_DIR/gui-src"
BUILD_DIR="$TOOLS_DIR/gui-build/${TARGET_ARCH}"
OUT_BIN_DIR="$TOOLS_DIR/gui-out/${TARGET_ARCH}"

mkdir -p "$SRC_DIR" "$BUILD_DIR" "$OUT_BIN_DIR"

LVGL_TARBALL="$SRC_DIR/lvgl-${LVGL_TAG}.tar.gz"
LVD_TARBALL="$SRC_DIR/lv_drivers-${LVD_REF//\//-}.tar.gz"

LVGL_DIR="$SRC_DIR/lvgl-${LVGL_TAG#v}"
LVD_DIR="$SRC_DIR/lv_drivers-${LVD_REF//\//-}"

if [[ ! -f "$LVGL_TARBALL" ]]; then
  echo "gui: downloading lvgl ${LVGL_TAG}"
  curl -L --fail -o "$LVGL_TARBALL" "$LVGL_URL"
fi
if [[ ! -d "$LVGL_DIR" ]]; then
  tar -xzf "$LVGL_TARBALL" -C "$SRC_DIR"
fi

if [[ ! -f "$LVD_TARBALL" ]]; then
  echo "gui: downloading lv_drivers"
  curl -L --fail -o "$LVD_TARBALL" "$LVD_URL"
fi
if [[ ! -d "$LVD_DIR" ]]; then
  tar -xzf "$LVD_TARBALL" -C "$SRC_DIR"
  # GitHub tar extracts to lv_drivers-<ref>/
  if [[ -d "$SRC_DIR/lv_drivers-${LVD_REF//\//-}" ]]; then
    : # ok
  else
    # best-effort: pick the only lv_drivers-* directory
    tmp_dir="$(find "$SRC_DIR" -maxdepth 1 -type d -name 'lv_drivers-*' | head -n 1 || true)"
    [[ -n "$tmp_dir" ]] && mv -f "$tmp_dir" "$LVD_DIR" 2>/dev/null || true
  fi
fi

# lv_drivers v8.3 evdev has an ABS_MT_TRACKING_ID handling bug:
# it only marks press when tracking_id == 0. Real devices often use non-zero IDs.
# Patch it to treat any non-negative tracking_id as pressed.
EVDEV_SRC="$LVD_DIR/indev/evdev.c"
if [[ -f "$EVDEV_SRC" ]]; then
  python3 - "$EVDEV_SRC" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, "r", encoding="utf-8").read()
s2 = re.sub(r"else if\(in\.value == 0\)\s*evdev_button = LV_INDEV_STATE_PR;",
            "else if(in.value != -1)\n                                    evdev_button = LV_INDEV_STATE_PR;",
            s, count=1)
if s2 != s:
    open(p, "w", encoding="utf-8").write(s2)
PY
fi

OUT_BIN="$OUT_BIN_DIR/prp-gui"
if [[ -f "$OUT_BIN" \
  && "$OUT_BIN" -nt "$SCRIPT_DIR/build-gui.sh" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_gui.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_ui.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_ui.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_wizard.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_wizard.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_net_ui.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_net_ui.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_theme.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_fbdev.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_fbdev.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_logo.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_logo.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/lv_conf.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/lv_drv_conf.h" \
  ]]; then
  echo "gui: using cached $OUT_BIN"
  ls -lh "$OUT_BIN"
  exit 0
fi

echo "gui: building prp-gui (static ${TARGET_ARCH} ${ZIG_TARGET})"

# Canonicalize for symlinks (so the include tree doesn't end up with broken relative links).
LVGL_ABS="$(cd "$LVGL_DIR" && pwd)"
LVD_ABS="$(cd "$LVD_DIR" && pwd)"

# Build-time include layout so third-party code can include "lvgl/lvgl.h" and "lv_drivers/...".
# lvgl.h uses relative includes like "src/...", so we also link the "src" tree next to it.
INC_ROOT="$BUILD_DIR/include"
rm -rf "$INC_ROOT"
mkdir -p "$INC_ROOT/lvgl"
ln -snf "$LVGL_ABS/lvgl.h" "$INC_ROOT/lvgl/lvgl.h"
ln -snf "$LVGL_ABS/src" "$INC_ROOT/lvgl/src"
ln -snf "$LVD_ABS" "$INC_ROOT/lv_drivers"

# Use PRP's minimal configs.
cp -a "$PRP_ROOT/gui/lv_conf.h" "$INC_ROOT/lv_conf.h"
cp -a "$PRP_ROOT/gui/lv_drv_conf.h" "$INC_ROOT/lv_drv_conf.h"

# Read the source list via a temp file rather than process substitution
# (`< <(...)`): process substitution needs /dev/fd, which isn't present in the
# Peacock build chroot, so it fails with "/dev/fd/NN: No such file or directory".
_lvgl_src_list="$(mktemp)"
find "$LVGL_DIR/src" -type f -name '*.c' | sort > "$_lvgl_src_list"
mapfile -t LVGL_SRCS < "$_lvgl_src_list"
rm -f "$_lvgl_src_list"
[[ "${#LVGL_SRCS[@]}" -gt 0 ]] || die "lvgl sources not found under $LVGL_DIR/src"

SRCS=(
  "${LVGL_SRCS[@]}"
  "$LVD_DIR/indev/evdev.c"
  "$PRP_ROOT/gui/prp_fbdev.c"
  "$PRP_ROOT/gui/prp_logo.c"
  "$PRP_ROOT/gui/fonts/pk_serif_30.c"
  "$PRP_ROOT/gui/fonts/pk_serif_44.c"
  "$PRP_ROOT/gui/fonts/pk_mono_16.c"
  "$PRP_ROOT/gui/fonts/pk_mono_20.c"
  "$PRP_ROOT/gui/fonts/pk_mono_26.c"
  "$PRP_ROOT/gui/prp_ui.c"
  "$PRP_ROOT/gui/prp_wizard.c"
  "$PRP_ROOT/gui/prp_net_ui.c"
  "$PRP_ROOT/gui/prp_gui.c"
)

INCS=(
  "-I$INC_ROOT"
  "-I$INC_ROOT/lv_drivers"
  "-I$INC_ROOT/lv_drivers/display"
  "-I$INC_ROOT/lv_drivers/indev"
)

zig cc -target "$ZIG_TARGET" -static -Os -ffunction-sections -fdata-sections \
  -Wl,--gc-sections -std=c99 -D_GNU_SOURCE \
  -DLV_CONF_INCLUDE_SIMPLE=1 \
  "${INCS[@]}" \
  "${SRCS[@]}" \
  -lm -lpthread \
  -o "$OUT_BIN"

if command -v llvm-strip >/dev/null 2>&1; then
  llvm-strip "$OUT_BIN" 2>/dev/null || true
elif command -v strip >/dev/null 2>&1; then
  strip "$OUT_BIN" 2>/dev/null || true
fi

echo "gui: built $OUT_BIN"
ls -lh "$OUT_BIN"
