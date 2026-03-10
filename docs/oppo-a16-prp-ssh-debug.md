# OPPO A16 PRP SSH Debug Status

## Current State

The SSH issue on Oppo A16 is no longer an active blocker.

Current known-good state:
- PRP boots on A16
- monolithic initramfs mode works
- framebuffer output works
- RNDIS works
- Dropbear SSH works
- interactive shell works

The current working configuration is:
- `prp/configs/oppo-a16.env`
  - `DROPBEAR_PATCH_MODE="instrumented"`
  - `DROPBEAR_CC_MODE="muslcc"`
  - `DROPBEAR_OPT_LEVEL="0"`

Current working image at the time of this note:
- `prp/out/oppo-a16/prp-oppo-a16-recovery.img`
- sha256: `cb52d398b2bdbd96c944c9bce2cc7ff7882a316e5f24f9bd4bfde97177245302`

## What Was Originally Failing

Observed failure mode from earlier runs:
- TCP connect to `172.16.42.1:22` succeeded
- SSH KEX/auth succeeded
- `pty-req` succeeded
- interactive shell never appeared
- OpenSSH hung after:
  - `PTY allocation request accepted on channel 0`

Later instrumentation proved that server-side execution was actually happening:
- shell wrapper executed
- `/bin/sh -i` was executed
- PTY output was being read
- channel data packets were being built and written

So the problem was never “shell missing” or “SSH daemon not starting”.

## A16 PRP State

What already works:
- initial PRP bring-up on A16
- monolithic initramfs mode
- framebuffer rendering
- PRP GUI
- RNDIS gadget bring-up
- telnet shell helpers
- Dropbear SSH

Relevant config/image files:
- `prp/configs/oppo-a16.env`
- `prp/out/oppo-a16/prp-oppo-a16-recovery.img`

## Monolithic Initramfs

Monolithic mode is enabled and works:
- `PRP_MONOLITHIC_INITRAMFS="1"`
- separate PRP rootfs mount is skipped
- overlay-stage contents are copied into initramfs at build time

Relevant files:
- `prp/scripts/common.sh`
- `prp/scripts/build-initramfs.sh`
- `prp/scripts/build-overlay.sh`
- `prp/initramfs/rootfs/init`

## Framebuffer Fix

PRP initially booted but did not render visibly because the A16 MTK framebuffer is double-buffered with a nonzero active `yoffset`.

The fix was outside the `prp/` repo:
- `peacock-ports/base/peacock-splash/splash.c`

What changed there:
- framebuffer writes use `xoffset` / `yoffset`
- writes are mirrored across virtual pages when needed
- `msync()` is called
- `FBIOPAN_DISPLAY` is issued

That made PRP visible on-screen.

## Recovery / Flashing Constraints

For this device, PRP is flashed with `mtkclient`:
- command pattern:
  - `sudo mtk w recovery prp/out/oppo-a16/prp-oppo-a16-recovery.img`

The device owner prefers this over adb flashing for PRP on the daily driver.

## SSH Debugging Done

### 1. Verified packaging/runtime basics

Checked that the monolithic image contains:
- `/usr/sbin/dropbear`
- `/usr/sbin/dropbearkey`
- `/usr/bin/prp-svc-ssh`
- `authorized_keys`

Checked shell binaries:
- `/bin/sh` exists
- `/bin/ash` exists
- `/usr/bin/sh` exists
- all are BusyBox-backed
- `/bin/bash` and `/usr/bin/bash` do not exist

This ruled out “missing shell”.

### 2. Instrumented PRP shell startup scripts

Files:
- `prp/overlay/rootfs/usr/bin/prp-svc-ssh`
- `prp/overlay/rootfs/usr/bin/prp-login-sh`
- `prp/overlay/rootfs/usr/bin/prp-shell-wrapper`

Added `[prp-ssh]` logging to:
- `/dev/kmsg`
- `/dev/console`
- `/dev/ttyS0`

Purpose:
- prove Dropbear startup
- prove shell wrapper execution

Result:
- wrapper executes
- `/bin/sh -i` exec path is reached

### 3. Instrumented Dropbear session and packet paths

Dropbear is rebuilt via:
- `prp/scripts/build-dropbear.sh`

Instrumentation was added at various points to:
- `src/dbutil.c`
- `src/svr-chansession.c`
- `src/common-channel.c`
- `src/packet.c`

Markers used included:
- `[prp-ssh] execchild enter`
- `[prp-ssh] run_shell_command ...`
- `[prp-ssh] chansessionrequest type=... wantreply=...`
- `[prp-ssh] send_msg_channel_success ...`
- `[prp-ssh] send_msg_channel_data ...`
- `[prp-ssh] pkt: encrypt_packet ...`
- `[prp-ssh] pkt: write_packet ...`
- `[prp-ssh] pkt: write_packet iov0 ...`
- `[prp-ssh] pkt: write_packet iov1 ...`

### 4. Proved the server-side shell path was actually working

Server-side logs proved all of these were happening:
- `pty-req` handled successfully
- `shell` handled successfully
- `CHANNEL_SUCCESS` packets sent for both
- PTY parent/child set up
- shell wrapper entered
- `/bin/sh -i` executed
- PTY output read
- `CHANNEL_DATA` packets built and written

