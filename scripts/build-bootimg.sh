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
require_cmd python3

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

# Normalize v0 header fields to match the format produced by Peacock's
# internal bootimg packer (which lk2nd paths are already validated against):
# - Keep second_addr = base + second_offset even when second_size==0.
# - Zero out boot ID words.
# Some boot paths are picky about these legacy fields.
python3 - "$OUT_IMG" "$BASE" "$SECOND_OFFSET" <<'PY'
import struct
import sys

path = sys.argv[1]
base = int(sys.argv[2], 0)
second_off = int(sys.argv[3], 0)
second_addr = (base + second_off) & 0xFFFFFFFF

with open(path, "r+b") as f:
    hdr = bytearray(f.read(608))
    if len(hdr) < 608 or hdr[:8] != b"ANDROID!":
        raise SystemExit(f"invalid boot image header: {path}")

    # boot_img_hdr.second_addr
    struct.pack_into("<I", hdr, 28, second_addr)
    # boot_img_hdr.id[8]
    hdr[576:608] = b"\x00" * 32

    f.seek(0)
    f.write(hdr)
PY

# Samsung aboot expects this footer on many legacy boot/recovery images.
if [[ "${APPEND_SEANDROIDENFORCE:-1}" == "1" ]]; then
  printf '%s' 'SEANDROIDENFORCE' >> "$OUT_IMG"
fi

echo "bootimg: $OUT_IMG"
ls -lh "$OUT_IMG"
sha256sum "$OUT_IMG"
