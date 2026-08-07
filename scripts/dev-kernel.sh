#!/usr/bin/env bash
# dev-kernel.sh — incremental ginkgo kernel build, bypassing `peacock`.
#
# `peacock build-packages` re-runs its whole pipeline per invocation and costs
# ~22 min even for a one-symbol config change. For iteration that is dead time:
# the source tree is already unpacked in the build chroot with its .o files
# intact, so kbuild can do the right thing in seconds if we just call make.
#
# This does exactly what the port's build() does, nothing more:
#   .config <- the port's config-prp, make Image.gz + the ginkgo dtb, then
#   concatenate them (ABL needs the DTB appended).
#
# Use `peacock build-packages` for the real, packaged artifact; use this to
# iterate. Output: $BUILD/zImage
set -euo pipefail

PORT="${PORT:-$HOME/Desktop/peacockos/Peacock/peacock-ports/device/linux-xiaomi-ginkgo-prp}"
BUILD="${BUILD:-$(ls -d "$HOME"/.local/var/peacock/build-chroot/x86_64/build/linux-xiaomi-ginkgo-prp-*/ | head -1)}"
CHROOT="${CHROOT:-$HOME/.local/var/peacock/build-chroot/x86_64}"
JOBS="${JOBS:-$(nproc)}"
DTB="arch/arm64/boot/dts/qcom/sm6125-xiaomi-ginkgo.dtb"

[[ -d "$BUILD" ]] || { echo "no build tree: $BUILD" >&2; exit 1; }
# The tree lives inside the chroot, so build inside it — the cross toolchain is
# installed there, not on the host.
INNER="/build/$(basename "${BUILD%/}")"

echo "==> syncing config from $PORT/config-prp"
sudo cp "$PORT/config-prp" "$BUILD/.config"
sudo sed -i -e '/^CONFIG_CC_VERSION_TEXT=/d' -e '/^CONFIG_CC_IS_/d' \
            -e '/^CONFIG_GCC_VERSION=/d' -e '/^CONFIG_CLANG_VERSION=/d' \
            -e '/^CONFIG_AS_IS_/d' -e '/^CONFIG_AS_VERSION=/d' \
            -e '/^CONFIG_LD_IS_/d' -e '/^CONFIG_LD_VERSION=/d' \
            -e '/^CONFIG_LLD_VERSION=/d' -e '/^CONFIG_CC_HAS_/d' \
            -e '/^CONFIG_CC_CAN_/d' -e '/^CONFIG_TOOLS_SUPPORT_RELR=/d' "$BUILD/.config"

run_in_chroot() { sudo chroot "$CHROOT" /bin/sh -c "cd $INNER && $*"; }

echo "==> olddefconfig"
run_in_chroot "yes '' | make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig >/dev/null"

echo "==> make Image.gz + dtb (-j$JOBS)"
time run_in_chroot "make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$JOBS Image.gz qcom/sm6125-xiaomi-ginkgo.dtb"

echo "==> appending DTB (ABL matches qcom,msm-id/board-id in it)"
run_in_chroot "cat arch/arm64/boot/Image.gz $DTB > zImage"
sudo chown "$(id -u):$(id -g)" "$BUILD/zImage"
echo "==> $BUILD/zImage  ($(stat -c %s "$BUILD/zImage") bytes)"
