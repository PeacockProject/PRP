# PRP SSH Bug Postmortem

**Date:** 2026-03-10
**Target:** OPPO A16 (MT6765) running PRP recovery image with Dropbear SSH over USB-Ethernet (u_ether gadget)
**Symptom:** SSH sessions hang indefinitely; non-interactive commands (`/bin/true`, `sleep 2`) never exit

---

## Two Bugs, Stacked

### Bug 1 — `prp-shell-wrapper` ignored its arguments

**File:** `prp/overlay/rootfs/usr/bin/prp-shell-wrapper`

When Dropbear runs a non-interactive command (e.g. `ssh host /bin/true`), it invokes the user's login shell as:

```
prp-shell-wrapper -c /bin/true
```

The original wrapper unconditionally ran `exec /bin/sh -i` and discarded `$@` entirely:

```sh
# BUG: always launches interactive shell, ignores arguments
exec /bin/sh -i
```

So every SSH command — `/bin/true`, `id`, `echo OK`, `sleep 2` — silently launched a full interactive shell and blocked waiting for input. All early debugging results were invalid: the hang was not a Dropbear protocol issue, it was a shell waiting for `Ctrl-D`.

**Fix:**

```sh
if [ "$#" -gt 0 ]; then
    exec /bin/sh "$@"
else
    exec /bin/sh -i
fi
```

---

### Bug 2 — TCP checksum corruption in the `u_ether` kernel driver

**File:** `drivers/usb/gadget/function/u_ether.c`

After fixing the wrapper, a clean `sleep 2` test still stalled. Host-side `tcpdump -vv` revealed the root cause:

```
172.16.42.1.22 > 172.16.42.20: seq 1531:1619  cksum 0x7553 (incorrect -> 0x1b18)
172.16.42.1.22 > 172.16.42.20: seq 1531:1619  cksum 0x6ca2 (incorrect -> 0x1267)  ← retransmit 1
172.16.42.1.22 > 172.16.42.20: seq 1531:1619  cksum 0x6b23 (incorrect -> 0x10e8)  ← retransmit 2
172.16.42.1.22 > 172.16.42.20: seq 1531:1619  cksum 0x6863 (incorrect -> 0x0e28)  ← retransmit 3
172.16.42.1.22 > 172.16.42.20: seq 1531:1619  cksum 0x62e3 (incorrect -> 0x08a8)  ← retransmit 4
```

The 88-byte SSH exit-status+close packet arrived at the host with an **incorrect TCP checksum** every single time — original send plus four TCP retransmits. The host kernel silently dropped all five. The error was a **constant offset of 0x5a3b** across all transmissions, ruling out random corruption and pointing to a systematic bug in the TX path.

The driver had two interacting problems:

#### Problem A — hrtimer pacing gate placed before `dev->wrap()`

An earlier experiment added a 100ms TX rate-limit gate into `eth_start_xmit()`:

```c
/* BUG: fires before dev->wrap() is called */
now = ktime_get();
min_gap = ms_to_ktime(100);
if (dev->last_tx_ktime != 0) {
    gap = ktime_sub(now, dev->last_tx_ktime);
    if (ktime_before(gap, min_gap)) {
        netif_stop_queue(net);
        hrtimer_start(&dev->tx_pace_timer, ...);
        return NETDEV_TX_BUSY;   /* skb returned un-wrapped */
    }
}
/* dev->wrap() is only called below here */
skb = dev->wrap(dev->port_usb, skb);
```

When the pacing gate fired and returned `NETDEV_TX_BUSY`, the skb was returned to the qdisc in an un-wrapped, mid-flight state. During the 100ms delay before the hrtimer woke the queue, TCP's own retransmit logic could inject a second copy of the same data — resulting in two copies of the exit-status+close competing through the TX path with inconsistent skb state on resubmit.

**Fix:** Remove the hrtimer pacing gate entirely. Restore the original conditional `netif_wake_queue()` in `tx_complete()`. TX pacing was addressing the wrong layer anyway.

#### Problem B — `NETIF_F_SG` advertised but non-multi_pkt path does not handle fragments

The driver sets:

```c
net->features |= NETIF_F_GSO | NETIF_F_SG;
net->hw_features |= NETIF_F_GSO | NETIF_F_SG;
```

`NETIF_F_SG` tells the kernel the driver can handle scatter-gather (fragmented skbs, where header and payload live in separate memory pages). But the non-multi_pkt TX path does:

```c
/* BUG: req->buf covers only the linear head */
length = skb->len;      /* total length, including page fragments */
req->buf = skb->data;   /* only the LINEAR head pointer */
req->context = skb;
```

For any skb with `skb->data_len > 0` (payload in page fragments), the USB DMA engine reads `skb->len` bytes starting from `skb->data` — but only the first `skb->headlen()` bytes are valid TCP data. The remainder is garbage from adjacent memory. The TCP checksum was computed over the *real* payload, but the host received garbage bytes in its place. Checksum mismatch → silent drop.

**Fix:** Add `skb_linearize()` at the top of `eth_start_xmit()`, before any other processing:

```c
if (skb_linearize(skb)) {
    dev_kfree_skb_any(skb);
    return NETDEV_TX_OK;
}
```

This forces the kernel to consolidate all fragments into the linear head before the driver touches the skb. The NETIF_F_SG / non-multi_pkt mismatch becomes impossible.

---

## Why It Was Hard to Spot

Only the SSH exit-status+close packet triggered Bug 2. All handshake, key exchange, auth, and channel-data packets transferred correctly. The exit-status+close is a small packet (~34 bytes of TCP payload) sent by Dropbear immediately after SIGCHLD fires when the remote command exits. This specific write path — combined with the hrtimer's 100ms NETDEV_TX_BUSY on a packet sent immediately after the preceding EOF — was the only codepath that produced a fragmented skb reaching the non-multi_pkt USB submission path.

The constant checksum error (same value on every retransmit of the same packet) was the key diagnostic signal: random hardware corruption would vary; a fixed offset means the same bytes are systematically wrong every time.

---

## Fix Summary

| File | Change |
|------|--------|
| `prp/overlay/rootfs/usr/bin/prp-shell-wrapper` | Check `$#`, pass `"$@"` for non-interactive exec |
| `drivers/usb/gadget/function/u_ether.c` | Remove hrtimer pacing gate; add `skb_linearize()` at top of `eth_start_xmit()` |
| `drivers/usb/gadget/function/u_ether.h` | Remove `tx_pace_timer` and `last_tx_ktime` fields |

Both `u_ether` changes are permanent correctness fixes, not workarounds. Neither introduces performance regressions for normal traffic.