This ruled out:
- missing shell
- failed exec
- missing `pty-req` reply
- missing `shell` reply

### 5. Captured traffic and compared it against Dropbear's write buffers

Host-side pcap was taken on the RNDIS interface with:
- `tcpdump -i enp0s20f0u3 -s 0 -w /tmp/prp-ssh.pcap host 172.16.42.1 and port 22`

Device-side packet bytes were logged from Dropbear immediately before `writev()`.

Result:
- the on-wire encrypted payload matched the exact `iov0` bytes logged before `writev()`

This ruled out:
- kernel/RNDIS payload corruption
- post-`writev()` corruption in the transport path

That was an important split: the kernel is not the primary problem here.

## Toolchain / Build Matrix Results

### Zig + vanilla Dropbear

Config:
- `DROPBEAR_PATCH_MODE="vanilla"`
- `DROPBEAR_CC_MODE="zig"`

Result:
- still hung

This ruled out “our Dropbear instrumentation caused the bug”.

### Zig + instrumented Dropbear

Result:
- worked on some runs
- failed on others

Interpretation:
- likely timing/layout-sensitive behavior
- instrumentation changed codegen/layout enough to sometimes mask the bug

### musl GCC wrapper initially

Package installed first:
- `muslcc-arm-linux-musleabihf-cross-bin`

Initial result:
- unusable for static Dropbear
- linker could not find `-lc`

Reason:
- that toolchain install did not ship a usable static libc in its own sysroot

### Installed additional musl ARM sysroot package

Installed:
- `armhf-musl`

This provided:
- `/usr/lib/armhf-musl/include`
- `/usr/lib/armhf-musl/lib/libc.a`

Then `prp/scripts/build-dropbear.sh` was patched so muslcc mode uses:
- `CPPFLAGS="-I/usr/lib/armhf-musl/include"`
- `LDFLAGS="-static -L/usr/lib/armhf-musl/lib"`

### musl GCC + vanilla Dropbear

Config:
- `DROPBEAR_PATCH_MODE="vanilla"`
- `DROPBEAR_CC_MODE="muslcc"`
- `DROPBEAR_OPT_LEVEL="0"`

Result:
- still hung

This ruled out:
- Zig-only codegen bug as the sole explanation

### musl GCC + instrumented Dropbear

Config:
- `DROPBEAR_PATCH_MODE="instrumented"`
- `DROPBEAR_CC_MODE="muslcc"`
- `DROPBEAR_OPT_LEVEL="0"`

Result:
- SSH shell works

Observed working behavior:
- shell opens normally
- expected debug strings appear:
  - `__PRP_STDOUT_AFTER_DUP2__`
  - `__PRP_STDERR_AFTER_DUP2__`
  - `__PRP_SHELL_ENTER__`
  - `uid=0(root) gid=0(root) groups=0(root)`

## Current Interpretation

The situation is now:
- not a kernel/RNDIS corruption bug
- not a missing shell/runtime bug
- not simply our instrumentation causing it
- not purely “Zig is broken”

What the evidence supports:
- the bug is timing/layout-sensitive
- instrumentation can perturb it enough to make it work
- toolchain/codegen likely influences how often it appears
- but the root cause is not fully pinned down yet

This is best described as a heisenbug in the Dropbear/PRP runtime path.

## Current Safe Working Choice

For A16, the practical stable choice right now is:
- `DROPBEAR_CC_MODE="muslcc"`
- `DROPBEAR_PATCH_MODE="instrumented"`
- `DROPBEAR_OPT_LEVEL="0"`

That combination currently yields a working SSH shell.

## Relevant Files

### PRP config/runtime
- `prp/configs/oppo-a16.env`
- `prp/initramfs/rootfs/init`
- `prp/scripts/build-initramfs.sh`
- `prp/scripts/build-overlay.sh`

### SSH helpers
- `prp/overlay/rootfs/usr/bin/prp-svc-ssh`
- `prp/overlay/rootfs/usr/bin/prp-login-sh`
- `prp/overlay/rootfs/usr/bin/prp-shell-wrapper`

### Dropbear build logic
- `prp/scripts/build-dropbear.sh`

### Backup of debug Dropbear work
- `prp/docs/dropbear-debug-backup-20260308/`

### UART helper
- `experiments/tail_prp_uart.py`

## Incremental Reduction Results

### Packet.c instrumentation removal

Removed all 5 packet.c patches → **BROKEN**.
Added back helpers + encrypt_packet only (no write_packet logging) → **BROKEN**.
Restored full write_packet instrumentation → **WORKS** again.

Replaced write_packet hexdump with `usleep(2000)` (2ms) → **BROKEN**.
Changed to `usleep(50000)` (50ms) → **WORKS**.
Changed to `usleep(10000)` (10ms) → **BROKEN**.

Confirmed: the bug is a timing race, not a semantic issue. ~50ms before writev is required.

### TCP_NODELAY patch

Added `set_sock_nodelay(childsock)` to `svr-main.c` after `accept()`.
Removed the usleep. Result: **BROKEN**.

