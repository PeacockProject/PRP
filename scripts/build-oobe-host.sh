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
require_cmd pkg-config

load_config "$CFG"
mkdir -p "$OUT_DIR"

if ! pkg-config --exists sdl2; then
  die "SDL2 dev package missing. Install SDL2 headers/libs (pkg-config name: sdl2)."
fi

LVGL_TAG="${LVGL_TAG:-v8.3.11}"
LVGL_URL="https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/${LVGL_TAG}"
LVD_REF="${LVD_REF:-release/v8.3}"
LVD_URL="https://codeload.github.com/lvgl/lv_drivers/tar.gz/refs/heads/${LVD_REF}"

TOOLS_DIR="$OUT_DIR/tools"
SRC_DIR="$TOOLS_DIR/gui-src"
BUILD_DIR="$TOOLS_DIR/gui-build/host"
OUT_BIN_DIR="$TOOLS_DIR/gui-out/host"

mkdir -p "$SRC_DIR" "$BUILD_DIR" "$OUT_BIN_DIR"

LVGL_TARBALL="$SRC_DIR/lvgl-${LVGL_TAG}.tar.gz"
LVD_TARBALL="$SRC_DIR/lv_drivers-${LVD_REF//\//-}.tar.gz"

LVGL_DIR="$SRC_DIR/lvgl-${LVGL_TAG#v}"
LVD_DIR="$SRC_DIR/lv_drivers-${LVD_REF//\//-}"

if [[ ! -f "$LVGL_TARBALL" ]]; then
  echo "gui-host: downloading lvgl ${LVGL_TAG}"
  curl -L --fail -o "$LVGL_TARBALL" "$LVGL_URL"
fi
if [[ ! -d "$LVGL_DIR" ]]; then
  tar -xzf "$LVGL_TARBALL" -C "$SRC_DIR"
fi

if [[ ! -f "$LVD_TARBALL" ]]; then
  echo "gui-host: downloading lv_drivers"
  curl -L --fail -o "$LVD_TARBALL" "$LVD_URL"
fi
if [[ ! -d "$LVD_DIR" ]]; then
  tar -xzf "$LVD_TARBALL" -C "$SRC_DIR"
  if [[ -d "$SRC_DIR/lv_drivers-${LVD_REF//\//-}" ]]; then
    : # ok
  else
    tmp_dir="$(find "$SRC_DIR" -maxdepth 1 -type d -name 'lv_drivers-*' | head -n 1 || true)"
    [[ -n "$tmp_dir" ]] && mv -f "$tmp_dir" "$LVD_DIR" 2>/dev/null || true
  fi
fi

OUT_BIN="$OUT_BIN_DIR/oobe-gui-host"
if false && [[ -f "$OUT_BIN" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_gui_sdl.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_ui.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_ui.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_wizard.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_wizard.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_net_ui.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_net_ui.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_theme.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/lv_conf.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_logo.c" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/prp_logo.h" \
  && "$OUT_BIN" -nt "$PRP_ROOT/gui/lv_conf.h" \
  ]]; then
  echo "gui-host: using cached $OUT_BIN"
  ls -lh "$OUT_BIN"
  exit 0
fi

echo "gui-host: building PRP GUI simulator (SDL2)"

LVGL_ABS="$(cd "$LVGL_DIR" && pwd)"
LVD_ABS="$(cd "$LVD_DIR" && pwd)"

INC_ROOT="$BUILD_DIR/include"
rm -rf "$INC_ROOT"
mkdir -p "$INC_ROOT/lvgl"
ln -snf "$LVGL_ABS/lvgl.h" "$INC_ROOT/lvgl/lvgl.h"
ln -snf "$LVGL_ABS/src" "$INC_ROOT/lvgl/src"
ln -snf "$LVD_ABS" "$INC_ROOT/lv_drivers"

cp -a "$PRP_ROOT/gui/lv_conf.h" "$INC_ROOT/lv_conf.h"

SDL_HOR_RES="${PRP_GUI_SDL_HOR_RES:-540}"
SDL_VER_RES="${PRP_GUI_SDL_VER_RES:-960}"
SDL_ZOOM="${PRP_GUI_SDL_ZOOM:-1}"

cat > "$INC_ROOT/lv_drv_conf.h" <<EOF
/* Generated for PRP GUI host simulator (SDL2) */
#pragma once

/* Disable non-SDL backends */
#define USE_FBDEV 0
#define USE_EVDEV 0
#define USE_DRM 0
#define USE_SDL 1
#define USE_MONITOR 0
#define USE_SDL_GPU 0

/* SDL window config */
#define SDL_HOR_RES ${SDL_HOR_RES}
#define SDL_VER_RES ${SDL_VER_RES}
#define SDL_ZOOM ${SDL_ZOOM}
#define SDL_DOUBLE_BUFFERED 0
#define SDL_INCLUDE_PATH <SDL2/SDL.h>
EOF

mapfile -t LVGL_SRCS < <(find "$LVGL_DIR/src" -type f -name '*.c' | sort)
[[ "${#LVGL_SRCS[@]}" -gt 0 ]] || die "lvgl sources not found under $LVGL_DIR/src"

SRCS=(
  "${LVGL_SRCS[@]}"
  "$LVD_DIR/sdl/sdl.c"
  "$LVD_DIR/sdl/sdl_common.c"
  "$PRP_ROOT/gui/prp_logo.c"
  "$PRP_ROOT/gui/fonts/pk_serif_30.c"
  "$PRP_ROOT/gui/fonts/pk_serif_44.c"
  "$PRP_ROOT/gui/fonts/pk_mono_16.c"
  "$PRP_ROOT/gui/fonts/pk_mono_20.c"
  "$PRP_ROOT/gui/blueprint.c"
  "$PRP_ROOT/gui/toml.c"
  "$PRP_ROOT/gui/bp_verify.c"
  "$PRP_ROOT/gui/tweetnacl.c"
  "$PRP_ROOT/gui/oobe_wizard.c"
  "$PRP_ROOT/gui/oobe_sdl.c"
)

INCS=(
  "-I$INC_ROOT"
  "-I$INC_ROOT/lv_drivers"
  "-I$INC_ROOT/lv_drivers/sdl"
)

# SDL2 flags from the host system.
SDL_CFLAGS="$(pkg-config --cflags sdl2)"
SDL_LIBS="$(pkg-config --libs sdl2)"

zig cc ${HOST_GUI_OPT:--O2} -std=c99 -D_GNU_SOURCE \
  -DLV_CONF_INCLUDE_SIMPLE=1 \
  "${INCS[@]}" \
  $SDL_CFLAGS \
  "${SRCS[@]}" \
  -lm -lpthread \
  $SDL_LIBS \
  -o "$OUT_BIN"

echo "gui-host: built $OUT_BIN"
ls -lh "$OUT_BIN"
