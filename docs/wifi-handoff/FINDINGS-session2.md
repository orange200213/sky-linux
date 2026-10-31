# M8 handoff — second session: what the first handoff got wrong

The first handoff (`HANDOFF.md`) assumed the `wifi@22800000` node inherited from
the ROCKNIX RG55G1 file had to be *replaced* by a hybrid/PCIe `wcn6750` node and
that only "firmware + `status = okay` + `qcom,rproc`" were missing. Both
assumptions turned out to be wrong; read `docs/m8-wifi.md` for the full record.

Corrections:

1. **The existing node was already correct.** The stock run-time DT has an
   enabled `qcom,icnss@22800000` node with `reg = <0x22800000 0x800000>`,
   `reg-names = "membase"`, 12 interrupts, `iommus = <&apps_smmu 0x4c0 0x1>` and
   the same four supplies (`pm6450 l11/l23/l14`, `pm8010 l7`) that the RG55G1
   derived node already had. The chip on sky is the **SNOC** (vendor
   `ADRASTEA`) variant, not the hybrid/PCIE one.
2. **`qcom,rproc` and the wlan smp2p state are required** — that part of the
   handoff was right, and both are in `dts/sm4450-xiaomi-sky.dts` now.
3. **The firmware name must end in `.mdt`** (`qcom/sm4450/xiaomi-sky/wpss.mdt`);
   the `.mbn` fallback only works because the split-segment names are built by
   overwriting the last three characters of the name.
4. **WPSS never auto-boots**: `auto_boot` fires at ~0.4 s, before `switch_root`.
   `/init` starts it (rootfs helper), exactly like the M6 ADSP retry.
5. **The WLFW QMI instance is 0 on this firmware**, not upstream's 3 — ath11k
   without patch 0001 waits forever for a service it will never match.
6. **`QMI_WLANFW_DEVICE_INFO` is not supported by this firmware (error 94)**:
   the hybrid-bus path mainline implements cannot work here, no matter what the
   DT says. A SNOC port inside ath11k is the remaining work.

State of the device: WPSS runs, ath11k probes, the QMI WLFW handshake completes,
`wlan0` does not exist. Kernel `#62` (with patches 0001/0002) and the hybrid
DTB are flashed; `wifi-backup/boot/boot_a-original.img` restores M7.
