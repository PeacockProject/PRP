#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"
OUT_DIR="$2"

require_cmd dd
require_cmd mkfs.ext4
require_cmd tar
require_cmd sha256sum

load_config "$CFG"
mkdir -p "$OUT_DIR"

OVERLAY_SIZE_MB="${OVERLAY_SIZE_MB:-60}"
OVERLAY_LABEL="${OVERLAY_LABEL:-PRP_ROOTFS}"
PRP_OVERLAY_STAGE_ONLY="${PRP_OVERLAY_STAGE_ONLY:-0}"
# jflte's 3.4 kernel can't mount modern ext4 features (metadata_csum/64bit/orphan_file).
# Keep those feature masks device-specific so modern devices get normal ext4 defaults.
if [[ -z "${OVERLAY_EXT4_OPTS+x}" ]]; then
  case "$TARGET_NAME" in
    jflte|samsung-jflte)
      OVERLAY_EXT4_OPTS="-O ^metadata_csum,^metadata_csum_seed,^64bit,^orphan_file -E lazy_itable_init=0,lazy_journal_init=0"
      ;;
    *)
      OVERLAY_EXT4_OPTS="-E lazy_itable_init=0,lazy_journal_init=0"
      ;;
  esac
fi

STAGE_DIR="$OUT_DIR/overlay-stage"
IMG="$OUT_DIR/prp-rootfs.img"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

# Base template (empty by default, script creates the structure we need).
TEMPLATE_DIR="$PRP_ROOT/overlay/rootfs"
[[ -d "$TEMPLATE_DIR" ]] || die "overlay template missing: $TEMPLATE_DIR"
cp -a "$TEMPLATE_DIR"/. "$STAGE_DIR"/

mkdir -p \
  "$STAGE_DIR/bin" \
  "$STAGE_DIR/lib" \
  "$STAGE_DIR/sbin" \
  "$STAGE_DIR/usr/bin" \
  "$STAGE_DIR/usr/lib" \
  "$STAGE_DIR/usr/sbin" \
  "$STAGE_DIR/etc/dropbear" \
  "$STAGE_DIR/etc/prp" \
  "$STAGE_DIR/root/.ssh" \
  "$STAGE_DIR/tmp" \
  "$STAGE_DIR/var" \
  "$STAGE_DIR/opt"

# Default GUI config (persistent, lives on PRP_ROOTFS).
if [[ ! -f "$STAGE_DIR/etc/prp-gui.conf" ]]; then
  cat > "$STAGE_DIR/etc/prp-gui.conf" <<'EOF'
# PRP GUI configuration
#
# Keys:
#   FBDEV=/dev/fb0
#   INPUT=/dev/input/eventX
#   POWER_INPUT=/dev/input/eventY
#   SCALE=100   (50..200)
#   LOGO=/etc/prp/header_logo.png
#
# Leave FBDEV/INPUT unset to auto-detect.

SCALE=100
EOF
fi

# Device-profile GUI input overrides.
if [[ -n "${GUI_POWER_INPUT:-}" ]]; then
  printf '\nPOWER_INPUT=%s\n' "$GUI_POWER_INPUT" >> "$STAGE_DIR/etc/prp-gui.conf"
fi
if [[ -n "${GUI_POWER_HINT:-}" ]]; then
  printf 'POWER_HINT=%s\n' "$GUI_POWER_HINT" >> "$STAGE_DIR/etc/prp-gui.conf"
fi
if [[ -n "${GUI_POWER_CODE:-}" ]]; then
  printf 'POWER_CODE=%s\n' "$GUI_POWER_CODE" >> "$STAGE_DIR/etc/prp-gui.conf"
fi

# Optional AppBar logo. Put header_logo.png in repo root or prp/overlay assets.
for cand in \
  "$PRP_ROOT/assets/header_logo.png" \
  "$PRP_ROOT/logo_header.png" \
  "$PRP_ROOT/header_logo.png" \
  "$PRP_ROOT/../header_logo.png"; do
  if [[ -f "$cand" ]]; then
    cp -a "$cand" "$STAGE_DIR/etc/prp/header_logo.png"
    break
  fi
done

# Optional SSH login MOTD banner (ASCII art etc.).
for cand in \
  "$PRP_ROOT/motd.txt" \
  "$PRP_ROOT/assets/motd.txt" \
  "$PRP_ROOT/overlay/rootfs/etc/prp/motd.txt" \
  "$PRP_ROOT/../motd.txt"; do
  if [[ -f "$cand" ]]; then
    cp -a "$cand" "$STAGE_DIR/etc/prp/motd.txt"
    break
  fi
done

# Ensure any template-provided scripts are executable.
find "$STAGE_DIR/usr/bin" -maxdepth 1 -type f -print0 2>/dev/null | xargs -0r chmod +x || true

