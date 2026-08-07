# Bringing up the touchscreen on PRP — Xiaomi Redmi Note 8 (ginkgo)

Written so the next person can pick this up without re-deriving anything, and
without re-losing the time already spent on the parts that look like bugs and
are not.

**Short version:** the driver exists, is finished, and is already published in
the kernel PRP pins. It works on **both** test units. What is not yet
demonstrated is touch working *inside a PRP session* — that is the job.

---

## 1. Status

| Piece | State |
|---|---|
| NT36672A SPI driver | **Done.** 1051 lines, 12 commits of hardware iteration. |
| Driver published | **Yes** — in the exact commit the PRP port pins. |
| DTS touch node | **Yes** — in `sm6125-xiaomi-ginkgo-common.dtsi`, same commit. |
| Kernel config | **Yes** — `CONFIG_TOUCHSCREEN_NT36672A=y` in `config-prp`. |
| Firmware baked into PRP | **Yes** by config; **verify at build time** (§6). |
| Works on spare `ce0568ba` | Yes — worked from early on. |
| Works on daily driver `18002ff0` | Yes, as of `ec4e858ec`. ~46 irq/s vs vendor ~89/s. |
| **Touch inside a PRP session** | **Not confirmed. This is the open item.** |

There is no known driver bug to fix. Treat this as an integration and
verification task, and only go back into the driver if PRP-specific evidence
points there.

---

## 2. Where everything lives

    kernel source (canonical)   github.com/PeacockProject/linux_sm6125_ginkgo  branch `ginkgo`
                                head = db3f159aa32c11688ee4873a6218ea113052fee6
    kernel work tree (local)    ~/Documents/skunk/ginkgo/mainline-linux  branch `ginkgo-mainline`
    driver                      drivers/input/touchscreen/nt36672a-ts.c
    DTS node                    arch/arm64/boot/dts/qcom/sm6125-xiaomi-ginkgo-common.dtsi:737
    PRP kernel port             peacock-ports/device/linux-xiaomi-ginkgo-prp   (package.toml + config-prp)
    PRP device profile          PRP/configs/xiaomi-ginkgo.env
    firmware port               peacock-ports/device/firmware-xiaomi-ginkgo
    incremental kernel build    PRP/scripts/dev-kernel.sh

**The local tree and the published branch are different lineages** — the local
branch is based on torvalds/linux and does not contain commit `db3f159` at all.
Do not be alarmed by that: for every touch-relevant file the two are byte
identical (`nt36672a-ts.c`, `Kconfig`, `Makefile`, the ginkgo DTS). Verified by
hashing both sides. If you change the driver locally you must publish it and
re-pin, because the port builds from the tarball URL, not from your working
tree.

`dev-kernel.sh` defaults `PORT` to `~/Desktop/peacockos/...`, which is another
machine's layout. Set `PORT=`/`BUILD=` explicitly.

---

## 3. What the part is, and why it is awkward

NT36672A is a **TDDI** controller: touch and display are the same silicon, on
the same die as the panel of the same name. Two consequences drive everything
below.

**It is a host-download part.** It has no flash. The touch firmware
(`novatek/nt36672a-ginkgo.fw`, named by `firmware-name` in the DTS) is streamed
into its SRAM over SPI on every boot. No firmware file means no touch at all —
not degraded touch, none. It is wired on `spi2`, a QUPv3/GENI serial engine,
with reset on gpio87 and irq on gpio88.

**The download must not race the panel.** Because the die is shared, running the
download from probe collides with the panel driver's reset sequence. The vendor
driver queues it 5 s after probe with the comment *"please make sure boot update
start after display reset(RESX) sequence"*; a stock Android boot shows the
download landing at 5.68 s against a probe that ended at 0.66 s. Mainline's
panel comes up later still, so **this driver registers the input device at probe
and does the download from a delayed work at 15 s** (`dcc4c6615`).

That 15 s is the single most important fact for PRP — see §5.

---

## 4. Hardware knowledge that cost real time — do not re-derive

Every item here is from a commit message on the branch; each was established on
hardware.

1. **SPI fast-read inserts an extra dummy *bit*.** Once the downloaded firmware
   starts it enables fast-read, and every byte afterwards arrives shifted up by
   one, its top bit carried in the previous byte's last bit. The reset-complete
   byte reads `0x40` where the true value is `0xa0`. The fix is to read one
   extra byte and **undo the shift**, not to disable fast-read per read —
   poking a bootloader register every 10 ms while the firmware is booting
   corrupts the download on some panels (`6ca3b79b0`).
2. **GPI-DMA is fine.** An early commit blamed geni GPI-DMA for corrupting large
   SRAM writes and split the download into FIFO-sized chunks. That was a
   misdiagnosis of the fast-read misalignment. Full 63K chunks work and are much
   faster; `CONFIG_QCOM_GPI_DMA=y` is correct (`96444fb8b`).
