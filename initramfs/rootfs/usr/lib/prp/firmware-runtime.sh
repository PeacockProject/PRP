#!/bin/sh
# Shared PRP helpers for vendor firmware that is too large to embed.
#
# Some blobs cannot ride in the initramfs: on ginkgo the modem/ADSP PAS images
# are ~72 MiB against a 64 MiB recovery partition. They are already on the phone
# though, in a stock partition PRP never writes to, so instead of shipping them
# we mount that partition read-only and point the kernel's firmware loader at it
# with firmware_class.path. The kernel searches that path FIRST and falls back to
# /lib/firmware, so the small baked-in blobs (ath10k board data, the Novatek
# touch firmware) keep resolving normally.
#
# This is the "init harness" the ginkgo device tree refers to:
#
#     &remoteproc_mpss {
#         /* Loaded from the device's own NON-HLOS partition, which the init
#            harness mounts and exposes via firmware_class.path ... Bare name so
#            it resolves under that path. */
#         firmware-name = "modem.mdt";
#     };
#
# Everything here is FAIL-SOFT. A wiped or missing vendor partition must cost
# the device its modem, not its recovery environment.
#
# Knobs (from /etc/prp/firmware.conf, templated out of the device profile):
#   FW_RUNTIME_PART      GPT name of the partition holding the blobs (e.g. modem)
#   FW_RUNTIME_SUBDIR    subdirectory within it (e.g. image); "" = partition root
#   FW_RUNTIME_FSTYPE    filesystem to try first (default: vfat)
#   FW_RUNTIME_RPROC_FW  firmware name of a remoteproc to start once the path is
#                        live; "" starts nothing

BB=${BB:-/sbin/busybox}
if [ ! -x "$BB" ]; then
  BB=busybox
fi

FW_RUNTIME_MNT=${FW_RUNTIME_MNT:-/mnt/prp_firmware}
FW_CLASS_PATH_NODE=${FW_CLASS_PATH_NODE:-/sys/module/firmware_class/parameters/path}
PRP_RPROC_CLASS=${PRP_RPROC_CLASS:-/sys/class/remoteproc}

if ! command -v log >/dev/null 2>&1; then
  log() {
    printf '%s\n' "$*"
  }
fi

# busybox ships a `sfdisk` symlink that does not implement `-d`, so it cannot
# read GPT partition NAMES. Find the real util-linux one, exactly as prp-targets
# does. Returns 1 when only the busybox stub is available.
prp_fw_find_sfdisk() {
  local c=""
  for c in /sbin/sfdisk /usr/sbin/sfdisk /bin/sfdisk \
    /mnt/prp_rootfs/sbin/sfdisk /mnt/prp_rootfs/usr/sbin/sfdisk; do
    [ -x "$c" ] || continue
    case "$($BB readlink -f "$c" 2>/dev/null)" in *busybox*) continue ;; esac
    echo "$c"
    return 0
  done
  return 1
}

