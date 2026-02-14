#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd sha256sum

load_config "$CFG"
mkdir -p "$OUT_DIR"

IMG="$OUT_DIR/prp-${TARGET_NAME}-recovery.img"
[[ -f "$IMG" ]] || die "recovery image not found: $IMG (run 'make bootimg' first)"

echo "flashing $IMG -> $RECOVERY_BLOCK"
$ADB_PREFIX devices -l
$ADB_PREFIX push "$IMG" /tmp/prp-recovery.img
$ADB_PREFIX shell "dd if=/tmp/prp-recovery.img of=$RECOVERY_BLOCK bs=4M conv=fsync && sync && rm -f /tmp/prp-recovery.img"

# Verify flash by reading back same byte size and hashing.
SIZE="$(stat -c %s "$IMG")"
BLOCKS="$(( (SIZE + 4095) / 4096 ))"
READBACK="$OUT_DIR/recovery-readback-$(date +%Y%m%d-%H%M%S).img"
$ADB_PREFIX exec-out "dd if=$RECOVERY_BLOCK bs=4096 count=$BLOCKS 2>/dev/null" > "$READBACK"
truncate -s "$SIZE" "$READBACK"

echo "sha256:"
sha256sum "$IMG" "$READBACK"
cmp -s "$IMG" "$READBACK" && echo "verify: OK" || die "verify failed"