3. **The bootloader register bank is not always readable.** On some display
   assemblies it returns canned per-page data: the reset-complete byte reads
   `0x88` (valid under no bit alignment), neighbours read `0xff` plus filler.
   **This is not evidence the controller is dead** — reading the same addresses
   through the vendor driver's SPI passthrough on a phone whose touch
   demonstrably works returns the same values, while the trim ROM reads fine in
   the same command. The vendor driver hits this too, logs `FW info is broken!`,
   retries, gives up, falls back to defaults, and touch works. Treat it as
   advisory (`d202589ab`, `ec4e858ec`).
4. **Do not print verdicts on those reads.** Comparing readback against expected
   values and printing `ilm MISMATCH` *"is what made a working download look
   corrupt for a long time"* (`20cea0683`). Dump raw bytes, state that they may
   be canned.
5. **`BLD_CRC_EN` must be read-modify-written**, not written as a bare
   `BIT(7)`. The low bits are panel specific, not scratch: one assembly reads
   `0x00` there so a bare write happens to be right, another has them set and a
   bare write clears them (`fd65458e6`, reversing `ba81538fd`).
6. **The memory map is selected at runtime from the trim ROM.** Several Novatek
   parts share the protocol with bootloader registers at different addresses,
   and *a Redmi Note 8 with a replaced screen can carry a different part*.
   NT36675/NT36672A/NT36772/NT36525 are supported; NT36676F is recognised and
   rejected (it boots from its own flash, has no BOOT_RDY, cannot be
   host-downloaded). ginkgo reports NT36672A/hw_crc 1 (`bfe59ec22`).
7. **Do not write POR_CD on parts whose map lacks it** — NT36672A does not
   document it, and forcing that path writes `0xa0` to address 0 (`e9eda810f`).
8. **Send zeros on MOSI during reads.** SPI is full duplex; stale firmware bytes
   left in the tx buffer were being driven at the chip while it replied
   (`bfe59ec22`).
9. **The downstream driver *does* pulse reset** — an earlier comment claimed it
   did not. Pulsing it the same way (held across regulator enable, released,
   20 ms settle) was tested and did **not** fix the post-download reset poll, so
   the simpler SPI-only reset is kept. Recorded so nobody re-tries it expecting
   a fix (`ec393a833`).
10. **Regulator load votes and pinctrl matter.** Downstream's display-stack load
    levels (62/100/100 mA on vddio/lab/ibb, allow-set-load on l9a) keep the RPM
    from leaving the touch VDDIO LDO in low-power mode during the download; the
    GENI serial engine is pinned awake (downstream does this via
    `pm_runtime_get`); TP_RST/TP_INT are driven at 8 mA with pull-up rather than
    2 mA floating (`ba81538fd`).

---

## 5. What PRP specifically has to get right

### 5.1 The 15-second download delay is the main integration risk

The input device is registered at **probe**, but no touch events exist until the
delayed download completes at **~15 s**. So a `/dev/input/eventN` node with the
right ABS bits appears early and is silent for a long time.

PRP already survives this by accident rather than by design, and it is worth
confirming rather than assuming:

- `gui/prp_gui.c` retries attaching every 1000 ms (`g_touch_retry_due_ms`) and
  logs `prp-gui: touch input attached late: ...`, so a late device is picked up.
- But because the node exists *immediately*, PRP will attach at once and then
  see nothing for ~15 s. **The user gets an unresponsive wizard for that
  window.** Decide whether that is acceptable or whether the wizard should show
  a "waiting for touch" state.
- If PRP's boot-to-GUI is faster than 15 s (it is), this is guaranteed to be
  visible on every boot.

Do not "fix" this by shortening the delay without re-reading §3 — the delay
exists because the download races panel bring-up, and mainline's panel is
*later* than downstream's, which is why it is 15 s and not the vendor's 5 s.

### 5.2 Device picking already matches

The driver registers `ts->input->name = "NVT36672A Touchscreen"`.
`score_touch_name()` in `gui/prp_gui.c` scores +6 for a name containing
`touch` (case-insensitive), and the node advertises `ABS_MT_POSITION_X/Y`, so it
is selected on both counts. No change needed — but if the name is ever changed
in the driver, check that scorer.

### 5.3 Firmware must actually be in the image

Chain, all of which currently agree — verify rather than trust:

    DTS firmware-name           novatek/nt36672a-ginkgo.fw
    firmware-xiaomi-ginkgo      lib/firmware/novatek/nt36672a-ginkgo.fw
    PRP FIRMWARE_INCLUDE        "ath10k novatek qcom/sm6125"
    assemble-rootfs-feather.sh  copies $FIRMWARE_INCLUDE subtrees from the tarball

`assemble-rootfs-feather.sh` prints
`assemble: WARN FIRMWARE_INCLUDE path not in tarball: <x>` and **carries on** if
a path is missing. That warning is the difference between working touch and no
touch — grep the build log for it.

