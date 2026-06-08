#!/bin/sh
# Shared Peacock subpartition mount helpers for PRP.

BB=${BB:-/sbin/busybox}
if [ ! -x "$BB" ]; then
  BB=busybox
fi

PEACOCK_BOOT_MNT=${PEACOCK_BOOT_MNT:-/mnt/peacock_boot}
PEACOCK_ROOT_MNT=${PEACOCK_ROOT_MNT:-/mnt/peacock_root}
USERDATA_DEV_HINT=${USERDATA_DEV_HINT:-"/dev/block/bootdevice/by-name/userdata /dev/block/platform/*/by-name/USERDATA /dev/block/platform/*/by-name/userdata /dev/block/by-name/USERDATA /dev/block/by-name/userdata"}
BUILD_TAG=${BUILD_TAG:-unknown}

if ! command -v log >/dev/null 2>&1; then
  log() {
    printf '%s\n' "$*"
  }
fi

is_peacock_container_dev() {
  local d="$1"
  local sig=""
  local sz=""
  [ -b "$d" ] || return 1
  sig="$($BB dd if="$d" bs=1 skip=512 count=8 2>/dev/null || true)"
  [ "$sig" = "EFI PART" ] || return 1
  sz="$($BB cat "/sys/class/block/${d##*/}/size" 2>/dev/null || true)"
  case "$sz" in
    ''|*[!0-9]*) return 1 ;;
  esac
  [ "$sz" -gt 1048576 ] || return 1
  return 0
}

find_best_gpt_container_dev() {
  local d=""
  local sz=""
  local best=""
  local best_sz=0
  for d in /dev/mmcblk0p* /dev/block/mmcblk0p*; do
    is_peacock_container_dev "$d" || continue
    sz="$($BB cat "/sys/class/block/${d##*/}/size" 2>/dev/null || true)"
    case "$sz" in
      ''|*[!0-9]*) continue ;;
    esac
    if [ "$sz" -gt "$best_sz" ]; then
      best_sz="$sz"
      best="$d"
    fi
  done
  [ -n "$best" ] || return 1
  echo "$best"
  return 0
}

resolve_userdata_dev() {
  local p=""
  local uevent=""
  local dev=""
  local node=""

  for p in $USERDATA_DEV_HINT; do
    [ -e "$p" ] || continue
    if [ -L "$p" ]; then
      dev="$(readlink -f "$p" 2>/dev/null || true)"
    else
      dev="$p"
    fi
    [ -n "$dev" ] || continue
    is_peacock_container_dev "$dev" && {
      echo "$dev"
      return 0
    }
  done

  for p in \
    /dev/block/platform/*/by-name/USERDATA \
    /dev/block/platform/*/by-name/userdata \
    /dev/block/by-name/USERDATA \
    /dev/block/by-name/userdata; do
    [ -e "$p" ] || continue
    if [ -L "$p" ]; then
      dev="$(readlink -f "$p" 2>/dev/null || true)"
      [ -n "$dev" ] && is_peacock_container_dev "$dev" && {
        echo "$dev"
        return 0
      }
    elif is_peacock_container_dev "$p"; then
      echo "$p"
      return 0
    fi
  done

  for uevent in /sys/class/block/mmcblk0p*/uevent; do
    [ -r "$uevent" ] || continue
    if $BB grep -qi '^PARTNAME=userdata$' "$uevent" 2>/dev/null; then
      node="$(basename "$(dirname "$uevent")")"
      dev="/dev/$node"
      is_peacock_container_dev "$dev" && {
        echo "$dev"
        return 0
      }
      dev="/dev/block/$node"
      is_peacock_container_dev "$dev" && {
        echo "$dev"
        return 0
      }
    fi
  done

  dev="$(find_best_gpt_container_dev 2>/dev/null || true)"
  if [ -n "$dev" ]; then
    echo "$dev"
    return 0
  fi
  return 1
}