# Resolve a GPT partition name to a block device. by-name symlinks are tried
# first (cheap, and present on some kernels); the GPT scan is the fallback since
# PRP has no udev to create them.
prp_fw_part_by_name() {
  local want="$1"
  local dir=""
  local sfd=""
  local disk=""
  local node=""

  [ -n "$want" ] || return 1

  for dir in /dev/block/bootdevice/by-name /dev/block/by-name /dev/block/platform/*/by-name; do
    [ -e "$dir/$want" ] || continue
    node="$($BB readlink -f "$dir/$want" 2>/dev/null)"
    [ -b "$node" ] && { echo "$node"; return 0; }
  done

  sfd="$(prp_fw_find_sfdisk)" || return 1
  for disk in /dev/mmcblk0 /dev/sda /dev/mmcblk1; do
    [ -b "$disk" ] || continue
    node="$("$sfd" -d "$disk" 2>/dev/null | $BB awk -v w="$want" '
      $0 ~ "name=\"" w "\"" { print $1; exit }
    ')"
    node="${node%,}"
    node="${node%:}"
    [ -n "$node" ] && [ -b "$node" ] && { echo "$node"; return 0; }
  done
  return 1
}

# Mount the vendor firmware partition read-only and hand its directory to the
# kernel firmware loader. Idempotent: a second call re-points the path and
# returns success without remounting.
#
# Returns 0 when the firmware path is live, 1 otherwise (never fatal).
prp_firmware_runtime_setup() {
  local part="${FW_RUNTIME_PART:-}"
  local subdir="${FW_RUNTIME_SUBDIR:-}"
  local fstype="${FW_RUNTIME_FSTYPE:-vfat}"
  local node=""
  local dir=""

  [ -n "$part" ] || return 1

  if [ ! -e "$FW_CLASS_PATH_NODE" ]; then
    log "firmware: kernel has no firmware_class path parameter; skipping"
    return 1
  fi

  if $BB mountpoint -q "$FW_RUNTIME_MNT" 2>/dev/null || \
     $BB grep -q " $FW_RUNTIME_MNT " /proc/mounts 2>/dev/null; then
    dir="$FW_RUNTIME_MNT"
    [ -n "$subdir" ] && [ -d "$FW_RUNTIME_MNT/$subdir" ] && dir="$FW_RUNTIME_MNT/$subdir"
    echo "$dir" >"$FW_CLASS_PATH_NODE" 2>/dev/null || true
    return 0
  fi

  node="$(prp_fw_part_by_name "$part" 2>/dev/null || true)"
  if [ -z "$node" ]; then
    log "firmware: no '$part' partition found; large vendor blobs unavailable"
    return 1
  fi

  $BB mkdir -p "$FW_RUNTIME_MNT" 2>/dev/null || true
  if ! $BB mount -t "$fstype" -o ro "$node" "$FW_RUNTIME_MNT" 2>/dev/null; then
    # Not every device formats this partition the same way; let the kernel pick.
    if ! $BB mount -o ro "$node" "$FW_RUNTIME_MNT" 2>/dev/null; then
      log "firmware: could not mount $node ($part) read-only"
      return 1
    fi
  fi

  dir="$FW_RUNTIME_MNT"
  if [ -n "$subdir" ]; then
    if [ -d "$FW_RUNTIME_MNT/$subdir" ]; then
      dir="$FW_RUNTIME_MNT/$subdir"
    else
      log "firmware: $part mounted but has no '$subdir'; using its root"
    fi
  fi

  if echo "$dir" >"$FW_CLASS_PATH_NODE" 2>/dev/null; then
    log "firmware: $part ($node) -> firmware_class.path=$dir"
    return 0
  fi

  log "firmware: could not set firmware_class.path (kernel refused)"
  return 1
}

# Start a remoteproc by the firmware file it was configured with — matching on
# the `firmware` attribute rather than the rproc name, because the name is the
# platform device address and tells you nothing about which core it is.
#
# Used for cores whose PAS descriptor sets auto_boot=false (ginkgo's MPSS), so
# nothing is requested from the firmware loader until the path above is live.
prp_rproc_start_by_firmware() {
  local want="$1"
  local timeout="${2:-30}"
  local rp=""
  local state=""
  local i=0

  [ -n "$want" ] || return 1
  [ -d "$PRP_RPROC_CLASS" ] || return 1

  for rp in "$PRP_RPROC_CLASS"/remoteproc*; do
    [ -r "$rp/firmware" ] || continue
    [ "$($BB cat "$rp/firmware" 2>/dev/null)" = "$want" ] || continue

    state="$($BB cat "$rp/state" 2>/dev/null || echo unknown)"
    if [ "$state" = "running" ]; then
      return 0
    fi

    if ! echo start >"$rp/state" 2>/dev/null; then
      log "firmware: remoteproc $want refused start (state=$state)"
      return 1
    fi

    i=0
    while [ "$i" -lt "$timeout" ]; do
      [ "$($BB cat "$rp/state" 2>/dev/null)" = "running" ] && {
        log "firmware: remoteproc $want running"
        return 0
      }
      i=$((i + 1))
      $BB sleep 1
    done
    log "firmware: remoteproc $want did not reach running within ${timeout}s"
    return 1
  done

  return 1
}
