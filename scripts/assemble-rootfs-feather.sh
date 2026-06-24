#!/usr/bin/env bash
# assemble-rootfs-feather.sh — build the PRP rootfs from feather packages.
#
# Replaces the old scavenging model (sync-runtime-assets.sh + the inline
# busybox/dropbear builds). Instead PRP's rootfs is a declared package set
# (`prp-base`) installed with `ftr` into a staging tree — dependency-resolved,
# signature-verified, same artifacts as the system.
#
# Flow:
#   1. peacock build-packages --arch <ARCH> the shared ports (busybox, dropbear,
#      util-linux, peacock-splash, e2fsprogs, wpa_supplicant, …) → cached .feathers
#   2. build the PRP GUI (build-gui.sh) → pack prp-gui.feather
#   3. pack prp-runtime.feather (the PRP init + /usr/lib/prp tree, incl. prp-net)
#   4. generate prp-base.feather (layout=meta, depends on all of the above)
#   5. ftr index + sign (build-local key) + ftr sync (file://) + ftr install --root
#
# Prints the staged rootfs path on stdout (last line) for build-initramfs.sh.
#
# Usage: assemble-rootfs-feather.sh <config.env> <out_dir>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/common.sh"

[[ $# -eq 2 ]] || die "usage: $0 <config.env> <out_dir>"
CFG="$1"; OUT_DIR="$2"
load_config "$CFG"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"   # absolute — the tar/cd subshells below need it

ARCH="${TARGET_ARCH:-aarch64}"

# Tool locations (override via env for non-standard checkouts).
PROJECT_ROOT="$(cd "$PRP_ROOT/.." && pwd)"
# Use the working-tree ports (incl. util-linux-prp), not Peacock's stale
# auto-clone, when resolving + building ports.
export PEACOCK_PORTS_DIR="${PEACOCK_PORTS_DIR:-$PROJECT_ROOT/peacock-ports}"
PEACOCK="${PEACOCK_BIN:-$PROJECT_ROOT/Peacock/peacock}"
FEATHER_DIR="${FEATHER_DIR:-$PROJECT_ROOT/feather}"
FTR="${FTR_BIN:-$FEATHER_DIR/ftr}"
GENKEY="${GENKEY_BIN:-$FEATHER_DIR/tools/gen-keypair}"
SIGN="${SIGN_BIN:-$FEATHER_DIR/tools/ftr-sign}"
PKG_STORE="${PEACOCK_PKG_STORE:-$HOME/.local/var/peacock/packages/$ARCH}"

for t in "$PEACOCK" "$FTR" "$GENKEY" "$SIGN"; do
  [[ -x "$t" ]] || die "required tool not found/executable: $t"
done

# Shared ports built by Peacock (already feather packages). util-linux-prp is the
# lean static-musl subset (fdisk/sfdisk/blkid/partx/losetup) — self-contained, so
# PRP needs no glibc/ncurses/readline. wpa_supplicant + e2fsprogs land later;
# absent ports are skipped with a warn.
SHARED_PORTS=(busybox dropbear util-linux-prp e2fsprogs-prp peacock-splash wpa_supplicant)
# The dep list baked into the prp-base metapackage (names only).
PRP_BASE_DEPS=(busybox dropbear util-linux-prp e2fsprogs-prp wpa_supplicant peacock-splash prp-gui prp-runtime)

WORK="$OUT_DIR/feather-assembly"
REPO="$WORK/repo"; STAGE="$WORK/stage"; KEYS="$WORK/keys"
rm -rf "$WORK"; mkdir -p "$REPO" "$STAGE" "$KEYS"

echo "assemble: building shared ports for $ARCH …"
build_list=()
for p in "${SHARED_PORTS[@]}"; do
  if [[ -d "$PROJECT_ROOT/peacock-ports/base/$p" || -d "$PROJECT_ROOT/peacock-ports/device/$p" ]]; then
    build_list+=(-p "$p")
  else
    echo "assemble: WARN port '$p' not found, skipping" >&2
  fi
done
"$PEACOCK" build-packages --arch "$ARCH" "${build_list[@]}"

echo "assemble: collecting shared .feathers …"
for p in "${SHARED_PORTS[@]}"; do
  f=$(ls -1 "$PKG_STORE/$p-"*"-$ARCH.feather" 2>/dev/null | head -1 || true)
  [[ -n "$f" ]] && cp "$f" "$REPO/" || echo "assemble: WARN no built feather for '$p'" >&2
done

echo "assemble: packing prp-gui …"
"$SCRIPT_DIR/build-gui.sh" "$CFG" "$OUT_DIR"
GUI_BIN="$OUT_DIR/tools/gui-out/$ARCH/prp-gui"
[[ -f "$GUI_BIN" ]] || die "prp-gui binary missing: $GUI_BIN"
g="$WORK/pkg-gui"; mkdir -p "$g/files/usr/bin"
cp "$GUI_BIN" "$g/files/usr/bin/prp-gui"
printf '[package]\nname = "prp-gui"\nversion = "0.1.0"\n\n[install]\nlayout = "system"\n' >"$g/manifest.toml"
( cd "$g" && tar -czf "$REPO/prp-gui-0.1.0-$ARCH.feather" manifest.toml files )

echo "assemble: packing prp-runtime …"
r="$WORK/pkg-runtime"; mkdir -p "$r/files"
cp -a "$PRP_ROOT/initramfs/rootfs/." "$r/files/"
printf '[package]\nname = "prp-runtime"\nversion = "0.1.0"\n\n[install]\nlayout = "system"\n' >"$r/manifest.toml"
( cd "$r" && tar -czf "$REPO/prp-runtime-0.1.0-$ARCH.feather" manifest.toml files )

echo "assemble: generating prp-base metapackage …"
m="$WORK/pkg-base"; mkdir -p "$m"
deps=$(printf '"%s", ' "${PRP_BASE_DEPS[@]}"); deps="${deps%, }"
printf '[package]\nname = "prp-base"\nversion = "0.1.0"\ndepends = [%s]\n\n[install]\nlayout = "meta"\n' "$deps" >"$m/manifest.toml"
( cd "$m" && tar -czf "$REPO/prp-base-0.1.0-$ARCH.feather" manifest.toml )

echo "assemble: signing + indexing …"
"$GENKEY" "prp-build-${ARCH}" "$KEYS/k.pub" "$KEYS/k.sec" "prp build key" >/dev/null
export FTR_PUBKEY="$KEYS/k.pub"
for f in "$REPO"/*.feather; do "$SIGN" "$KEYS/k.sec" "$f" "$f.sig" "prp" >/dev/null; done
"$FTR" index --arch "$ARCH" --name prp "$REPO" >/dev/null
"$SIGN" "$KEYS/k.sec" "$REPO/index.toml" "$REPO/index.toml.sig" "prp index" >/dev/null

echo "assemble: installing prp-base into the stage …"
export FTR_DB_ROOT="$STAGE/var/lib/feather"
export FTR_CONFIG="$WORK/feather.conf"
printf '[[repos]]\nname = "prp"\nurl = "file://%s"\npubkey = "%s"\n' "$REPO" "$KEYS/k.pub" >"$WORK/feather.conf"
"$FTR" sync
"$FTR" install --root "$STAGE" --arch "$ARCH" prp-base

echo "assemble: done. staged rootfs:"
echo "$STAGE"