find_free_loop() {
  local skip="${1:-}"
  local l=""
  local sys=""
  # Prefer kernel-selected free loop when possible.
  if $BB --list 2>/dev/null | $BB grep -qx losetup; then
    l="$($BB losetup -f 2>/dev/null || true)"
    if [ -n "$l" ] && [ "$l" != "$skip" ] && [ -b "$l" ]; then
      echo "$l"
      return 0
    fi
  fi
  # Next, inspect loops that exist in sysfs.
  for sys in /sys/class/block/loop*/loop/backing_file; do
    [ -e "$sys" ] || continue
    l="/dev/${sys%/loop/backing_file}"
    l="${l##*/}"
    l="/dev/$l"
    [ -b "$l" ] || continue
    [ "$l" = "$skip" ] && continue
    if [ ! -s "$sys" ]; then
      echo "$l"
      return 0
    fi
  done
  for l in /dev/loop0 /dev/loop1 /dev/loop2 /dev/loop3 /dev/loop4 /dev/loop5 /dev/loop6 /dev/loop7; do
    [ -b "$l" ] || continue
    [ "$l" = "$skip" ] && continue
    sys="/sys/class/block/${l##*/}/loop/backing_file"
    if [ ! -e "$sys" ] || [ ! -s "$sys" ]; then
      echo "$l"
      return 0
    fi
  done
  return 1
}

loop_attach_labeled() {
  local loopdev="$1"
  local backing="$2"
  local off_bytes="$3"
  local off_alt="$4"
  local expect_label="$5"
  local logf="$6"
  local off=""
  local blk=""
  local magic=""
  local raw_magic=""
  local derr=""

  for off in "$off_bytes" "$off_alt"; do
    [ -n "$off" ] || continue
    $BB losetup -d "$loopdev" >/dev/null 2>&1 || true
    if ! $BB losetup -o "$off" "$loopdev" "$backing" >>"$logf" 2>&1; then
      echo "loop_attach $loopdev off=$off failed" >> "$logf"
      continue
    fi
    raw_magic="$($BB dd if="$backing" bs=1 skip=$((off + 1080)) count=2 2>/tmp/prp-dd.raw.err | $BB hexdump -v -e '1/1 \"%02x\"' 2>/dev/null || true)"
    derr="$($BB cat /tmp/prp-dd.raw.err 2>/dev/null || true)"
    echo "loop_attach backing_probe off=$off raw_magic=${raw_magic:-none} raw_err=${derr:-none}" >> "$logf"
    blk="$($BB blkid "$loopdev" 2>/dev/null || true)"
    magic="$($BB dd if="$loopdev" bs=1 skip=1080 count=2 2>/tmp/prp-dd.loop.err | $BB hexdump -v -e '1/1 \"%02x\"' 2>/dev/null || true)"
    derr="$($BB cat /tmp/prp-dd.loop.err 2>/dev/null || true)"
    echo "loop_attach $loopdev off=$off blkid=${blk:-none} sb_magic=${magic:-none} loop_err=${derr:-none}" >> "$logf"
    $BB losetup -a >>"$logf" 2>&1 || true
    log_loop_state "$loopdev" "loop_attach_state" "$logf"
    case "$blk" in
      *"LABEL=\"$expect_label\""*) return 0 ;;
    esac
  done
  $BB losetup -d "$loopdev" >/dev/null 2>&1 || true
  return 1
}

log_loop_state() {
  local loopdev="$1"
  local tag="$2"
  local name="${loopdev##*/}"
  local bf=""
  local off=""
  local lim=""
  local sz=""
  [ -b "$loopdev" ] || return 0
  bf="$($BB cat "/sys/class/block/$name/loop/backing_file" 2>/dev/null || true)"
  off="$($BB cat "/sys/class/block/$name/loop/offset" 2>/dev/null || true)"
  lim="$($BB cat "/sys/class/block/$name/loop/sizelimit" 2>/dev/null || true)"
  sz="$($BB cat "/sys/class/block/$name/size" 2>/dev/null || true)"
  echo "$tag loop=$loopdev backing=${bf:-none} offset=${off:-none} sizelimit=${lim:-none} sectors=${sz:-none}" >> "$3"
}

