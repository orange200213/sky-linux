# M8 kernel patches (ath11k)

Two patches on top of the M7 tree (`sky-mainline/kernel/linux-7.3`,
`78458d4ee` + the M1–M7 work). Both are needed for the WCN6750 bring-up; see
`docs/m8-wifi.md` for the measurements behind them.

| Patch | What |
|---|---|
| `0001-ath11k-wcn6750-wlfw-qmi-instance0.patch` | the wcn6750 hw params ask for WLFW QMI instance 0 instead of upstream's 3 — this board's stock firmware registers `version \| instance << 8 == 0x0001`, upstream looks for `0x0301` and never finds it |
| `0002-ath11k-ahb-remove-without-registered.patch` | backport of upstream `1a7bcf5324c8`; without it, unbinding a stalled wcn6750 panics in `ath11k_ce_cleanup_pipes()` |
| `0003-ath11k-wcn6750-snoc-mode.patch` | `qcom,snoc-ahb` opt-in: membase registers instead of the QMI `DEVICE_INFO` BAR, per-copy-engine GIC interrupts instead of MSI, and the WLFW MSA handshake (`msa_mem_info` + `msa_ready`, ported from ath10k) before cap/BDF |

Apply:

```bash
cd <kernel tree>
git am kernel/patches-m8/000*.patch
```

Build (in-tree, host cross compiler, config from M7):

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image
```

and check that `include/generated/utsrelease.h` / the banner changed build
number — a failed build silently keeps the previous `Image`.

Not yet patched, but required for the SNOC/adrastea variant (see
`docs/m8-wifi.md` §2.3 and §6): direct-MMIO register access
(`hybrid_bus_type = false` + `ath11k_ahb_hif_ops_ipq8074`), the interrupt-name
mapping for the non-hybrid IRQ path, and possibly `wlfw_msa_mem_info` /
`wlfw_msa_ready`.