# Include a standalone busybox on the overlay so PRP_ROOTFS can be used as a "toolbox" partition.
BB_PATH="${BUSYBOX_STATIC/#\~/$HOME}"
[[ -f "$BB_PATH" ]] || die "static busybox not found: $BB_PATH"
cp -a "$BB_PATH" "$STAGE_DIR/sbin/busybox"
chmod +x "$STAGE_DIR/sbin/busybox"
ln -snf /sbin/busybox "$STAGE_DIR/bin/sh"

# Broader set of applets for rescue/debug sessions.
for app in \
  sh ash \
  ls cat echo printf grep egrep fgrep sed awk cut tr sort uniq wc head tail tee \
  basename dirname \
  find xargs strings \
  ps top dmesg logread free df du stat uptime uname env id whoami \
  mount umount mountpoint blkid lsmod modprobe insmod rmmod \
  mkdir rmdir mknod chmod chown ln mv cp rm sync sleep kill killall \
  hexdump xxd md5sum sha1sum sha256sum \
  vi less more \
  ip ifconfig route netstat ping ping6 nslookup \
  tar gzip gunzip zcat unzip \
  dd \
  nc telnet telnetd tcpsvd \
  fdisk sfdisk lsblk \
  mke2fs mkfs.ext4 e2fsck fsck.ext4 fsck tune2fs dumpe2fs resize2fs; do
  ln -snf /sbin/busybox "$STAGE_DIR/bin/$app" 2>/dev/null || true
  ln -snf /sbin/busybox "$STAGE_DIR/usr/bin/$app" 2>/dev/null || true
done

# Compatibility wrappers for commands frequently expected during debugging.
# They gracefully fall back when a BusyBox applet wasn't compiled in.
rm -f \
  "$STAGE_DIR/bin/whoami" "$STAGE_DIR/bin/lsblk" "$STAGE_DIR/bin/fdisk" \
  "$STAGE_DIR/usr/bin/whoami" "$STAGE_DIR/usr/bin/lsblk" "$STAGE_DIR/usr/bin/fdisk"

cat > "$STAGE_DIR/usr/bin/whoami" <<'EOF'
#!/bin/sh
exec /bin/id -un
EOF
chmod +x "$STAGE_DIR/usr/bin/whoami"

cat > "$STAGE_DIR/usr/bin/lsblk" <<'EOF'
#!/bin/sh
if /sbin/busybox --list 2>/dev/null | /sbin/busybox grep -qx lsblk; then
  exec /sbin/busybox lsblk "$@"
fi
printf '%-14s %-12s %-8s %s\n' NAME TYPE SIZE_MiB LABEL
for u in /sys/class/block/mmcblk0* /sys/class/block/loop* /sys/class/block/sd*; do
  [ -e "$u" ] || continue
  n="${u##*/}"
  [ "$n" = "mmcblk0boot0" ] && continue
  [ "$n" = "mmcblk0boot1" ] && continue
  szs="$(cat "$u/size" 2>/dev/null || echo 0)"
  t="disk"
  case "$n" in
    *p[0-9]*) t="part" ;;
  esac
  mib=$((szs / 2048))
  lbl="$(/sbin/busybox blkid "/dev/$n" 2>/dev/null | /sbin/busybox sed -n 's/.*LABEL=\"\\([^\"]*\\)\".*/\\1/p' || true)"
  printf '%-14s %-12s %-8s %s\n' "$n" "$t" "$mib" "${lbl:-"-"}"
done

# Nested-partition aliases (exported by init as /dev/mmcblk*p*s* -> /dev/loopXpN).
for a in /dev/mmcblk*p*s* /dev/block/mmcblk*p*s*; do
  [ -L "$a" ] || continue
  n="${a##*/}"
  r="$("/sbin/busybox" readlink -f "$a" 2>/dev/null || true)"
  [ -n "$r" ] || continue
  src="${r##*/}"
  szs="$(cat "/sys/class/block/$src/size" 2>/dev/null || echo 0)"
  mib=$((szs / 2048))
  lbl="$(/sbin/busybox blkid "$a" 2>/dev/null | /sbin/busybox sed -n 's/.*LABEL=\"\\([^\"]*\\)\".*/\\1/p' || true)"
  printf '%-14s %-12s %-8s %s\n' "$n" "part" "$mib" "${lbl:-"-"}"
done
EOF
chmod +x "$STAGE_DIR/usr/bin/lsblk"

cat > "$STAGE_DIR/usr/bin/fdisk" <<'EOF'
#!/bin/sh
if [ -x /sbin/fdisk ]; then
  export LD_LIBRARY_PATH="/lib:/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  exec /sbin/fdisk "$@"