ensure_block_node() {
  local node_name="$1"
  local devspec=""
  local maj=""
  local min=""
  [ -n "$node_name" ] || return 1
  [ -b "/dev/$node_name" ] && {
    echo "/dev/$node_name"
    return 0
  }
  devspec="$($BB cat "/sys/class/block/$node_name/dev" 2>/dev/null || true)"
  case "$devspec" in
    *:*)
      maj="${devspec%:*}"
      min="${devspec#*:}"
      $BB mknod "/dev/$node_name" b "$maj" "$min" 2>/dev/null || true
      ;;
  esac
  [ -b "/dev/$node_name" ] && {
    echo "/dev/$node_name"
    return 0
  }
  return 1
}

mount_subparts() {
  local userdata_dev="${1:-}"
  local boot_start=2048
  local root_start=1050624
  local boot_span_sectors=$((root_start - boot_start))
  local boot_off
  local root_off
  local boot_size_bytes=0
  local root_size_bytes=0
  local total_sectors=""
  local sect_path=""
  local gpt_sig=""
  local has_gpt=0
  local logic_bs=""
  local phys_bs=""
  local boot_sb_magic=""
  local root_sb_magic=""
  local losetup_has_sizelimit=0
  local logf="/tmp/prp-subparts.log"
  local boot_loop=""
  local root_loop=""
  local boot_src=""
  local root_src=""
  local boot_magic=""
  local main_loop=""
  local main_p1=""
  local main_p2=""
  local base=""
  local boot_mounted=0
  local root_mounted=0
  local ptries=0
  local root_off_alt="$root_start"
  local boot_off_alt="$boot_start"
  local loop_backing=""
  local cand=""
  local dmsetup_cmd=""
  local dm_boot_name="prp_peacock_boot"
  local dm_root_name="prp_peacock_root"
  local dm_boot_dev=""
  local dm_root_dev=""
  local root_span_sectors=0
  local fdisk_cmd=""
  local fdisk_boot_start=""
  local fdisk_boot_sectors=""
  local fdisk_root_start=""
  local fdisk_root_sectors=""
  local fdisk_line=""
  local alias_bases=""

  mkdir -p /tmp "$PEACOCK_BOOT_MNT" "$PEACOCK_ROOT_MNT"
  {
    echo "=== prp subparts ==="
    echo "build=$BUILD_TAG"
    echo "date=$($BB date -u 2>/dev/null || echo unknown)"
  } > "$logf"

  if [ -z "$userdata_dev" ]; then
    userdata_dev="$(resolve_userdata_dev 2>/dev/null || true)"
  fi
  if [ -z "$userdata_dev" ]; then
    userdata_dev="$(find_best_gpt_container_dev 2>/dev/null || true)"
  fi

  sect_path="/sys/class/block/${userdata_dev##*/}/size"
  if [ -r "$sect_path" ]; then
    total_sectors="$($BB cat "$sect_path" 2>/dev/null || true)"
    case "$total_sectors" in
      ''|*[!0-9]*) total_sectors="" ;;
    esac
  fi
  if $BB losetup --help 2>&1 | $BB grep -q -- ' -S '; then
    losetup_has_sizelimit=1
  fi

  echo "userdata_dev=${userdata_dev:-none}" >> "$logf"
  echo "losetup_sizelimit=$losetup_has_sizelimit total_sectors=${total_sectors:-unknown}" >> "$logf"
  if [ ! -b "$userdata_dev" ]; then
    echo "userdata block device missing" >> "$logf"
    $BB ls -la /dev/mmcblk0p* /dev/block/mmcblk0p* >> "$logf" 2>&1 || true
    log "userdata device not found: ${userdata_dev:-none}"
    return 1
  fi

  gpt_sig="$($BB dd if="$userdata_dev" bs=1 skip=512 count=8 2>/dev/null || true)"
  if [ "$gpt_sig" = "EFI PART" ]; then
    has_gpt=1
  fi
  logic_bs="$($BB cat "/sys/class/block/${userdata_dev##*/}/queue/logical_block_size" 2>/dev/null || true)"
  phys_bs="$($BB cat "/sys/class/block/${userdata_dev##*/}/queue/physical_block_size" 2>/dev/null || true)"
  echo "gpt_sig=${gpt_sig:-none} has_gpt=$has_gpt" >> "$logf"
  echo "logical_block_size=${logic_bs:-unknown} physical_block_size=${phys_bs:-unknown}" >> "$logf"
  if [ "$has_gpt" != "1" ]; then
    log "userdata is not peacock GPT image: $userdata_dev"
    return 1
  fi

  # Dynamic partition offsets from the nested GPT image when available.
  # Keep constants as fallback for compatibility.
  for cand in /sbin/fdisk /usr/sbin/fdisk /usr/bin/fdisk /bin/fdisk; do
    [ -x "$cand" ] || continue
    fdisk_cmd="$cand"
    break
  done
  echo "fdisk_cmd=${fdisk_cmd:-none}" >> "$logf"
  if [ -n "$fdisk_cmd" ]; then
    fdisk_line="$(LD_LIBRARY_PATH=/sbin:/lib:/usr/lib "$fdisk_cmd" -l "$userdata_dev" 2>/tmp/prp-fdisk.err | $BB awk -v d="$userdata_dev" '$1==d"p1"{print $2" "$4; exit}' || true)"
    set -- $fdisk_line
    case "${1:-}:${2:-}" in
      [0-9]*:[0-9]*)
        fdisk_boot_start="$1"
        fdisk_boot_sectors="$2"
        ;;
    esac
    fdisk_line="$(LD_LIBRARY_PATH=/sbin:/lib:/usr/lib "$fdisk_cmd" -l "$userdata_dev" 2>/tmp/prp-fdisk.err | $BB awk -v d="$userdata_dev" '$1==d"p2"{print $2" "$4; exit}' || true)"
    set -- $fdisk_line
    case "${1:-}:${2:-}" in
      [0-9]*:[0-9]*)
        fdisk_root_start="$1"
        fdisk_root_sectors="$2"
        ;;
    esac
  fi

  if [ -n "$fdisk_boot_start" ] && [ -n "$fdisk_boot_sectors" ] && [ -n "$fdisk_root_start" ] && [ -n "$fdisk_root_sectors" ]; then
    boot_start="$fdisk_boot_start"
    boot_span_sectors="$fdisk_boot_sectors"
    root_start="$fdisk_root_start"
    root_span_sectors="$fdisk_root_sectors"
    echo "layout_source=fdisk p1_start=$boot_start p1_sectors=$boot_span_sectors p2_start=$root_start p2_sectors=$root_span_sectors" >> "$logf"
  else
    echo "layout_source=default p1_start=$boot_start p2_start=$root_start" >> "$logf"
  fi

  boot_off=$((boot_start * 512))
  root_off=$((root_start * 512))
  boot_size_bytes=$((boot_span_sectors * 512))
  if [ -n "$root_span_sectors" ] && [ "$root_span_sectors" -gt 0 ]; then
    root_size_bytes=$((root_span_sectors * 512))
  elif [ -n "$total_sectors" ] && [ "$total_sectors" -gt "$root_start" ]; then
    root_size_bytes=$(((total_sectors - root_start) * 512))
  fi
  boot_sb_magic="$($BB dd if="$userdata_dev" bs=1 skip=$((boot_off + 1080)) count=2 2>/dev/null | $BB hexdump -v -e '1/1 "%02x"' 2>/dev/null || true)"
  root_sb_magic="$($BB dd if="$userdata_dev" bs=1 skip=$((root_off + 1080)) count=2 2>/dev/null | $BB hexdump -v -e '1/1 "%02x"' 2>/dev/null || true)"
  echo "boot_off=$boot_off root_off=$root_off" >> "$logf"
  echo "boot_size_bytes=$boot_size_bytes root_size_bytes=${root_size_bytes:-0}" >> "$logf"
  echo "boot_sb_magic=${boot_sb_magic:-none} root_sb_magic=${root_sb_magic:-none}" >> "$logf"

  $BB umount "$PEACOCK_BOOT_MNT" 2>/dev/null || true
  $BB umount "$PEACOCK_ROOT_MNT" 2>/dev/null || true
  alias_bases="$userdata_dev"
  for cand in $USERDATA_DEV_HINT; do
    case " $alias_bases " in
      *" $cand "*) ;;
      *) alias_bases="$alias_bases $cand" ;;
    esac
  done
  for base in $alias_bases; do
    rm -f "${base}s0" "${base}s1" 2>/dev/null || true
  done

  # Try kernel-native nested-partition exposure first (no loop-on-block required).
  if $BB --list 2>/dev/null | $BB grep -qx partprobe; then
    $BB partprobe "$userdata_dev" >>"$logf" 2>&1 || true
  fi
  if $BB --list 2>/dev/null | $BB grep -qx blockdev; then
    $BB blockdev --rereadpt "$userdata_dev" >>"$logf" 2>&1 || true
  fi
  while [ "$ptries" -lt 4 ]; do
    /sbin/mdev -s >/dev/null 2>&1 || true
    [ -z "$boot_src" ] && {
      for base in \
        "${userdata_dev}p1" "${userdata_dev}s0" \
        "/dev/${userdata_dev##*/}p1" "/dev/${userdata_dev##*/}s0" \
        "/dev/block/${userdata_dev##*/}p1" "/dev/block/${userdata_dev##*/}s0"; do
        [ -b "$base" ] || continue
        boot_src="$base"
        break
      done
    }
    [ -z "$root_src" ] && {
      for base in \
        "${userdata_dev}p2" "${userdata_dev}s1" \
        "/dev/${userdata_dev##*/}p2" "/dev/${userdata_dev##*/}s1" \
        "/dev/block/${userdata_dev##*/}p2" "/dev/block/${userdata_dev##*/}s1"; do
        [ -b "$base" ] || continue
        root_src="$base"
        break
      done
    }
    [ -n "$boot_src" ] && [ -n "$root_src" ] && break
    ptries=$((ptries + 1))
    $BB sleep 1
  done
  [ -n "$boot_src" ] && echo "kernel_subpart boot=$boot_src" >> "$logf"
  [ -n "$root_src" ] && echo "kernel_subpart root=$root_src" >> "$logf"

  # Preferred fallback on this kernel: device-mapper linear mappings.
  # This avoids loop-on-block I/O errors seen on some recovery kernels.
  for cand in /sbin/dmsetup /usr/sbin/dmsetup /usr/bin/dmsetup /bin/dmsetup; do
    [ -x "$cand" ] || continue
    dmsetup_cmd="$cand"
    break
  done
  echo "dmsetup_cmd=${dmsetup_cmd:-none}" >> "$logf"
  if [ -n "$dmsetup_cmd" ] && { [ -z "$boot_src" ] || [ -z "$root_src" ]; }; then
    if [ -n "$total_sectors" ] && [ "$total_sectors" -gt "$root_start" ]; then
      if [ -z "$root_span_sectors" ] || [ "$root_span_sectors" -le 0 ]; then
        root_span_sectors=$((total_sectors - root_start))
      fi
      "$dmsetup_cmd" remove -f "$dm_boot_name" >>"$logf" 2>&1 || true
      "$dmsetup_cmd" remove -f "$dm_root_name" >>"$logf" 2>&1 || true
      echo "dm_boot_table=0 $boot_span_sectors linear $userdata_dev $boot_start" >> "$logf"
      if echo "0 $boot_span_sectors linear $userdata_dev $boot_start" | "$dmsetup_cmd" create "$dm_boot_name" >>"$logf" 2>&1; then
        "$dmsetup_cmd" mknodes >>"$logf" 2>&1 || true
        dm_boot_dev="/dev/mapper/$dm_boot_name"
        [ -b "$dm_boot_dev" ] || dm_boot_dev=""
      fi
      echo "dm_root_table=0 $root_span_sectors linear $userdata_dev $root_start" >> "$logf"
      if echo "0 $root_span_sectors linear $userdata_dev $root_start" | "$dmsetup_cmd" create "$dm_root_name" >>"$logf" 2>&1; then
        "$dmsetup_cmd" mknodes >>"$logf" 2>&1 || true
        dm_root_dev="/dev/mapper/$dm_root_name"
        [ -b "$dm_root_dev" ] || dm_root_dev=""
      fi
      [ -n "$dm_boot_dev" ] && [ -z "$boot_src" ] && {
        boot_src="$dm_boot_dev"
        echo "dm_boot_dev=$dm_boot_dev" >> "$logf"
      }
      [ -n "$dm_root_dev" ] && [ -z "$root_src" ] && {
        root_src="$dm_root_dev"
        echo "dm_root_dev=$dm_root_dev" >> "$logf"
      }
    else
      echo "dmsetup skipped: invalid total_sectors=${total_sectors:-none}" >> "$logf"
    fi
  fi

  # Preferred: kernel-partitioned loop mapping (gives loopXp1/loopXp2 directly).
  if { [ -z "$boot_src" ] || [ -z "$root_src" ]; } && $BB --list 2>/dev/null | $BB grep -qx losetup; then
    main_loop="$(find_free_loop 2>/dev/null || true)"
    if [ -n "$main_loop" ]; then
      $BB losetup -d "$main_loop" >/dev/null 2>&1 || true
      if $BB losetup -P "$main_loop" "$userdata_dev" >>"$logf" 2>&1; then
        # Some kernels ignore -P during setup; force a re-read on loop itself.
        if $BB --list 2>/dev/null | $BB grep -qx partprobe; then
          $BB partprobe "$main_loop" >>"$logf" 2>&1 || true
        fi
        if $BB --list 2>/dev/null | $BB grep -qx blockdev; then
          $BB blockdev --rereadpt "$main_loop" >>"$logf" 2>&1 || true
        fi
        # Wait briefly and ensure /dev nodes for loop partitions exist.
        ptries=0
        while [ "$ptries" -lt 4 ]; do
          /sbin/mdev -s >/dev/null 2>&1 || true
          cand="$(ensure_block_node "${main_loop##*/}p1" 2>/dev/null || true)"
          [ -n "$cand" ] && main_p1="$cand"
          cand="$(ensure_block_node "${main_loop##*/}p2" 2>/dev/null || true)"
          [ -n "$cand" ] && main_p2="$cand"
          [ -n "$main_p1" ] && [ -n "$main_p2" ] && break
          ptries=$((ptries + 1))
          $BB sleep 1
        done
        if [ -n "$main_p1" ] && [ -b "$main_p1" ]; then
          boot_src="$main_p1"
          echo "main_loop boot=$boot_src" >> "$logf"
        fi
        if [ -n "$main_p2" ] && [ -b "$main_p2" ]; then
          root_src="$main_p2"
          echo "main_loop root=$root_src" >> "$logf"
        fi
        $BB losetup -a >>"$logf" 2>&1 || true
        log_loop_state "$main_loop" "main_loop_state" "$logf"
      else
        echo "losetup -P failed on $main_loop" >> "$logf"
        main_loop=""
      fi
    else
      echo "no free loop for losetup -P" >> "$logf"
    fi
  fi

  # Fallback: explicit loops at fixed offsets.
  # Use only for mounting; aliases remain reserved for accurate loopXp1/loopXp2.
  if $BB --list 2>/dev/null | $BB grep -qx losetup; then
    if [ -z "$boot_src" ] || [ -z "$root_src" ]; then
      # Prefer raw userdata backing; loop-on-loop is unreliable on this kernel.
      loop_backing="$userdata_dev"
      echo "loop_backing=$loop_backing" >> "$logf"
      # Map root first (required), boot second (optional).
      if [ -z "$root_src" ]; then
        root_loop="$(find_free_loop "$main_loop" 2>/dev/null || true)"
        if [ -n "$root_loop" ]; then
          if loop_attach_labeled "$root_loop" "$loop_backing" "$root_off" "$root_off_alt" "ROOT" "$logf"; then
            root_src="$root_loop"
            echo "root_loop=$root_loop" >> "$logf"
            log_loop_state "$root_loop" "root_loop_state" "$logf"
          else
            root_loop=""
          fi
        fi
      fi
      if [ -z "$boot_src" ]; then
        boot_loop="$(find_free_loop "$root_loop" 2>/dev/null || true)"
        if [ -n "$boot_loop" ] && [ "$boot_loop" != "$main_loop" ]; then
          if loop_attach_labeled "$boot_loop" "$loop_backing" "$boot_off" "$boot_off_alt" "BOOT" "$logf"; then
            boot_src="$boot_loop"
            echo "boot_loop=$boot_loop" >> "$logf"
            log_loop_state "$boot_loop" "boot_loop_state" "$logf"
          else
            boot_loop=""
          fi
        fi
      fi
      if [ -z "$root_src" ] && [ -z "$boot_src" ]; then
        echo "no usable loop devices for fixed-offset subparts" >> "$logf"
      fi
    fi
  else
    echo "busybox losetup applet missing; using implicit loop mounts" >>"$logf"
  fi

  # Create aliases from discovered partition mappings.
  for base in $alias_bases; do
    [ -n "$base" ] || continue
    [ -e "$base" ] || continue
    [ -n "$boot_src" ] && [ -b "$boot_src" ] && ln -snf "$boot_src" "${base}s0" 2>/dev/null || true
    [ -n "$root_src" ] && [ -b "$root_src" ] && ln -snf "$root_src" "${base}s1" 2>/dev/null || true
  done

  if [ -n "$boot_src" ]; then
    boot_magic="$($BB dd if="$boot_src" bs=8 count=1 2>/dev/null || true)"
    [ -n "$boot_magic" ] && echo "boot_magic=$boot_magic" >> "$logf"
    if $BB mount -t ext2 -o ro "$boot_src" "$PEACOCK_BOOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro "$boot_src" "$PEACOCK_BOOT_MNT" >>"$logf" 2>&1; then
      boot_mounted=1
    fi
  fi
  if [ "$boot_mounted" -ne 1 ]; then
    if $BB mount -t ext2 -o ro,loop,offset="$boot_off" "$userdata_dev" "$PEACOCK_BOOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro,loop,offset="$boot_off" "$userdata_dev" "$PEACOCK_BOOT_MNT" >>"$logf" 2>&1; then
      boot_mounted=1
      boot_src="$($BB awk -v m="$PEACOCK_BOOT_MNT" '$2==m{print $1}' /proc/mounts 2>/dev/null | $BB head -n1 || true)"
    fi
  fi
  if [ -n "$root_src" ]; then
    if $BB mount -t ext4 -o rw "$root_src" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro "$root_src" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro,noload "$root_src" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext2 -o ro "$root_src" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1; then
      root_mounted=1
    fi
  fi

  # Offset fallback if explicit loop mapping didn't mount.
  if [ "$root_mounted" -ne 1 ]; then
    if $BB mount -t ext4 -o rw,loop,offset="$root_off" "$userdata_dev" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro,loop,offset="$root_off" "$userdata_dev" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext4 -o ro,noload,loop,offset="$root_off" "$userdata_dev" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1 || \
       $BB mount -t ext2 -o ro,loop,offset="$root_off" "$userdata_dev" "$PEACOCK_ROOT_MNT" >>"$logf" 2>&1; then
      root_mounted=1
      root_src="$($BB awk -v m="$PEACOCK_ROOT_MNT" '$2==m{print $1}' /proc/mounts 2>/dev/null | $BB head -n1 || true)"
    fi
  fi

  if [ "$root_mounted" -eq 1 ]; then
    log "mounted subpartitions from $userdata_dev"
    if [ "$boot_mounted" -eq 1 ]; then
      log "subpart aliases: ${userdata_dev}s0 -> ${boot_src:-none}, ${userdata_dev}s1 -> ${root_src:-none}"
    else
      log "subpart root mounted; boot partition mount skipped/failed"
      log "subpart alias: ${userdata_dev}s1 -> ${root_src:-none}"
    fi
    log "subpart log: $logf"
    return 0
  fi
  # Clean up failed dm mappings to avoid stale devices across retries.
  if [ -n "$dmsetup_cmd" ]; then
    "$dmsetup_cmd" remove -f "$dm_boot_name" >>"$logf" 2>&1 || true
    "$dmsetup_cmd" remove -f "$dm_root_name" >>"$logf" 2>&1 || true
  fi
  log "subpartition mount failed (see $logf)"
  return 1
}
