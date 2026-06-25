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

case "$FLASH_METHOD" in
  adb-dd)
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
    ;;
  fastboot-bootimg)
    require_cmd fastboot
    read -r -a FASTBOOT_CMD <<<"${FASTBOOT_PREFIX:-fastboot}"
    [[ "${#FASTBOOT_CMD[@]}" -gt 0 ]] || die "FASTBOOT_PREFIX resolved to empty command"
    fastboot_getvar() {
      local key="$1"
      "${FASTBOOT_CMD[@]}" getvar "$key" 2>&1 || true
    }
    "${FASTBOOT_CMD[@]}" devices
    if [[ "${FASTBOOT_AB_LK2ND_SPLIT_BOOT:-0}" == "1" ]]; then
      BOOT_PART="${FASTBOOT_BOOT_PARTITION:-boot}"
      LK2ND_PART="${FASTBOOT_LK2ND_PARTITION:-lk2nd}"
      echo "A/B lk2nd split-boot mode enabled (boot=$BOOT_PART lk2nd=$LK2ND_PART)"

      PARTINFO="$(fastboot_getvar "partition-size:${LK2ND_PART}")"
      if ! grep -qi "partition-size:${LK2ND_PART}:[[:space:]]*0x" <<<"$PARTINFO"; then
        echo "$PARTINFO" >&2
        die "lk2nd partition not visible via fastboot (need lk2nd fastboot for split-boot mode)"
      fi

      if [[ -n "${FASTBOOT_SET_ACTIVE:-}" ]]; then
        echo "set active slot (pre-flash): ${FASTBOOT_SET_ACTIVE}"
        # lk2nd's fastboot doesn't implement set_active; it also already booted
        # from the active slot, so this is best-effort, not fatal.
        "${FASTBOOT_CMD[@]}" set_active "$FASTBOOT_SET_ACTIVE" || \
          echo "warning: set_active ${FASTBOOT_SET_ACTIVE} unsupported (lk2nd); continuing" >&2
      fi

      if [[ -n "${FASTBOOT_LK2ND_IMAGE:-}" ]]; then
        [[ -f "$FASTBOOT_LK2ND_IMAGE" ]] || die "FASTBOOT_LK2ND_IMAGE not found: $FASTBOOT_LK2ND_IMAGE"
        echo "flashing $FASTBOOT_LK2ND_IMAGE -> $LK2ND_PART"
        "${FASTBOOT_CMD[@]}" flash "$LK2ND_PART" "$FASTBOOT_LK2ND_IMAGE"
      fi

      echo "flashing $IMG -> $BOOT_PART"
      "${FASTBOOT_CMD[@]}" flash "$BOOT_PART" "$IMG"

      if [[ -n "${FASTBOOT_SET_ACTIVE:-}" ]]; then
        echo "set active slot (post-flash): ${FASTBOOT_SET_ACTIVE}"
        "${FASTBOOT_CMD[@]}" set_active "$FASTBOOT_SET_ACTIVE" || \
          echo "warning: set_active ${FASTBOOT_SET_ACTIVE} unsupported (lk2nd); continuing" >&2
      fi
    else
      for part in $FASTBOOT_PARTITIONS; do
        echo "flashing $IMG -> $part"
        "${FASTBOOT_CMD[@]}" flash "$part" "$IMG"
      done
      if [[ -n "${FASTBOOT_SET_ACTIVE:-}" ]]; then
        echo "set active slot: ${FASTBOOT_SET_ACTIVE}"
        if ! "${FASTBOOT_CMD[@]}" set_active "$FASTBOOT_SET_ACTIVE"; then
          echo "warning: fastboot set_active ${FASTBOOT_SET_ACTIVE} failed; continuing" >&2
        fi
      fi
    fi
    ;;
  *)
    die "unknown FLASH_METHOD: $FLASH_METHOD (supported: adb-dd, fastboot-bootimg)"
    ;;
esac