fi
if /sbin/busybox --list 2>/dev/null | /sbin/busybox grep -qx fdisk; then
  exec /sbin/busybox fdisk "$@"
fi
echo "fdisk applet not available in busybox; showing partition summary instead."
exec /usr/bin/prp-partlist
EOF
chmod +x "$STAGE_DIR/usr/bin/fdisk"

VENDOR_RUNTIME="$PRP_ROOT/vendor/$TARGET_NAME/rootfs-runtime"
# SSH server + ssh/scp client bits.
# Prefer distro/runtime binaries synced from the target rootfs; fall back to
# local static cross-build when unavailable.
dropbear_ok=0
if [[ -x "$VENDOR_RUNTIME/usr/sbin/dropbear" && -x "$VENDOR_RUNTIME/usr/sbin/dropbearkey" ]]; then
  cp -a "$VENDOR_RUNTIME/usr/sbin/dropbear" "$STAGE_DIR/usr/sbin/dropbear"
  cp -a "$VENDOR_RUNTIME/usr/sbin/dropbearkey" "$STAGE_DIR/usr/sbin/dropbearkey"
  [[ -x "$VENDOR_RUNTIME/usr/bin/dbclient" ]] && cp -a "$VENDOR_RUNTIME/usr/bin/dbclient" "$STAGE_DIR/usr/bin/dbclient"
  [[ -x "$VENDOR_RUNTIME/usr/bin/scp" ]] && cp -a "$VENDOR_RUNTIME/usr/bin/scp" "$STAGE_DIR/usr/bin/scp"
  dropbear_ok=1
# SSH is a convenience (remote recovery), NOT required for the GUI/tty recovery
# session. Don't let a dropbear build failure abort the whole overlay — warn and
# continue, mirroring the GUI's best-effort handling below.
elif "$SCRIPT_DIR/build-dropbear.sh" "$CFG" "$OUT_DIR"; then
  cp -a "$OUT_DIR/tools/dropbear-out/${TARGET_ARCH}/dropbear" "$STAGE_DIR/usr/sbin/dropbear"
  cp -a "$OUT_DIR/tools/dropbear-out/${TARGET_ARCH}/dropbearkey" "$STAGE_DIR/usr/sbin/dropbearkey"
  cp -a "$OUT_DIR/tools/dropbear-out/${TARGET_ARCH}/dbclient" "$STAGE_DIR/usr/bin/dbclient"
  cp -a "$OUT_DIR/tools/dropbear-out/${TARGET_ARCH}/scp" "$STAGE_DIR/usr/bin/scp"
  dropbear_ok=1
else
  echo "overlay: dropbear build failed; continuing without ssh (GUI/tty recovery unaffected)" >&2
fi
if [[ "$dropbear_ok" == "1" ]]; then
  ln -snf /usr/bin/dbclient "$STAGE_DIR/usr/bin/ssh"
  chmod +x "$STAGE_DIR/usr/sbin/dropbear" "$STAGE_DIR/usr/sbin/dropbearkey" "$STAGE_DIR/usr/bin/dbclient" "$STAGE_DIR/usr/bin/scp" 2>/dev/null || true
fi

# Keep framebuffer helpers available even when /usr is bind-mounted from overlay.
overlay_fb_helpers=(peacock-splash)
if [[ "${USE_FB_REFRESHER:-0}" == "1" && "${IS_MSM_FB_REFRESHER:-0}" == "1" ]]; then
  overlay_fb_helpers+=(msm-fb-refresher)
fi
for f in "${overlay_fb_helpers[@]}"; do
  if [[ -x "$VENDOR_RUNTIME/usr/bin/$f" ]]; then
    cp -a "$VENDOR_RUNTIME/usr/bin/$f" "$STAGE_DIR/usr/bin/$f"
    chmod +x "$STAGE_DIR/usr/bin/$f"
  fi
done

# Keep full fdisk stack on PRP_ROOTFS (overlay) for maintenance/debug sessions.
if [[ -x "$VENDOR_RUNTIME/sbin/fdisk" ]]; then
  cp -a "$VENDOR_RUNTIME/sbin/fdisk" "$STAGE_DIR/sbin/fdisk"
  chmod +x "$STAGE_DIR/sbin/fdisk"
  shopt -s nullglob
  for pat in \
    'ld-linux-armhf.so*' \
    'ld-linux-aarch64.so*' \
    'libc.so*' \
    'libgcc_s.so*' \
    'libm.so*' \
    'libfdisk.so*' \
    'libreadline.so*' \
    'libncursesw.so*' \
    'libtinfo.so*' \
    'libsmartcols.so*' \
    'libblkid.so*' \
    'libuuid.so*' \
    'libudev.so*'; do
    for src in "$VENDOR_RUNTIME"/lib/$pat "$VENDOR_RUNTIME"/usr/lib/$pat; do
      [[ -e "$src" || -L "$src" ]] || continue
      cp -a "$src" "$STAGE_DIR/lib/"
    done
  done
  shopt -u nullglob
