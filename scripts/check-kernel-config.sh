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

CFG_PATH="${KERNEL_CONFIG/#\~/$HOME}"
[[ -f "$CFG_PATH" ]] || die "kernel config not found: $CFG_PATH"

required=(
  "CONFIG_EFI_PARTITION=y"
  "CONFIG_MSDOS_PARTITION=y"
  "CONFIG_EXT4_FS=y"
  "CONFIG_BLK_DEV_LOOP=y"
  "CONFIG_BLK_DEV_DM=y"
  "CONFIG_BLK_DEV_DM_BUILTIN=y"
  "CONFIG_DEVTMPFS=y"
)

recommended=(
  "CONFIG_DEVTMPFS_MOUNT=y"
  "CONFIG_DM_UEVENT=y"
)

missing=0
echo "Kernel config: $CFG_PATH"
for opt in "${required[@]}"; do
  if grep -q "^$opt$" "$CFG_PATH"; then
    echo "  OK  $opt"
  else
    echo "  ERR $opt"
    missing=1
  fi
done

for opt in "${recommended[@]}"; do
  if grep -q "^$opt$" "$CFG_PATH"; then
    echo "  OK  $opt (recommended)"
  else
    echo "  WARN missing recommended: $opt"
  fi
done

if [[ "$missing" -ne 0 ]]; then
  die "required kernel options missing"
fi

echo "kernel config check: PASS"
