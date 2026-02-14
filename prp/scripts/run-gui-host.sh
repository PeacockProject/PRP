#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

load_config "$CFG"
mkdir -p "$OUT_DIR"

./scripts/build-gui-host.sh "$CFG" "$OUT_DIR"

BIN="$OUT_DIR/tools/gui-out/host/prp-gui-host"
if [[ ! -x "$BIN" ]]; then
  die "missing host gui binary: $BIN"
fi

echo "run: $BIN"
echo "  tip: PRP_GUI_SCALE=140 $BIN"
echo "  tip: PRP_GUI_SDL_HOR_RES=1080 PRP_GUI_SDL_VER_RES=1920 make -C prp gui-host TARGET=jflte"

exec "$BIN"