TCP_NODELAY alone is insufficient. It is kept as correct practice but does not fix the hang.

### Kernel-level investigation (u_ether, MTK QMU)

Investigated the u_ether `multi_pkt_xfer` TX aggregation path:
- `goto success` path buffers small packets without USB submission when `no_tx_req_used > TX_REQ_THRESHOLD (= 1)`
- Flush occurs via `tx_complete` when a prior request completes
- `req->no_interrupt` field is **ignored** by the MTK MUSB QMU driver — it always sets IOC=1 in every GPD
- `musb_g_giveback` properly releases `musb->lock` before calling the completion, so `tx_complete → usb_ep_queue → musb_gadget_queue` does not deadlock
- `qmu_done_tx` always calls `musb_g_giveback` for all completed GPDs

Analysis shows the aggregation path should self-flush via `tx_complete` within microseconds. The CHANNEL_SUCCESS pty-req and CHANNEL_SUCCESS shell packets are sent in separate select() iterations (~1-5ms apart), and CHANNEL_DATA arrives ~5-30ms after CHANNEL_SUCCESS shell (shell startup time). By any of these points, `no_tx_req_used` should be 0 and aggregation buffering should not occur.

**Root cause not fully identified.** The timing threshold of ~40-50ms points toward:
- TCP delayed-ACK cycle on the host (Linux default: 40ms)
- Some SoC-level bus or cache ordering effect on the ARM MT6765
- An interaction between the non-blocking socket write path and the RNDIS gadget TX queue state

## Current Safe Working Choice

For A16, the practical stable choice right now is:
- `DROPBEAR_CC_MODE="muslcc"`
- `DROPBEAR_PATCH_MODE="instrumented"`
- `DROPBEAR_OPT_LEVEL="0"`

With patches in `build-dropbear.sh`:
- `set_sock_nodelay(childsock)` in `svr-main.c` after accept (kept, correct practice)
- `usleep(50000)` before `writev()` in `write_packet()` in `packet.c` (workaround)
- encrypt_packet / channel / session tracing (diagnostic, kept for now)

## Kernel Patch (u_ether.c TX aggregation deferral)

File: `experiments/android_kernel_oppo_mt6765/drivers/usb/gadget/function/u_ether.c`
(hardlinked to `experiments/twrp/kernel/oppo/mt6765/drivers/usb/gadget/function/u_ether.c`)

Removed the inner `if (dev->no_tx_req_used > TX_REQ_THRESHOLD)` block in
`eth_start_xmit` (was lines 874-878) that deferred packets via `goto success`.

### What the removed block did

When `multi_pkt_xfer` is active and there are ≥ 2 TX requests in-flight
(`no_tx_req_used > TX_REQ_THRESHOLD`, where `TX_REQ_THRESHOLD = 1`), small
packets within the size/count limits were placed back into the `tx_reqs`
freelist HEAD **without calling `usb_ep_queue()`**. They were only submitted
later when `tx_complete` ran for a prior request.

### Why this is the likely root cause

SSH `CHANNEL_DATA` from Dropbear arrives ~5-30ms after `CHANNEL_SUCCESS shell`.
At that point both `CHANNEL_SUCCESS pty-req` and `CHANNEL_SUCCESS shell` may
still be in-flight at the USB hardware level, so `no_tx_req_used = 2 > 1`.
The `CHANNEL_DATA` packet (a small SSH payload) would be deferred. `tx_complete`
fires only when a prior request completes, but the host's TCP stack has already
ACKed those packets and the host TCP stack uses a 40ms delayed-ACK timer for
the SSH data itself. Result: `CHANNEL_DATA` is stuck for ~40ms, matching the
observed threshold exactly.

### Patch effect

Every packet now goes directly to `usb_ep_queue()` regardless of
`no_tx_req_used`. The `success:` label was also removed (it became dead code).

### How to rebuild and test

1. Rebuild the TWRP kernel:
   ```
   cd experiments/twrp
   source build/envsetup.sh
   lunch twrp_a16-eng
   mka bootimage
   ```
   Kernel lands at `out/target/product/a16/kernel`.

2. The PRP config already points to that path:
   `KERNEL_IMAGE="$PRP_ROOT/../experiments/twrp/out/target/product/a16/kernel"`

3. Build PRP as normal, flash with mtkclient, test SSH.

4. If SSH works without `usleep(50000)` in Dropbear, the kernel patch is
   confirmed as the fix. Remove the usleep workaround from `build-dropbear.sh`.

## Good Next Steps

1. Rebuild the TWRP kernel with the u_ether.c patch above.
2. Build PRP with the patched kernel, test SSH with and without the usleep workaround.
3. If SSH works without usleep, strip the timing workaround from build-dropbear.sh.
4. If SSH still fails with the kernel patch alone, fall back to: build the **broken** config (TCP_NODELAY, no usleep, no kernel patch), capture UART + `ssh -vvv`, confirm whether CHANNEL_DATA is logged before writev hang.
5. Once root cause is confirmed, strip unnecessary instrumentation from build-dropbear.sh.
