#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: qemu-boot-image.sh [IMAGE]

Boot a Peacock-generated x86_64 disk image in QEMU by extracting
kernel/initramfs from the image's BOOT partition and using -kernel/-initrd.

Environment variables:
  QEMU_MEM_MB   Memory in MB (default: 2048)
  QEMU_SMP      CPU cores (default: 2)
  QEMU_APPEND   Extra kernel cmdline arguments
  QEMU_BIN      QEMU binary (default: qemu-system-x86_64)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

IMG="${1:-$HOME/.local/var/peacock/qemu-x86_64.img}"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_MEM_MB="${QEMU_MEM_MB:-2048}"
QEMU_SMP="${QEMU_SMP:-2}"
QEMU_APPEND="${QEMU_APPEND:-}"

if [[ ! -f "$IMG" ]]; then
  echo "image not found: $IMG" >&2
  exit 1
fi

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
  echo "missing qemu binary: $QEMU_BIN" >&2
  exit 1
fi

if ! command -v sudo >/dev/null 2>&1; then
  echo "sudo is required" >&2
  exit 1
fi

tmp_dir="$(mktemp -d)"
boot_mnt="$tmp_dir/boot"
kernel_dst="$tmp_dir/vmlinuz"
initrd_dst="$tmp_dir/initramfs.img"
loopdev=""

cleanup() {
  set +e
  if mountpoint -q "$boot_mnt"; then
    sudo umount "$boot_mnt"
  fi
  if [[ -n "$loopdev" ]]; then
    sudo losetup -d "$loopdev"
  fi
  rm -rf "$tmp_dir"
}
trap cleanup EXIT

mkdir -p "$boot_mnt"
loopdev="$(sudo losetup --find --show --partscan "$IMG")"
if [[ ! -b "${loopdev}p1" ]]; then
  echo "boot partition not found on loop device: ${loopdev}p1" >&2
  exit 1
fi

sudo mount -o ro "${loopdev}p1" "$boot_mnt"

kernel_src=""
if [[ -f "$boot_mnt/vmlinuz-linux" ]]; then
  kernel_src="$boot_mnt/vmlinuz-linux"
else
  kernel_src="$(find "$boot_mnt" -maxdepth 1 -type f -name 'vmlinuz*' | head -n 1 || true)"
fi
if [[ -z "$kernel_src" ]]; then
  echo "kernel not found in BOOT partition" >&2
  exit 1
fi

initrd_src=""
if [[ -f "$boot_mnt/initramfs-linux.img" ]]; then
  initrd_src="$boot_mnt/initramfs-linux.img"
else
  initrd_src="$(find "$boot_mnt" -maxdepth 1 -type f -name 'initramfs*.img' ! -name '*fallback*' | head -n 1 || true)"
fi
if [[ -z "$initrd_src" ]]; then
  echo "initramfs not found in BOOT partition" >&2
  exit 1
fi

sudo cp "$kernel_src" "$kernel_dst"
sudo cp "$initrd_src" "$initrd_dst"
sudo chown "$(id -u):$(id -g)" "$kernel_dst" "$initrd_dst"

base_append="root=LABEL=ROOT rw console=ttyS0,115200 loglevel=7"
if [[ -n "$QEMU_APPEND" ]]; then
  base_append="$base_append $QEMU_APPEND"
fi

echo "Launching $IMG with $QEMU_BIN"
echo "kernel: $(basename "$kernel_src"), initramfs: $(basename "$initrd_src")"

exec "$QEMU_BIN" \
  -machine q35,accel=kvm:tcg \
  -cpu max \
  -smp "$QEMU_SMP" \
  -m "$QEMU_MEM_MB" \
  -drive "file=$IMG,format=raw,if=virtio" \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  -kernel "$kernel_dst" \
  -initrd "$initrd_dst" \
  -append "$base_append" \
  -nographic \
  -serial mon:stdio