fi

# GUI binary — this IS the recovery UI. REQUIRED: an overlay without prp-gui is
# useless for recovery, so fail loudly rather than silently shipping a GUI-less
# rootfs. (Contrast dropbear above, where ssh is a genuine convenience.)
if ! "$SCRIPT_DIR/build-gui.sh" "$CFG" "$OUT_DIR"; then
  die "prp-gui build failed (see above) — refusing to ship a GUI-less overlay. \
Common cause: no DNS in the build chroot (curl 'Could not resolve host') so the \
LVGL source can't be fetched; ensure the chroot has a working /etc/resolv.conf."
fi
GUI_BIN="$OUT_DIR/tools/gui-out/${TARGET_ARCH}/prp-gui"
[[ -f "$GUI_BIN" ]] || die "prp-gui build reported success but $GUI_BIN is missing"
cp -a "$GUI_BIN" "$STAGE_DIR/usr/bin/prp-gui"
chmod +x "$STAGE_DIR/usr/bin/prp-gui"

# Best-effort: bake host public keys into overlay for key-based login.
if [[ -d "$HOME/.ssh" ]]; then
  auth_src=()
  while IFS= read -r -d '' f; do auth_src+=("$f"); done < <(find "$HOME/.ssh" -maxdepth 1 -type f -name '*.pub' -print0 2>/dev/null || true)
  if [[ -f "$HOME/.ssh/authorized_keys" ]]; then
    auth_src+=("$HOME/.ssh/authorized_keys")
  fi
  if [[ "${#auth_src[@]}" -gt 0 ]]; then
    # De-dupe and strip empty/comment-only lines.
    awk 'NF && $1 !~ /^#/ { if(!seen[$0]++) print $0 }' "${auth_src[@]}" > "$STAGE_DIR/root/.ssh/authorized_keys"
    cp -a "$STAGE_DIR/root/.ssh/authorized_keys" "$STAGE_DIR/etc/dropbear/authorized_keys"
    chmod 0700 "$STAGE_DIR/root/.ssh"
    chmod 0600 "$STAGE_DIR/root/.ssh/authorized_keys"
    chmod 0600 "$STAGE_DIR/etc/dropbear/authorized_keys"
  fi
fi

# PRP recovery re-entry, for the PeacockOS base (peacock-init).
# When an active flavor fails to boot, peacock-init mounts this PRP_ROOTFS and
# chroots into it, then runs /usr/bin/prp-recovery-enter. That re-execs PRP's
# OWN init (this same script, installed as /usr/lib/prp/prp-init with the device
# @PRP_*@ values baked in) with PRP_RECOVERY_SESSION=1, which skips the boot-only
# steps and brings up the real recovery session (framebuffer/GUI/ssh/usb/tty) in
# place — no reboot, no duplicated logic.
mkdir -p "$STAGE_DIR/usr/lib/prp"
cp -a "$PRP_ROOT/$INITRAMFS_ROOTFS/init" "$STAGE_DIR/usr/lib/prp/prp-init"
substitute_prp_init "$STAGE_DIR/usr/lib/prp/prp-init"
chmod +x "$STAGE_DIR/usr/lib/prp/prp-init"

cat > "$STAGE_DIR/usr/bin/prp-recovery-enter" <<'EOF'
#!/sbin/busybox sh
# Recovery entry invoked by the PeacockOS base (peacock-init) after it mounts
# PRP_ROOTFS and chroots in. Run PRP's real recovery session in place.
export PRP_RECOVERY_SESSION=1
exec /usr/lib/prp/prp-init "$@"
EOF
chmod +x "$STAGE_DIR/usr/bin/prp-recovery-enter"

# Create ext4 image populated from a tarball so we can force numeric root ownership without sudo.
if [[ "$PRP_OVERLAY_STAGE_ONLY" == "1" ]]; then
  echo "overlay-stage: $STAGE_DIR"
  exit 0
fi

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$OVERLAY_SIZE_MB" status=none

TAR="$OUT_DIR/overlay-stage.tar"
rm -f "$TAR"
tar --numeric-owner --owner=0 --group=0 -cpf "$TAR" -C "$STAGE_DIR" .

mkfs.ext4 -F -L "$OVERLAY_LABEL" $OVERLAY_EXT4_OPTS -d "$TAR" "$IMG" >/dev/null

echo "overlay: $IMG"
ls -lh "$IMG"
sha256sum "$IMG"
