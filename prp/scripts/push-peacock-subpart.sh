#!/usr/bin/env bash
set -euo pipefail

PART_INDEX="${1:-}"
IMG="${2:-$HOME/.local/var/peacock/samsung-jflte.img}"
TARGET="${PRP_SSH_TARGET:-root@172.16.42.1}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/tmp/prp_known_hosts)
SECTOR_SIZE=512

if [[ -z "$PART_INDEX" ]]; then
  echo "usage: $0 <partition-index-in-image-gpt> [image-path]" >&2
  echo "example: $0 1 ~/.local/var/peacock/samsung-jflte.img" >&2
  exit 1
fi

if [[ ! "$PART_INDEX" =~ ^[0-9]+$ ]] || (( PART_INDEX < 1 )); then
  echo "invalid partition index: $PART_INDEX" >&2
  exit 1
fi

if [[ ! -f "$IMG" ]]; then
  echo "image not found: $IMG" >&2
  exit 1
fi

if ! command -v sfdisk >/dev/null 2>&1; then
  echo "sfdisk is required on host" >&2
  exit 1
fi

mapfile -t PART_INFO < <(
  sfdisk --json "$IMG" | awk -v idx="$PART_INDEX" '
    /"partitions"/ { inparts=1; next }
    inparts && /"start":/ {
      gsub(/[^0-9]/, "", $0)
      start=$0
    }
    inparts && /"size":/ && start {
      gsub(/[^0-9]/, "", $0)
      size=$0
      part++
      if (part == idx) {
        print start
        print size
        exit
      }
      start=""
      size=""
    }
  '
)

if [[ "${#PART_INFO[@]}" -lt 2 ]]; then
  echo "failed to parse p${PART_INDEX} start/size from image GPT: $IMG" >&2
  exit 1
fi

PART_START="${PART_INFO[0]}"
PART_SIZE="${PART_INFO[1]}"

echo "Image: $IMG"
echo "p${PART_INDEX} start sectors: $PART_START"
echo "p${PART_INDEX} size sectors : $PART_SIZE"
echo "Target                : $TARGET"

ssh "${SSH_OPTS[@]}" "$TARGET" '
set -e
for c in /dev/block/by-name/userdata /dev/block/platform/*/by-name/userdata; do
  [ -e "$c" ] || continue
  d="$(readlink -f "$c" 2>/dev/null || true)"
  [ -b "$d" ] && { echo "$d"; exit 0; }
done
[ -b /dev/mmcblk0p29 ] && { echo /dev/mmcblk0p29; exit 0; }
exit 1
' >/tmp/prp_userdata_dev.txt

USERDATA_DEV="$(cat /tmp/prp_userdata_dev.txt)"
rm -f /tmp/prp_userdata_dev.txt

echo "Resolved remote userdata device: $USERDATA_DEV"
echo "Preparing target (unmounting nested root/boot if mounted)..."
ssh "${SSH_OPTS[@]}" "$TARGET" '
set -e
for m in /mnt/prp_root /mnt/prp_boot; do
  if grep -q " $m " /proc/mounts 2>/dev/null; then
    umount "$m" || true
  fi
done
if command -v dmsetup >/dev/null 2>&1; then
  dmsetup remove prp_peacock_root 2>/dev/null || true
  dmsetup remove prp_peacock_boot 2>/dev/null || true
fi
sync
'

echo "Writing p${PART_INDEX} payload into nested image area..."

dd if="$IMG" bs="$SECTOR_SIZE" skip="$PART_START" count="$PART_SIZE" status=progress | \
  ssh "${SSH_OPTS[@]}" "$TARGET" \
  "dd of='$USERDATA_DEV' bs=$SECTOR_SIZE seek=$PART_START conv=fsync"

ssh "${SSH_OPTS[@]}" "$TARGET" "sync"
echo "Done."
