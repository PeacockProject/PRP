#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd mkbootimg
require_cmd sha256sum

load_config "$CFG"
mkdir -p "$OUT_DIR"

KERNEL_PATH="$(resolve_kernel_image)"
RAMDISK_PATH="$(resolve_ramdisk_image "$OUT_DIR")"
OUT_IMG="$OUT_DIR/prp-${TARGET_NAME}-recovery.img"

args=(
  --header_version "$HEADER_VERSION"
  --kernel "$KERNEL_PATH"
  --ramdisk "$RAMDISK_PATH"
  --cmdline "$CMDLINE"
  --base "$BASE"
  --kernel_offset "$KERNEL_OFFSET"
  --ramdisk_offset "$RAMDISK_OFFSET"
  --second_offset "$SECOND_OFFSET"
  --tags_offset "$TAGS_OFFSET"
  --pagesize "$PAGESIZE"
  --board "$BOARD_NAME"
  --output "$OUT_IMG"
)

if [[ -n "${DTB_IMAGE:-}" && -f "$DTB_IMAGE" ]]; then
  args+=(--dtb "$DTB_IMAGE")
fi

mkbootimg "${args[@]}"

# Samsung aboot expects this footer on many legacy boot/recovery images.
if [[ "${APPEND_SEANDROIDENFORCE:-1}" == "1" ]]; then
  printf '%s' 'SEANDROIDENFORCE' >> "$OUT_IMG"
fi

echo "bootimg: $OUT_IMG"
ls -lh "$OUT_IMG"
sha256sum "$OUT_IMG"