Note also `qcom/sm6125/qupv3fw.elf` is in `FIRMWARE_INCLUDE` deliberately: the
touch controller hangs off a QUP engine, and the ginkgo firmware README warns
that reading the revision register of an engine the bootloader never programmed
hangs the config bus hard with no console output. 64 KiB of insurance on the
touch path.

The touch firmware is **baked in**, not runtime-mounted, so it is unaffected by
the `FW_RUNTIME_PART` / `firmware_class.path` mechanism (that is for the ~72 MiB
modem/ADSP images) and works in a recovery session where `start_firmware_runtime`
returns early.

---

## 6. How to verify, in order

1. **Confirm the built kernel has the driver.** `CONFIG_TOUCHSCREEN_NT36672A=y`
   in `config-prp` (line ~3255) and the port pinned at `db3f159`.
2. **Confirm the firmware landed.** Build log has no `WARN FIRMWARE_INCLUDE path
   not in tarball`, and `/lib/firmware/novatek/nt36672a-ginkgo.fw` exists in the
   assembled rootfs.
3. **Boot PRP and watch dmesg** for the probe, then the download ~15 s later.
   The failure dump (if any) prints raw chip id, ILM SRAM, checksum block and
   event buffer — **these may be canned values, not a verdict** (§4.3/4.4).
4. **Confirm events reach evdev**, not just that the node exists. The useful
   measure is interrupt rate under sustained contact:
   - noise floor before it works: ~0.2/s
   - this driver, previously-failing assembly: ~46/s
   - vendor kernel reference, same panel: ~89/s
   Anything in the tens with real coordinates means the download succeeded.
5. **Confirm the PRP wizard responds** — that is the actual goal, and it is the
   step that has never been demonstrated.
6. **Test both phones.** They have different display assemblies and have behaved
   differently at every stage (§7).

---

## 7. The two units are not interchangeable

- `ce0568ba` — spare. Worked first and kept working. **Every genuine
  `TOUCH RESULT: PASS` in the older logs is this unit.**
- `18002ff0` — daily driver. Failed its download 100% with identical code for a
  long investigation. Its display assembly is the one with the canned bootloader
  register bank. Fixed by `d202589ab` + `ec4e858ec`; now downloads and reports
  touch at ~46 irq/s.

A result from one unit is not a result for the other. When something works,
say which phone.

Also relevant from the MinKernel side: the spare **does not support
`fastboot boot`** (returns OKAY and does nothing) — flash and reboot instead.
The daily driver does support it.

---

## 8. Repo state you are inheriting (read before pushing)

Nothing here is broken, but it is diverged, and one part needs a human decision.

- **PRP** — pulled and rebased onto `a7e1dfd` ("prp bullshit", Mahiko), which
  added the ginkgo device profile, the runtime-firmware mechanism, and
  `dev-kernel.sh`. **6 local commits unpushed** on top.
- **peacock-ports** — `origin/main` has `8c39bc7` ("broken bullshit"), **not yet
  merged locally**. It is purely additive (48,777 insertions, **0 deletions** —
  verified against the merge base; the scary deletion count in a plain
  `HEAD..origin/main` diff is a divergence artifact, not lost work). It adds the
  `linux-xiaomi-ginkgo-prp` port. **13 local commits unpushed.**
  - ⚠ **Add/add conflict pending.** Both sides independently create
    `device/linux-xiaomi-ginkgo/package.toml`, `device/xiaomi-ginkgo/device.toml`
    and `device/xiaomi-ginkgo/package.toml`. Merging or rebasing will conflict on
    all three. This is a content decision, not a mechanical one.
  - The new port also carries four scratch configs — `config-prp.drmmsm`,
    `.drmmsm.working`, `.sweep.bak`, `.unswept` (~38k lines). Decide whether they
    belong in the repo.
- **Peacock** — `origin/master` has `abab95d` ("broken bullshit"): `builder.go`
  (+46/-9) and a `peacock-ports` submodule bump. **3 local commits unpushed.**
- **Kernel repo** — unchanged; `ginkgo` still `db3f159`, which is what the port
  pins and which already contains the finished driver.

Both of Mahiko's commits are messaged "broken bullshit" — treat them as parked
WIP, not signed-off work.

---

## 9. If you do end up back in the driver

Read §4 first — most of what looks wrong there is documented as *not* wrong.
Specifically, before concluding a download failed:

- a reset-complete byte of `0x88`, or `0xff` neighbours, proves nothing;
- `ilm MISMATCH`-style output no longer exists precisely because it lied;
- decide success by **whether the controller reports touches**, nothing else.

Build iteration: `dev-kernel.sh` exists because `peacock build-packages` re-runs
its whole pipeline (~22 min) even for a one-symbol config change, while kbuild
in the existing build chroot takes seconds. Use `peacock build-packages` for the
real packaged artifact.

Commit hygiene: no Claude/Anthropic co-author or "Generated with" trailers.
(Note the existing kernel commits carry `Co-Authored-By: Claude ...` trailers —
that is Mahiko's tree and its own convention, not this repo's.)
