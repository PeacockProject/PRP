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

OUT_BACKUP="$OUT_DIR/recovery-backup-$(date +%Y%m%d-%H%M%S).img"

case "$FLASH_METHOD" in
  adb-dd)
    $ADB_PREFIX exec-out "dd if=$RECOVERY_BLOCK bs=1M 2>/dev/null" > "$OUT_BACKUP"
    ;;
  fastboot-bootimg)
    die "backup-recovery is not implemented for fastboot-bootimg targets"
    ;;
  *)
    die "unknown FLASH_METHOD: $FLASH_METHOD"
    ;;
esac

echo "backup: $OUT_BACKUP"
ls -lh "$OUT_BACKUP"
sha256sum "$OUT_BACKUP"
