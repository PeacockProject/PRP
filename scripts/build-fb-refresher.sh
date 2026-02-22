#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 3 ]] || die "usage: $0 <config.env> <out_dir> <dest_bin>"
CFG="$1"
OUT_DIR="$2"
DEST_BIN="$3"

require_cmd zig
load_config "$CFG"
mkdir -p "$OUT_DIR"

SRC=""
for cand in \
  "$PRP_ROOT/../peacock-ports/device/msm-fb-refresher/refresher.c" \
  "$PRP_ROOT/peacock-ports/device/msm-fb-refresher/refresher.c"; do
  if [[ -f "$cand" ]]; then
    SRC="$cand"
    break
  fi
done
[[ -n "$SRC" ]] || die "msm-fb-refresher source not found"

mkdir -p "$(dirname "$DEST_BIN")"
zig cc -target "$ZIG_TARGET" -static -Os -s -o "$DEST_BIN" "$SRC"
chmod +x "$DEST_BIN"
echo "built fb refresher: $DEST_BIN"

