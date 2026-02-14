#!/usr/bin/env bash
set -euo pipefail

ROOTFS_DEFAULT="/home/tiel/.local/var/peacock/image-build-chroot/x86_64/rootfs"
ROOTFS="${PEACOCK_ROOTFS:-$ROOTFS_DEFAULT}"

MOUNTED_TARGETS=()

die() {
  echo "error: $*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage:
  qemu-arm-rootfs.sh setup [ROOTFS]
  qemu-arm-rootfs.sh status [ROOTFS]
  qemu-arm-rootfs.sh run [ROOTFS] "command"
  qemu-arm-rootfs.sh shell [ROOTFS]

Examples:
  ./tools/qemu-arm-rootfs.sh setup
  ./tools/qemu-arm-rootfs.sh run "uname -m; /usr/bin/sddm --help | head -n 5"
  ./tools/qemu-arm-rootfs.sh shell
EOF
}

set_rootfs_from_arg() {
  if [ "${1:-}" != "" ] && [ -d "${1:-}" ]; then
    ROOTFS="$1"
    shift
  fi
  REMAINING_ARGS=("$@")
}

find_qemu_arm_static() {
  local candidates=(
    "/home/tiel/.local/var/peacock/image-build-chroot/x86_64/usr/bin/qemu-arm-static"
    "/home/tiel/.local/var/peacock/build-chroot/x86_64/usr/bin/qemu-arm-static"
    "/home/tiel/.local/var/peacock/build-chroot/armv7/usr/bin/qemu-arm-static"
    "/home/tiel/.local/var/pmbootstrap/chroot_buildroot_armv7/usr/bin/qemu-arm-static"
  )
  local c
  for c in "${candidates[@]}"; do
    if [ -x "$c" ]; then
      echo "$c"
      return 0
    fi
  done
  die "could not find qemu-arm-static"
}

ensure_rootfs() {
  [ -d "$ROOTFS" ] || die "rootfs not found: $ROOTFS"
  [ -d "$ROOTFS/usr/bin" ] || die "missing $ROOTFS/usr/bin"
}

ensure_binfmt() {
  local magic mask
  magic=$'\x7f\x45\x4c\x46\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00'
  mask=$'\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff'

  if [ ! -e /proc/sys/fs/binfmt_misc/status ]; then
    sudo modprobe binfmt_misc || true
    sudo mount -t binfmt_misc none /proc/sys/fs/binfmt_misc || true
  fi

  if [ -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    echo -1 | sudo tee /proc/sys/fs/binfmt_misc/qemu-arm >/dev/null
  fi

  printf ':qemu-arm:M::%b:%b:/usr/bin/qemu-arm-static:OC\n' "$magic" "$mask" | \
    sudo tee /proc/sys/fs/binfmt_misc/register >/dev/null
}

ensure_qemu_in_rootfs() {
  local qemu_static
  qemu_static="$(find_qemu_arm_static)"
  sudo install -m 0755 "$qemu_static" "$ROOTFS/usr/bin/qemu-arm-static"
}

mount_runtime() {
  local target

  target="$ROOTFS/proc"
  if ! mountpoint -q "$target"; then
    sudo mount -t proc proc "$target"
    MOUNTED_TARGETS+=("$target")
  fi

  for d in /sys /dev /run /tmp; do
    target="$ROOTFS$d"
    [ -d "$target" ] || continue
    if ! mountpoint -q "$target"; then
      sudo mount --bind "$d" "$target"
      MOUNTED_TARGETS+=("$target")
    fi
  done
}

cleanup_mounts() {
  local i target
  for ((i=${#MOUNTED_TARGETS[@]}-1; i>=0; i--)); do
    target="${MOUNTED_TARGETS[$i]}"
    sudo umount "$target" 2>/dev/null || sudo umount -l "$target" 2>/dev/null || true
  done
}

cmd_setup() {
  ensure_rootfs
  ensure_qemu_in_rootfs
  ensure_binfmt
  echo "setup ok"
  echo "rootfs: $ROOTFS"
}

cmd_status() {
  ensure_rootfs
  echo "rootfs: $ROOTFS"
  if [ -x "$ROOTFS/usr/bin/qemu-arm-static" ]; then
    echo "rootfs qemu-arm-static: present"
  else
    echo "rootfs qemu-arm-static: missing"
  fi
  if [ -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    echo "binfmt qemu-arm: registered"
    cat /proc/sys/fs/binfmt_misc/qemu-arm
  else
    echo "binfmt qemu-arm: not registered"
  fi
}

cmd_run() {
  local command="$1"
  cmd_setup
  mount_runtime
  trap cleanup_mounts EXIT
  sudo chroot "$ROOTFS" /bin/bash -lc "$command"
}

cmd_shell() {
  cmd_setup
  mount_runtime
  trap cleanup_mounts EXIT
  sudo chroot "$ROOTFS" /bin/bash -l
}

main() {
  local sub
  sub="${1:-}"
  [ -n "$sub" ] || { usage; exit 1; }
  shift || true

  case "$sub" in
    setup)
      set_rootfs_from_arg "$@"
      cmd_setup
      ;;
    status)
      set_rootfs_from_arg "$@"
      cmd_status
      ;;
    run)
      set_rootfs_from_arg "$@"
      [ "${#REMAINING_ARGS[@]}" -ge 1 ] || die "run requires a command string"
      cmd_run "${REMAINING_ARGS[*]}"
      ;;
    shell)
      set_rootfs_from_arg "$@"
      cmd_shell
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
