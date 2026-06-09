# Dropbear Debug Backup

Date: 2026-03-08

This backup captures the current Dropbear debugging work for Oppo A16 PRP SSH.

Included:
- `build-dropbear.sh`: current patching/build script
- `build-dropbear.diff`: git diff for the script against PRP repo HEAD
- `generated-src/packet.c`
- `generated-src/common-channel.c`
- `generated-src/svr-chansession.c`
- `generated-src/dbutil.c`

These generated sources are from:
- `prp/out/oppo-a16/tools/dropbear-build/dropbear-2024.86-armv7/src/`

Purpose:
- preserve the exact instrumented Dropbear state before trying alternative builds/toolchains
- allow reapplying the current instrumentation without depending on the live build directory
