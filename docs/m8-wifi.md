# M8 — Wi-Fi (WCN6750 "adrastea" on SM4450)

> ## Status: ✅ works — through the out-of-tree `qcacld-3.0` stack
>
> There are two routes on this device and both are documented here:
>
> | Route | State |
> |---|---|
> | **`qcacld-3.0` ported to this mainline kernel** — [`sky-qcacld-3.0-mainline`](https://github.com/orange200213/sky-qcacld-3.0-mainline): Qualcomm's downstream WLAN driver (qcacld-3.0 + qca-wifi-host-cmn + fw-api) built as a module against our kernel, using `icnss2` and the vendor firmware from `NON-HLOS.bin`/`image/adrastea` | **works** — Wi-Fi is usable. **Still being optimised: it is a large downstream driver, so bugs can appear at any time** (association failures, firmware recovery, suspend/resume). Treat it as "working, not stable". |
> | **Pure mainline `ath11k`** with the SNOC/adrastea variant | **closed (2026-11-02)** — the bring-up reached `ath11k_core_qmi_firmware_ready()`, then hardware probing proved the APSS can only reach the SR_APSS block (`membase+0x20000..0x52000`) and 12 CE shadow slots: CE ring base/size and the UMAC/WBM/REO/TCL registers are unreachable, so ath11k's "host programs the rings" model cannot work on this platform. Evidence: `docs/wifi-handoff/FINDINGS-session9-shadow-reg-confirmed.md`; route decision: `docs/wifi-handoff/HANDOFF-qcacld-mainline.md`. |
>
> The rest of this file is the record of the mainline-`ath11k` attempt (hybrid vs SNOC,
> every experiment, every trap). It stays as evidence for why the qcacld route was chosen —
> it is *not* required to have working Wi-Fi.
>
> First on-device test of the qcacld modules (2026-11-04) found two integration gaps that are
> **not** driver bugs: 27 `wlfw_*_msg_v01_ei` symbols missing/not exported, and no
> `qcom,icnss` node in the DTS. See `docs/wifi-handoff/FINDINGS-qcacld-device-test-2026-11-04.md`.

Status at the end of the **mainline-`ath11k`** session (kept for the record): WPSS and the
WLFW QMI handshake work, the WLAN interface does not come up yet. The remaining blocker is a
chip-variant
mismatch inside mainline `ath11k`: this board's WLAN is the **SNOC** variant of
the WCN6750 (vendor compatible `qcom,icnss`, registers directly mapped at
`0x22800000`), while mainline's `qcom,wcn6750-wifi` support is written for the
**hybrid/PCIe** variant (register BAR reported by the firmware through
`QMI_WLANFW_DEVICE_INFO`). Our firmware answers that request with
`QMI_ERR_NOT_SUPPORTED` (94), so the hybrid path can never complete on sky.

Everything below was measured on the device; the artifacts are in
`/home/orange/redmi125g/wifi-backup/` and in this repo.

---

## 1. What works now

```
remoteproc1 (wpss)                 running      firmware from NON-HLOS.bin loads
ath11k 17210040.wifi               probes, gets smp2p / iommu / irqs
QMI WLFW                           connected   (after patch 0001)
firmware device info               chip_id 0x4130 chip_family 0x4007
                                   fw_version 0x343b815a
                                   build CI_WLAN.HL.3.4.3-00346.1-QCAHLSWMTPLZ-23
QMI_WLANFW_DEVICE_INFO             -> result 1 (failure), error 94 = NOT_SUPPORTED
wlan0                              not created yet
```

The kernel log of a good boot:

```
remoteproc remoteproc1: powering up wpss
remoteproc remoteproc1: Booting fw image qcom/sm4450/xiaomi-sky/wpss.mdt, size 4684
remoteproc remoteproc1: remote processor wpss is now up
ath11k 17210040.wifi: wcn6750 hw1.0
ath11k 17210040.wifi: chip_id 0x4130 chip_family 0x4007 board_id 0xff soc_id 0x406a0000
ath11k 17210040.wifi: fw_version 0x343b815a fw_build_timestamp 2023-06-23 08:24 ...
ath11k 17210040.wifi: qmi device info request failed: 1 94
ath11k 17210040.wifi: failed to request qmi device info: -22
ath11k 17210040.wifi: qmi failed to download BDF:-22
```

## 2. Root causes found

### 2.1 WPSS firmware name (fixed)

`firmware-name` was `qcom/sm4450/xiaomi-sky/wpss.mbn`, but the stock image is a
split MTD: `wpss.mdt` + `wpss.b00..b08, b10`. `qcom_mdt_load_no_init()` builds the
segment names by overwriting the last three characters of the firmware name
(`sprintf(seg_name + strlen(fw_name) - 3, "b%02d", segment)`), so the `.mdt`
name is what has to be used — it is now `qcom/sm4450/xiaomi-sky/wpss.mdt`.

The remoteproc auto-boot still happens at ~0.4 s, before `switch_root`, so
`/lib/firmware` does not exist yet. `/init` now starts WPSS after switch_root
(see `rootfs/sky-wifi-up.sh`), exactly like M6 did for the ADSP.

### 2.2 QMI WLFW service instance (fixed — patch 0001)

The firmware registers the WLFW service, but ath11k looked for it with the wrong
instance and therefore never saw it. `qmi_interface.c` encodes the qrtr lookup
field as `version | instance << 8`:

```c
static void qmi_send_new_lookup(...)
	pkt.server.instance = cpu_to_le32(svc->version | svc->instance << 8);
```

* upstream value for wcn6750: `.qmi_service_ins_id = 3` →
  lookup value `0x0301`
* this firmware registers: qrtr instance field `0x0001` = version 1 / instance 0
* `server_match()` requires an exact match when the field is non-zero, so
  `0x0301 != 0x0001` → no match, the driver waits forever, "firmware running but
  no wireless interface" (the symptom also described by upstream commit
  `32ec8488d75d`).

Patch 0001 changes the wcn6750 hw params to `.qmi_service_ins_id = 0`, i.e. the
lookup value becomes `0x0001`.

Measured with `tools/qrtrls` (name-service dump, compiled for aarch64 and run on
the device):

```
$ qrtrls 0x45          # service WLFW, any instance
service  instance node     port
0x45     1        13       0x9        <-- WLFW
$ qrtrls 0x45 0x301    # what upstream asks for
(end)                                  <-- no match
$ qrtrls 0x45 0x1      # what patch 0001 asks for
0x45     1        13       0x9        <-- match
```

Node 13 is the WPSS (`qcom,remote-pid = <13>`), the other services of node 13
(`0xf, 0x2b, 0x42, 0x33`) are the intra-WLAN ones (WMI/ATH11K/etc.).

### 2.3 The remaining blocker: hybrid vs SNOC WCN6750

The run-time vendor DT of this phone (dumped from `/sys/firmware/devicetree`)
describes **two** WCN6750 nodes; the enabled one is the SNOC one:

```dts
qcom,icnss@22800000 {                 /* enabled: vendor icnss2, ADRASTEA path */
	compatible = "qcom,icnss";
	reg = <0x22800000 0x800000>;
	reg-names = "membase";
	qcom,rproc-handle = <&remoteproc-wpss@8a00000>;
	iommus = <&apps_smmu 0x4c0 0x1>;
	interrupts = <0 0x185 4>, <0 0x19d 4>, <0 0x1a0 4>, ... (12 IRQs);
	qcom,wlan-msa-fixed-region = <&wlan_msa_mem_region>;
	vdd-cx-mx / vdd-1.8-xo / vdd-1.3-rfa / vdd-3.3-ch0;
	qcom,smem-states = <&wlan_smp2p_out 0>;
};

qcom,wcn6750 {                        /* status = "disabled" */
	compatible = "qcom,wcn6750";       /* the hybrid/PCIE variant */
	reg = <0x17210040 0x0 0xb0000000 0x10000>;   /* msi_addr, smmu_iova_ipa */
	...
};
```

The vendor driver treats `qcom,icnss` as `ADRASTEA_DEVICE_ID` and `qcom,wcn6750`
as `WCN6750_DEVICE_ID` (`drivers/soc/qcom/icnss2/main.c`). Only the WCN6750
branch sends `wlfw_device_info_send_msg()` and maps the BAR; the ADRASTEA branch
instead sends `wlfw_msa_mem_info` + `wlfw_msa_ready` and accesses the registers
directly through `membase`. Our firmware refuses `DEVICE_INFO` with
"not supported", i.e. it is the ADRASTEA/SNOC firmware — consistent with the
fact that the stock firmware directory is called `adrastea`.

Mainline `ath11k` has no code path for that variant:

* `qcom,wcn6750-wifi` → `ATH11K_HW_WCN6750_HW10` → `.hybrid_bus_type = true`,
  `.qmi_service_ins_id` (patched), `.static_window_map = true`
* hybrid means: `ath11k_ahb_setup_msi_resources()` uses `reg[0]` as the MSI
  address, and `ath11k_qmi_request_device_info()` must return `bar_addr` /
  `bar_size`; `ab->mem` (the register window) is *only* set from that response
  and every register access goes through `ath11k_ahb_window_{read,write}32_*`.

A SNOC port therefore needs:

1. `ab->mem = ioremap(reg[0], reg_len)` — the direct-MMIO path
   (`ath11k_ahb_setup_resources()` non-hybrid branch) and
   `ath11k_ahb_hif_ops_ipq8074` instead of the window ops.
2. `hybrid_bus_type = false` so `ath11k_qmi_request_device_info()` returns
   immediately (`if (!ab->hw_params.hybrid_bus_type) return 0;`).
3. `ath11k_ahb_config_irq()` non-hybrid branch uses
   `platform_get_irq_byname()` with the names in `irq_name[]`
   (`misc-pulse1, misc-latch, sw-exception, watchdog, ce0..ce11`); the stock
   node lists 12 unnamed interrupts, so their mapping to those names still has
   to be determined (the vendor's `icnss2` requests them by index).
4. The ADRASTEA start sequence (`wlfw_msa_mem_info`, `wlfw_msa_ready`) is not
   implemented by ath11k at all — whether it is optional on this firmware is
   unknown.
5. The 4 supplies must be on before the handshake; ath11k never touches
   regulators. Current DTB marks `pm6450 smps7/smps8/smps9` always-on (those are
   the BT/IPCI rails) — for the SNOC chip the vendor's rails are
   `pm6450_l11` (cx-mx), `pm6450_l23` (1.8-xo), `pm6450_l14` (1.3-rfa),
   `pm8010_l7` (3.3-ch0), which is exactly what the repo DTS lists.

### 2.5 ath10k (wcn3990) is the closest working reference for the SNOC half

`drivers/net/wireless/ath/ath10k/{snoc.c,qmi.c}` (wcn3990, e.g. SDM845) is an
integrated-SNOC WLAN of the same shape as our chip, and it already contains
several pieces ath11k lacks:

| ath10k (wcn3990, SNOC) | why it matters for sky |
|---|---|
| `qmi_add_lookup(..., QMI_SERVICE_ID_WLFW, WLFW_SERVICE_VERS_V01, 0)` — **instance 0** | the same encoding that this firmware registers, i.e. exactly what patch 0001 had to force into ath11k |
| `ath10k_qmi_msa_mem_info_send_sync_msg()` → `QMI_WLFW_MSA_INFO_REQ_V01` | the ADRASTEA start sequence wants `wlfw_msa_mem_info`; ath11k has neither the message nor the structs |
| `ath10k_qmi_msa_ready_send_sync_msg()` → `QMI_WLFW_MSA_READY_REQ_V01` | ditto for `wlfw_msa_ready` |
| SCM call with `QCOM_SCM_VMID_MSS_MSA` / `QCOM_SCM_VMID_WLAN` before the MSA messages | gives the WLAN subsystem permission to read/write the MSA carve-out; nothing in ath11k does this |
| `ath10k_snoc_resource_init()`: `platform_get_resource_byname(…, "membase")` → `devm_ioremap()`, then `platform_get_irq(dev, i)` for `i < CE_COUNT` (12) | the SNOC register model: direct base + 12 un-named per-CE IRQs, no MSI, no `DEVICE_INFO` |
| `vreg_list = {"vdd-0.8-cx-mx", "vdd-1.8-xo", "vdd-1.3-rfa", "vdd-3.3-ch0", …}` | the four rails of the stock `qcom,icnss` node, driven explicitly by the driver |
| `ath10k_snoc_wlan_enable()` (per-pipe CE config → `wlfw_wlan_mode`) | the same message ath11k sends, driven from the platform side |

Differences that must stay ath11k's: the 11ax register offsets/HAL, the CE
configuration (9 pipes for wcn6750), WMI/HTT, and — per the vendor stack — the
*windowed* "fake PCI" register access (`ath11k_ahb_window_*_wcn6750`), which
ath10k does not need because wcn3990 is flat mapped. The one open question is
where the 11ax datapath ring interrupts (reo2host_status, wbm2host_rx_release,
…) come from on this variant: the stock node has no such IRQs, so either they
are muxed behind the CE IRQs / a MISC status register, or the port has to
discover the vendor's dispatch (librephone notes that most `qca6750def.c`
registers are "MISSING", i.e. not needed by the host).

### 2.4 The two host interfaces, in the vendor stack and upstream

The WCN6750 has two host interfaces and the firmware decides which one it
implements:

| | SNOC ("adrastea") | hybrid / fake-PCI ("IPCI") |
|---|---|---|
| vendor DT node | `qcom,icnss@22800000` (**enabled on sky**) | `qcom,wcn6750` (**disabled on sky**) |
| registers | `reg = <0x22800000 0x800000>`, `reg-names = "membase"`, windowed access through the same block | `reg = <…msi_addr 0x0>`, BAR reported by `QMI_WLANFW_DEVICE_INFO` |
| interrupts | 12 GIC SPIs, one per copy engine (CE0..CE11) | 32 MSI vectors (`GIC_SPI 768..799`), MSI address in the SoC's `pcie0_msi` window |
| IOMMU | SID `0x4c0` | SID `0x1400` |
| QMI start sequence | `ind_register`, `msa_mem_info`, `msa_ready`, `cap`, BDF | `ind_register`, `host_cap`, `cap`, `device_info`, BDF |
| vendor driver path | icnss2 `ADRASTEA_DEVICE_ID` | icnss2 `WCN6750_DEVICE_ID` |
| upstream ath11k | **not implemented** | `ath11k_ahb` + `hybrid_bus_type` (`ath11k_ahb_window_{read,write}32_wcn6750`) |

References for the vendor side:

* `drivers/soc/qcom/icnss2/{main,qmi,power}.c` in
  [sky-lineage/android_kernel_xiaomi_sky](https://github.com/sky-lineage/android_kernel_xiaomi_sky)
  (branches `lineage-23.2`): `icnss_dt_match` maps `qcom,icnss` → ADRASTEA and
  `qcom,wcn6750` → WCN6750, and the ADRASTEA branch is the one without
  `wlfw_device_info_send_msg()`.
* qcacld's host-interface layer for this chip:
  `qca-wifi-host-cmn/hif/{inc/hif.h,src/snoc,src/ce,src/qca6750def.c}`
  (`HIF_TYPE_ADRASTEA` in the 2021 tree, `HIF_TYPE_QCA6750 = 23` /
  "fake PCI" DID `0x1105` in the 2024 tree). The
  [librephone HIF notes](https://librephone.fsf.org/site/hif/) summarise it:
  MMIO base and 12 per-copy-engine IRQs come from the WPSS side (not from a QMI
  `device_info`), registers are accessed through a windowed "fake PCI" mapping,
  and most of the QCA6750 register definitions are not needed by the host.

Upstream reference for the *other* interface:
`arch/arm64/boot/dts/qcom/milos.dtsi` (SM7635 / Fairphone 6, in mainline since
2026) describes exactly the hybrid node —

```dts
wifi: wifi@17110040 {
	compatible = "qcom,wcn6755-wifi", "qcom,wcn6750-wifi";
	reg = <0x0 0x17110040 0x0 0x0>;            /* msi_addr */
	iommus = <&apps_smmu 0x1400 0x1>;           /* v1 SID, hybrid */
	interrupts = <0 768 … 799>;                 /* 32 MSI vectors */
	qcom,rproc = <&remoteproc_wpss>;
	qcom,smem-states = <&smp2p_wlan_out 0>;
	qcom,smem-state-names = "wlan-smp2p-out";
};
```

which is the same shape as the *disabled* `qcom,wcn6750` node in sky's vendor DT
(`0x17210040`, `GIC_SPI 0x300..0x31f`, SID `0x1400`). So the chip is upstream
supported — with the IPCI firmware. sky's stock `NON-HLOS.bin` ships only the
SNOC ("adrastea") build, and `image/adrastea` is the only WLAN firmware
directory in it.

`dts/sm4450-xiaomi-sky.dts` has been updated to the SNOC-correct node
(`membase`, 12 IRQs, SID `0x4c0`, the four `l11/l23/l14/l7` rails) plus
`qcom,rproc` and the wlan smp2p state, i.e. the DT is ready for the port; the
DTB currently flashed on the phone is the *hybrid* one used to get the QMI
diagnosis.

### 2.6 Phase 1 implementation (kernel `#63`, boot image `…-k63-snoc.img`)

`kernel/patches-m8/0003` adds a DT-gated SNOC mode to `ath11k_ahb`:

```
qcom,snoc-ahb;                 /* in the wifi node */
```
* `ath11k_ahb_probe()` sets `ab->hw_params.hybrid_bus_type = false` right after
  `ath11k_core_pre_init()`, which makes `ath11k_ahb_setup_resources()` ioremap
  the DT `membase` (0x22800000) and turns `ath11k_qmi_request_device_info()`
  into a no-op; the wcn6750 windowed register ops stay in use
  (`ath11k_ahb_window_{read,write}32_wcn6750`, window register at BAR+0x310c).
* `ath11k_ahb_config_irq()` requests the copy-engine interrupts **by index**
  (`platform_get_irq()`, like `ath10k/snoc.c`) instead of by name, and skips
  the extended interrupts (this variant's DT has no such IRQs — open issue,
  see §2.5).
* `ath11k_qmi_event_server_arrive()` sends `msa_mem_info` + `msa_ready` right
  after `ind_register` when the property is present: the MSA range comes from
  `memory-region` (the `wlan_msa_mem` carve-out, 0x82a00000+3 MiB), and
  `qcom,msa-fixed-perm` (already in the node) means the TZ keeps the
  permissions, so no `qcom_scm_assign_mem()` call is needed.

First run of phase 1 (kernel `#63`, flashed): everything up to the MSA
handshake works —

```
ath11k 22800000.wifi: snoc mode: membase registers, per-CE interrupts
ath11k 22800000.wifi: snoc mode: extended interrupts not configured
ath11k 22800000.wifi: msa 0x82a00000+0x300000, 2 region(s)
ath11k 22800000.wifi: host capability request failed: 1 90
ath11k 22800000.wifi: failed to send qmi host cap: -22
```

`msa_mem_info` is accepted by the firmware (it returns two memory regions) and
the per-CE interrupts are registered. The next message, `host_cap`, is
rejected with QMI error **90 = `QMI_ERR_INCOMPATIBLE_STATE_V01`** — which is
exactly why the vendor's ADRASTEA sequence has no `host_cap` at all. Kernel
`#64` therefore skips it in SNOC mode (the boot image
`wifi-backup/out/sky-shim-boot-v4-wifi-k64-snoc.img` is built, verified and
**not flashed yet**).

The matching DTB is `wifi-backup/wifi-snoc-fdt.dtb` (built from the live DT by
`wifi-backup/patch-fdt-snoc.py`): the stock SNOC node (membase, 12 per-CE IRQs,
SID 0x4c0, four rails) plus `qcom,rproc`, the wlan smp2p state,
`qcom,snoc-ahb` and `status = "okay"`, with the four rails marked
`regulator-always-on` (ath11k never touches regulators).

### 2.7 Phase 1 experiment log (kernels #63 … #71)

All of these are on-hardware results; the WLFW start sequence is the only thing
that changed between builds.

| Build | Change | Result |
|---|---|---|
| `#63` | membase + per-CE IRQs + `msa_mem_info`/`msa_ready` | `msa 0x82a00000+0x300000, 2 region(s)` ✅, then `host capability request failed: 1 90` (`INCOMPATIBLE_STATE`) |
| `#64` | skip `host_cap` | target cap OK (`chip_id 0x4130 …`), `board file download request failed: 1 19` |
| `#66` | `bdf_chunk` / `bdf_file_id` / `bdf_total_full` / `regdb_download` / `msa_handshake` knobs | chunk 2048 and `file_id` 0 still error 19 → not a message-length or file-id problem |
| `#67` | `host_cap` before MSA (ath10k order) + fix NULL deref in `ath11k_ahb_free_irq()` (SNOC never sets the ext IRQ groups, `unbind` crashed) | BDF still error 19 |
| `#68` | register the `MSA_READY_IND` (0x2B) handler and request it in `ind_register` | the indication really arrives (`waiting for the MSA ready indication` → BDF 16 ms later), BDF still error 19 |
| `#69` | `snoc_bdf_download=n` | firmware stays silent afterwards → the BDF download really is required |
| `#70` | `ind_register` set copied from the stock icnss2 ADRASTEA branch (`fw_ready` + `pin_connect_result` + `msa_ready`, no `cal_done` / `fw_init_done`) | BDF still error 19 |
| `#71` | `snoc_fixed_mem=n` → firmware asks for DDR memory over QMI | **new behaviour**: `qmi respond memory request failed: 1 48` (`QMI_ERR_INVALID_ARG`), and the BDF attempt then returns **90** instead of 19 → the state machine *did* move |

The firmware itself documents the gate —

```
qmi_platform.c:qmi_plat_check_state: error - received BDF DOWNLOAD REQ at wrong state %d
qmi_platform.c:QMI Platform: BDF download REQ received, seg_id 0x%x
```

— i.e. error 19 is not a bad TLV, the firmware refuses the *message order*.
The two remaining candidate sequences are:

1. **Firmware owns the MSA, host never asks for memory** (stock icnss2 ADRASTEA
   branch). Everything up to and including `cap` matches the vendor code and the
   `MSA_READY_IND` handshake is fine, yet `BDF DOWNLOAD` is still refused. Next
   thing to try: log the requested/returned chunks and diff the remaining
   `ind_register` bits (`daemon_support`, `respond_get_info`, `qdss`), and the
   `wlan_en_delay`/temperature step the vendor does for WCN6750 only.
2. **Host provides DDR memory** (`fixed_fw_mem = false`). The firmware
   immediately sends `REQUEST_MEM_IND`; our `RESPOND_MEM_REQ` is rejected with
   `INVALID_ARG`, most likely because the request does not fit the 3 MiB
   `wlan_msa_mem` carve-out (the chunk sizes are only logged with
   `ATH11K_DBG_QMI`, which this kernel has compiled out). If this path is taken,
   the host also has to map the region into the WLAN IOMMU
   (`ath11k_ahb_fw_resources_init()` + a `wifi-firmware` child node as in
   `arch/arm64/boot/dts/qcom/kodiak.dtsi`), so it is the larger of the two.

### 2.9 What the firmware actually asks for (kernel `#72`)

Kernel `#72` logs the WLFW memory handshake at `info` level. With
`snoc_fixed_mem=n` (host-provided DDR) the answer is short and clear:

```
ath11k: fw memory request: 1 segment(s)
ath11k:   seg 0: type 0 size 0x100000 (1048576)      /* HOST_DDR_REGION_TYPE */
ath11k:   dma seg 0: type 0 size 0x100000 -> paddr 0x00000000ffe00000
ath11k: respond mem: 1 segment(s)
ath11k:   rsp seg 0: type 0 size 0x100000 addr 0x00000000ffe00000
ath11k: qmi respond memory request failed: 1 48      /* QMI_ERR_INVALID_ARG */
```

So the firmware wants **one 1 MiB host-DDR chunk** and rejects our answer. Two
things are wrong with it, both visible in the vendor DT/driver:

* the address is an IOVA handed out by the DMA API near the top of the 32-bit
  space (`0xffe00000`), while the stock node pins the WLAN IOVA pool to
  `qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>`; mainline has **no
  support for that property**, so the IOVA lands outside the window the
  firmware's SMMU context (set up by the TZ) can reach;
* `host_cap` is sent **without `ddr_range`**. The stock `icnss2` fills it with
  `ddr_range[0] = {iova_start, iova_size + iova_ipa_size}` and
  `ddr_range[1] = {msa_pa, msa_size}` whenever SMMU stage 1 is on, i.e. the
  firmware never learns which DDR ranges it may use - a range check failing
  with `INVALID_ARG` matches that.

Next steps on this path (in order): send `ddr_range` in `host_cap` like the
vendor, then make the chunk address land inside `0xa0000000 + 0x10000000`
(either by allocating the chunk from a dedicated reserved region declared in
`ddr_range`, or by teaching the port to program the IOMMU domain's IOVA base).

Both paths are switchable at run time (kernel `#71`), so the next session can
iterate without reflashing:

```
/sys/module/ath11k/parameters/{msa_handshake,host_cap? snoc_host_cap,
    snoc_bdf_wait_ind,snoc_bdf_download,snoc_bdf_delay_ms,snoc_bdf_type,
    snoc_ind_vendor,regdb_download,bdf_chunk,bdf_file_id,bdf_total_full}
/sys/module/ath11k_ahb/parameters/snoc_fixed_mem
```

with `/etc/sky-wifi-params` (`key=value` per line) applied by
`rootfs/sky-wifi-up.sh` at boot, and `sysrq-b` to reboot.

### 2.8 Evaluated alternative: out-of-tree qcacld-3.0 + icnss2 (rejected)

The obvious "vendor stack" fallback — LineageOS/CAF `qcacld-3.0` +
`qca-wifi-host-cmn` + `fw-api` + `icnss2`, which is what Android uses on this
exact firmware — was measured against this kernel rather than guessed at.

Probe: sparse clone of a recent vendor WLAN tree (LineageOS 23.0 branch of
`ArianK16a/android_kernel_fairphone_sm7635-modules`, i.e. the same
`qcom/opensource/wlan` layout), then

```bash
make -C <linux-7.3> M=$PWD/qcacld-3.0 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     MODNAME=wlan WLAN_ROOT=$PWD/qcacld-3.0 \
     CONFIG_QCA_CLD_WLAN_PROFILE=adrastea CONFIG_QCA_CLD_WLAN=m modules
```

Results, in order:

1. the vendor `Kbuild` needs `WLAN_ROOT` + a device profile
   (`configs/adrastea_defconfig` exists for this chip) and greps the kernel for
   the downstream `kernel/sched/walt/walt.c`;
2. with the profile set it compiles — and stops on a missing sibling repo
   (`qcom/opensource/dataipa/config/dataipa_vendor.h`, the IPA offload glue);
3. with `dataipa` added it stops on `linux/qcom-iommu-util.h`, a
   **downstream-only kernel header** (Qualcomm's `drivers/iommu` helpers) that
   does not exist in mainline.

Static size measurements of the stack: **1.25 M lines of C** in
`qcacld-3.0` + `qca-wifi-host-cmn` (5782 files), 167 distinct kernel headers
included, of which **~20 do not exist in mainline 7.3**:

```
linux/ipa.h                linux/ipa_wdi3.h        linux/qcom-iommu-util.h
linux/sched/walt.h         linux/sched/core_ctl.h  linux/minidump_tlv.h
linux/wakelock.h           linux/qcn_sdio_al.h     linux/inet_lro.h
linux/of_gpio.h            net/cnss2.h             net/cnss.h
net/cnss_utils.h           net/cnss_logger.h       net/cnss_nl.h
net/cnss_prealloc.h        soc/qcom/icnss2.h       soc/qcom/icnss.h
soc/qcom/minidump.h        soc/qcom/boot_stats.h   linux/autoconf.h
```

plus ~40 known API-drift sites that will each be a compile error on 7.3
(`hrtimer_init()` ×17 → `hrtimer_setup()`, `class_create()` ×7 → one argument,
`setup_timer()` ×3, WEXT `iw_handler`/`wireless_send_event` ×10, …).

Verdict: **not the cheaper path.** It is a multi-week port of a 1.25 M line
vendor blob onto a kernel it was never written for, permanently out-of-tree,
and it contradicts the point of this project (mainline kernel). Its only
advantage — "this is what Android runs" — is already captured: the stock
`icnss2` sources gave us the exact ADRASTEA start sequence (host_cap,
msa_mem_info/msa_ready, cap, BDF) and the register/IRQ model we implemented in
`ath11k`. Keep using it as *documentation*, not as build input.

### 2.10 Kernel `#73`: `ddr_range` in `host_cap`, and the state gate stays

`#73` adds the missing `host_cap` TLV 0x1F (`ddr_range`, 3-entry static array,
same layout as the stock `wlfw_host_cap_req_msg_v01`) and a `snoc_ddr_range`
knob (0 = none, 1 = whole 32-bit window + MSA, 2 = stock `0xa0000000+0x10000000`
+ MSA). `host_cap` now reports `0+0x100000000, 0x82a00000+0x300000` and is
accepted by the firmware.

Measured afterwards (all via `/etc/sky-wifi-params` + reboot, no reflash):

| Variant | Result |
|---|---|
| default (host_cap + MSA + wait for `MSA_READY_IND` + BDF) | BDF `1 19` |
| `snoc_host_cap=0 snoc_bdf_wait_ind=n` — **exactly the stock icnss2 ADRASTEA order** (no host_cap, BDF immediately after cap) | BDF `1 19` |
| `regdb_download=n` (BDF only) | BDF `1 19` |
| `snoc_bdf_type=1` (ELF instead of BIN) | BDF `1 19` |
| `snoc_fixed_mem=n` (host DDR, now with `ddr_range` present) | `RESPOND_MEM` still `1 48` (`INVALID_ARG`) at IOVA `0xffe00000`, then BDF `1 90` |

So the message order, the BDF type, the regdb and the DDR range announcement
are all ruled out. What the stock stack does that we still do not do is the
*platform* side of icnss2's probe: `icnss_smmu_init()` (attach the WLAN SMMU
domain and program the `qcom,iommu-dma-addr-pool` window, `qcom,iommu-dma =
"fastmap"`) and `icnss_hw_power_on()` (rails + the `cxo_ref_clk_pin` clock).
In mainline the SMMU side is whatever `arm-smmu-qcom` set up for the device and
we run the `use_tz` MSA path, i.e. the firmware's MSA is expected to be mapped
by the TZ alone.

Next experiment (needs a DTB change, so one flash): make ath11k map the
firmware regions itself instead of trusting the TZ - add the `wifi-firmware`
child node plus a second `memory-region` (CE) to the wifi node, exactly like
`arch/arm64/boot/dts/qcom/kodiak.dtsi`, so that
`ath11k_ahb_fw_resources_init()` allocates its own IOMMU domain and
`iommu_map()`s MSA + CE with the device's SID (`0x4c0`). If the BDF download
still refuses, the remaining suspect is the clock/power vote that only
`icnss_hw_power_on()` does.

### 2.11 Breakthrough: the BDF download works, the firmware boots, and the SNOC register window is now the blocker

Three one-line causes were all that stood between "firmware rejects everything"
and "driver runs the real init sequence".

**1. `total_size` (kernel `#75`/`#76`).** The firmware accepts a segment only if
`total_size` is the size of the *whole file*, not the amount still to come -
i.e. exactly what the stock icnss2 does. ath11k sends `remaining`. The
per-segment logging added in `#75` showed it plainly:

```
bdf seg 0 ok: len 6144 end 0 type 0 file_id 255 total 26328
bdf seg 1 ok: ... seg 2 ok ... seg 3 ok
board file download failed: seg 4 len 1752 end 1 type 0 file_id 255: 1 19
```

With `bdf_total_full=y` both the regdb (4 segments) and the BDF (5 segments,
including the final `end=1` one) transfer completely.

**2. cal data (`#76`).** ath11k then insists on a separate `cal-ahb-*.bin`,
which this platform does not have (`cal_download` knob, off by default now).

**3. `QMI_WLFW_CAL_REPORT_REQ_V01` (`0x26`, kernel `#78`).** The stock ADRASTEA
branch sends a cal report right after the BDF; ath11k never does. With it
(`snoc_cal_report`, on by default) the firmware finally continues and
`ath11k_core_qmi_firmware_ready()` runs - the driver is in the real bring-up
path for the first time.

**4. CE/HAL init (`#79`).** `ath11k_ce_init_ring()` crashed on a NULL
`ab->pci.msi.config` because the AHB hif ops point at the PCI MSI helper; `#79`
adds `ath11k_ahb_get_user_msi_vector()`, which returns `-ENOPROTOOPT` in SNOC
mode (one GIC line per CE, no MSI block).

**Current blocker:** HAL SRNG setup for the CE rings then does a register write
to offset `0x100000` in the membase window and takes a **synchronous external
abort**:

```
pc : ath11k_ahb_window_write32_wcn6750+0x48/0x50
lr : ath11k_pcic_write32+0x44/0xfc
ath11k_hal_srng_setup+0x220/0x7ec -> ath11k_ce_init_ring+0x98/0x270
Internal error: synchronous external abort: 0000000096000010 [#1]
```

The WCN6750 window scheme (`ath11k_ahb_get_window_start_wcn6750()`: UMAC range
`0x00a00000` -> window at `ATH11K_PCI_WINDOW_START` `0x80000`, CE range ->
`2 * 0x80000`, everything else direct) is the *PCI* layout. For the ADRASTEA
SNOC variant the UMAC/DP/CE register blocks live at different offsets inside
the 8 MiB membase (`0x22800000+0x800000`), so a direct access at `0x100000`
hits nothing and the bus aborts.

Next step: take the register map for the SNOC variant from the stock sources
(the vendor `ath11k`/`icnss2` in the sky 5.10 tree, or `ravelin-adrastea.dtsi`
if it lists extra `reg` windows) and teach `ath11k_ahb_window_*_wcn6750()` and
`ath11k_ahb_read32()/write32()` the SNOC layout.

### 2.12 Kernel `#80`: CE init passes, the DP/UMAC register window is the last mapping question

`#80` does three things for SNOC mode: the window ops become plain MMIO, the CE
ring bases are patched to the adrastea values from the vendor's own register
header (`ADRASTEA_A_WCSS_HM_A_WIFI_APB_1_A_WFSS_CE0_SR_BA_LOW = 0x00240000`,
`..._CE0_DR_BA_LOW = 0x0024000c`, one 0x1000 block per CE), and `mem_ce` is set
so `ATH11K_CE_OFFSET()` is zero. Note that plain MMIO *alone* (the obvious
"just drop the window arithmetic" patch) cannot work: mainline's CE bases
(`0x01b80000`) are PCI-window space and land outside the 8 MiB membase.

Result: **CE initialisation passes** - the crash moved one level up to

```
pc : ath11k_ahb_window_write32_snoc+0xc/0x14
ath11k_hal_srng_setup+0x220/0x7ec
ath11k_dp_srng_setup+0x200/0x490
ath11k_dp_alloc+0x7c/0x5f0
ath11k_core_qmi_firmware_ready+0xd8/0x5a4
```

i.e. the data-path rings. Their register offsets are the `HAL_SEQ_WCSS_UMAC_*`
constants (`WBM 0x00a34000`, `REO 0x00a38000`, `TCL 0x00a44000`) - the same
values the vendor `fw-api/hw/qca6750/v1/wcss_seq_hwiobase.h` uses - but they
are *above* the 8 MiB membase, so a direct access aborts.

Facts collected while chasing this (all from the vendor sources):

* the stock `icnss2` maps only `resource_size(reg)` (8 MiB) for SNOC and its
  `hif_write32_mb()` is a plain `iowrite32()` (`hif_io32.h`: the register-window
  helper is `#if defined(HIF_REG_WINDOW_SUPPORT) && defined(HIF_PCI)`), so the
  offsets it uses for SNOC *cannot* be the PCI ones;
* `adrastea_reg_def.h` only ever describes offsets up to `0x002F1008`
  (CE blocks at `0x240000+`, WCSS_HM/`SR_APSS` registers at `0x2f1000`/`0x30000`),
  i.e. the host-visible view is the first ~3 MiB of the membase;
* mainline already has the "shadow register" machinery for REO/TCL rings
  (`ath11k_hw_params.supports_shadow_regs`, `HAL_SHADOW_REG()`).

Next experiment (`#81`, cheap and decisive): ioremap the membase with a larger
window in SNOC mode (e.g. 16 MiB) and keep the standard UMAC offsets - if the
DP rings then program, the map is simply bigger than the DT describes;
otherwise the SNOC ring registers live at adrastea-specific offsets that still
have to be derived.

### 2.13 Kernel `#81`: a bigger membase window does not reach the UMAC block

`#81` adds `snoc_membase_mib` (map more than the DT's 8 MiB). Two measurements
settle what the data-path rings need:

| mapping | write to `membase + 0xa34874` (WBM ring) |
|---|---|
| 8 MiB (DT) | kernel page fault (`FSC = 0x06`, level 2 translation fault) |
| 16 MiB | **synchronous external abort** (`ESR 0x96000010`) |

So the pages exist at 16 MiB, but the hardware does not decode
`0x22800000 + 0xa34874`. Together with the earlier finding that
`adrastea_reg_def.h` never describes anything above `0x002F1008`, the UMAC /
WBM / REO / TCL ring registers are **not** directly reachable from the host on
this variant - the stock driver must reach them some other way.

The strongest candidate in the vendor register header is the **SR_APSS shadow
register interface** at the bottom of the membase:

```
ADRASTEA_A_WCSS_SR_APSS_SHADOW_ADDRESS0..17   (0x00030000 + n*8)
ADRASTEA_A_WCSS_SR_APSS_SHADOW_VALUE0..17
ADRASTEA_A_WCSS_SR_APSS_SHADOW_ADDRESS2__ADDRESS_REGISTER___M 0x003FFFFF
ADRASTEA_A_WCSS_SR_APSS_DIRTY / _ADDRESS_VALID / _INVALID_ADDR_ACCESS
ADRASTEA_A_WCSS_SR_APSS_SR_CONTROL / _SR_INTERRUPT_CLEAR
```

i.e. "write address, write value, wait for the handshake" instead of a plain
MMIO store, with a 22-bit address window (4 MiB) that the UMAC offsets
(`0xa00000 & 0x3fffff = 0x200000`) fit into.

Also worth noting from this run: the firmware itself asserts while this happens
(`cmnos_thread.c:4003: Asserted in copy_engine.c:_CE_misc_handler:2227`) and the
WPSS is then recovered by remoteproc, so the CE/MISC registers we already touch
are being watched.

Next step: implement the shadow-register write/read path for the UMAC range in
`ath11k_ahb_window_{read,write}32_snoc()` and see whether the DP rings program.
Reference: `qca-wifi-host-cmn/hif/src/adrastea_reg_def.h` (register set) and
`copy_engine.c`/`qmi_platform.c` behaviour above.

### 2.14 Kernel `#82`: the stock window protocol does not exist on adrastea either

`#82` implements the stock register-remap protocol literally (from
`qca-wifi-host-cmn/hif/src/hif_io32.h`: `WINDOW_REG_ADDRESS 0x310C`,
`WINDOW_ENABLE_BIT 0x40000000`, `WINDOW_SHIFT 19`, `WINDOW_VALUE_MASK 0x3F`,
`WINDOW_START 0x80000`, `WINDOW_RANGE_MASK 0x7FFFF`, i.e. write
`ENABLE | ((offset >> 19) & 0x3F)` to `membase + 0x310C`, then access
`membase + 0x80000 + (offset & 0x7FFFF)`), with the verified-good CE block
(`0x240000`..`0x260000`) kept direct, and `snoc_reg_mode` to switch between
plain MMIO / selector-for-everything / CE-direct+selector.

Result: the abort is now *inside the window select write itself*

```
pc : ath11k_ahb_window_write32_snoc+0x4c/0xa0
x1 : 0000000000a34874   (offset)
x3 : 0000000040000014   (ENABLE | window 0x14)
x4 : ffff80008980310c   (ab->mem + 0x310C)
Code: ... d50332bf (b9000083)      <- str w3, [x4]
```

The stock header explains why - for adrastea those registers are marked
missing:

```
#define ADRASTEA_WINDOW_DATA_ADDRESS        MISSING
#define ADRASTEA_WINDOW_READ_ADDR_ADDRESS   MISSING
#define ADRASTEA_WINDOW_WRITE_ADDR_ADDRESS  MISSING
```

so the adrastea SNOC has no legacy window register at all, and there is no
512 KiB..1 MiB remap window to select. What *does* exist in the host-visible
part of the membase is the **SR_APSS shadow register block**:

```
SR_APSS_SHADOW_ADDRESS0..23  0x00030020 + 4*n
SR_APSS_SHADOW_VALUE0..23    0x00032000 + 4*n
SR_APSS_ADDRESS_VALID        0x000300E0
SR_APSS_INVALID_ADDR_ACCESS / _DIRTY / _SR_CONTROL / _SR_INTERRUPT_CLEAR
SR_APSS_FORCE_WAKE           0x00032060     SR_APSS_SW_SCRATCH 0x00032064
SR_APSS_WCSSAON_SR_LSB       0x00032070
```

(address+value pairs plus a validity bitmap, i.e. a host->WCSS write port.)

Two open questions for the next session, in order of value:

1. **Does the host program the data-path rings at all on this variant?** On an
   integrated-MAC SNOC design the WPSS firmware may own the SRNG
   configuration and the host only needs the CE path (which works) plus the
   ring addresses. If so, `ath11k_dp_alloc()`'s direct ring programming is the
   wrong model here, and the port needs a different DP initialisation path
   rather than a cleverer register window.
2. If the host does own them, the SR_APSS shadow pairs are the documented
   write port and their handshake has to be derived from the firmware that is
   already running on the WPSS (`copy_engine.c` asserts while we poke around
   the CE block, so it is watching).

### 2.15 Confirmation: yes the host programs the rings, and the stock static window table does not fit either

Answered from the stock sources (kernel `#83` implements the result):

* the host **does** program the data-path rings (`hal_srng_setup()` in
  `qca-wifi-host-cmn/hal/wifi3.0/hal_srng.c`, called from the DP init path), so
  this is not a "the firmware owns the rings" design;
* `hal_target_based_configure()` sets, for `TARGET_TYPE_QCA6750`:
  `use_register_windowing = true; static_window_map = true;`
* the static map is `hal_get_window_address_6750()` (hal/wifi3.0/qca6750/hal_6750.c):

```
#define UMAC_WINDOW_REMAP_RANGE 0x14
#define CE_WINDOW_REMAP_RANGE   0x37      /* 0x37 * 0x80000 = 0x1b80000 */
#define CMEM_WINDOW_REMAP_RANGE 0x02
window = (offset >> 19) & 0x3f;
  UMAC -> scale 1, CE -> scale 2, CMEM -> scale 3
addr = scale * 0x80000 + (offset & 0x7ffff);
```

`#83` implements exactly that (modes 3/4, CE block optionally direct). Result:
the DP ring write for the UMAC offset `0xa34874` becomes `membase + 0xb4874`
(UMAC window 1), and **that aborts as well** - as did `0x80000`, `0x100000` and
`0x180000` in the earlier builds.

So on this device the membase aperture responds at `0x240000` (CE) and
`0x30000` (SR_APSS) but **not** at the 0x80000/0x100000/0x180000 window area.
That is consistent with the stock window scheme being a *PCI BAR* artefact
("Device memory is 32MB but bar size is only 1MB" - hif_io32.h): with the BAR
there is a 512 KiB direct area plus remap windows, while the SNOC membase is
already a wide, direct view. The UMAC block therefore has to be somewhere the
DT does not describe.

Next experiment (`#84`): a one-shot probe that ioremaps a handful of candidate
bases/windows and reads one known register from each (the kernel survives the
external aborts, as the last runs show) so the real location of the UMAC /
WBM / REO / TCL block is found in a single boot instead of one guess per flash.

### 2.16 `#84` probe and where the SNOC path stands

`#84` added a fault-tolerant register probe, but it ran inside the probe path
and the bind is issued by `/init` (PID 1), so the first dead address killed
init: `Internal error: synchronous external abort` followed by
`Kernel panic - not syncing: Attempted to kill init`. Device fell back to
`#83`; the probe is now behind `ath11k_snoc_probe` (default off) and must be
moved to a workqueue before it is used again.

Status of the SNOC port after ~24 hardware builds:

| piece | state |
|---|---|
| WPSS start, split-MDT fw, smp2p | works |
| WLFW QMI: ind_register, host_cap + ddr_range, msa_info/msa_ready, target cap, regdb (19 KB) + BDF (26 KB) download, cal report | works |
| firmware reaches "driver init" | works |
| CE0..CE11 rings (registers at membase+0x240000) | works |
| DP rings (UMAC/WBM/REO/TCL) | **register block not reachable** - not at membase+0xa3xxxx, not through the stock PCI-style window map (0x80000/0x100000/0x180000 all abort), not at 16 MiB |
| extended interrupts / NAPI / WMI-HTC start | not attempted yet |

So the remaining gap is not a single bug: the WCSS data-path register block has
to be found (SR_APSS shadow port at 0x30020/0x32000 is the only documented
write path left) and then the DP, ext-IRQ and WMI start-up have to be brought
up on top of it. That is weeks of work with a live risk of dead ends, because
the access mechanism is undocumented outside Qualcomm.

### 2.17 Decision: SNOC vs out-of-tree qcacld

Two honest options for "WiFi working on this phone":

1. **Keep porting ath11k SNOC** (mainline stays clean). Remaining: locate the
   DP register block or implement the SR_APSS shadow port, then DP + ext-IRQ +
   WMI start-up, then a working scan/assoc path. Estimate: weeks, unbounded
   risk on the register access mechanism.
2. **Run the vendor stack** where it was written: `icnss2` + `qcacld-3.0` on the
   stock 5.10/6.1 kernel. That stack is what ships on this device and it
   already contains the SNOC DP/ring/shadow handling we are reverse
   engineering. It means giving up the mainline kernel for WiFi.

Porting `qcacld-3.0` (1.25 M lines, ~20 downstream-only headers such as
`linux/ipa.h`, `linux/qcom-iommu-util.h`, `linux/sched/walt.h`, `net/cnss*.h`,
`soc/qcom/icnss2.h`, plus `dataipa` and the `wlan/platform` repos) onto 7.3 is
the worst of both worlds: the same scale of work as (1) *and* permanently
out-of-tree. If the goal ever changes from "mainline WiFi" to "WiFi", option 2
on the vendor kernel is the shortest path; it is not a shortcut for mainline.

## 3. What is installed on the device

| Path | Content | Backup |
|---|---|---|
| `/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.mdt` + `wpss.b00..b08, b10` | WPSS firmware (from `NON-HLOS.bin`, `image/adrastea/`) | new files |
| `/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.mbn` | copy of `wpss.mdt` — only needed for the old `.mbn` DT name | new file |
| `/lib/firmware/ath11k/WCN6750/hw1.0/board.bin` | `image/adrastea/bdwlan.bin` (BDF) | new dir |
| `/lib/firmware/ath11k/WCN6750/hw1.0/regdb.bin` | `image/adrastea/regdb.bin` | new dir |
| `/usr/local/sbin/sky-wifi-up.sh` | starts WPSS after switch_root, then binds ath11k | new file |
| `/init` | one added line launching the helper | `/init.pre-m8` |

`board.bin` is the generic BDF; per-board variants (`bdwlan.<id>`, board id of
the device is `0x1000b`) have not been tried yet, and a `board-2.bin` container
(with the per-board BDFs) is the next thing to try if the BDF is rejected.

## 4. Boot image / DTB

The flashed `boot_a` is the old shim image with the payload DTB replaced; the
kernel Image is rebuilt from the M7 tree plus the two ath11k patches.

```
wifi-backup/
  boot/boot_a-original.img            byte-for-byte stock-with-M7 boot_a (md5 matched device)
  boot/Image-before-m8                the M7 Image (build #60)
  out/sky-shim-boot-v4-wifi-k61.img   DTB only, Image #61 (WLFW instance 1 — wrong value)
  out/sky-shim-boot-v4-wifi-k62.img   DTB + Image #62 (WLFW instance 0) — flashed
  wifi-fdt.dtb / wifi-fdt.dts         the DTB that is in boot_a now (hybrid wifi node)
  patch-fdt.py                        produces wifi-fdt.dtb from the live runtime DT
  repack-boot.py                      swaps Image + DTB inside the shim payload
  logs/build-m8-wifi*.log             kernel build logs
```

Repack/verify rules that keep the image provably minimal:

* shim prefix (77824 B), ramdisk (512 B), `load_pa = 0xc0000000`,
  `dtb_pa = 0xc7800000` and every other byte are copied from the backup;
  a `cmp`-style diff against `boot_a-original.img` shows only the DTB region,
  the shim `image_size` field and the boot header `kernel_size`.
* the `arena` of the payload is reconstructed with `tools/mklinuxpayload.py`
  and `tools/mkbootimg_v3.py`, so the layout stays what the shim expects.

Rollback:

```bash
fastboot flash boot_a wifi-backup/boot/boot_a-original.img     # back to M7
```

## 5. Reproduce / debug

```bash
# firmware (host)
7z x sky_in_global_images_V14.0.7.0.TMWINXM_13.0/images/NON-HLOS.bin "image/adrastea/*"
# kernel
patches: kernel/patches-m8/0001, 0002
cd sky-mainline/kernel/linux-7.3 && make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j12 Image
# boot image
python3 wifi-backup/repack-boot.py <path>/arch/arm64/boot/Image
# device side
echo start > /sys/class/remoteproc/remoteproc1/state
cat /log/wifi-up.log; dmesg | grep -i ath11k
aarch64-linux-gnu-gcc -static -O1 -o qrtrls tools/qrtrls.c && scp qrtrls root@192.168.7.1:/tmp/
/tmp/qrtrls 0x45          # is WLFW registered, with which instance
```

Gotchas confirmed again on this board:

* `echo 17210040.wifi > .../drivers/ath11k/unbind` on a **stalled** device
  panics the kernel (NULL deref in `ath11k_ce_cleanup_pipes()`); patch 0002
  (upstream `1a7bcf5324c8`) fixes that, so after flashing #62 unbind/rebind no
  longer crashes — but a reboot is still the cleaner way to retry a probe.
* the kernel log buffer is small: `dmesg` loses the boot log after ~1000 extra
  lines (a sysrq-t dump is enough). Use `/log/` for anything that must survive.
* `/sys/kernel/debug/gpio` still hangs the box — do not read it.

## 6. Where it stands now, and what is still hard

The SNOC/adrastea port described in §2.3 *was* written and taken much further in a
dedicated thread; read **`docs/wifi-handoff/HANDOFF-snoc-mainline.md`** for the working
handoff. Short version — solved (measured on hardware, kernel #83):

* WPSS remoteproc boots from the split-MDT firmware (`wpss.mdt` + `b00..b08,b10`);
* WLFW QMI over QRTR with **instance 0** (patch 0001), `chip_id 0x4130`,
  `fw_version 0x343b815a …CI_WLAN.HL.3.4.3-00346.1`;
* `host_cap` + `ddr_range`, MSA handshake (`msa_mem_info` / `msa_ready` /
  `MSA_READY_IND`), regdb **and** BDF download (the `total_size`-per-segment semantic
  fix), the mandatory `CAL_REPORT` (0x26), and CE0..CE11 ring initialisation;
* `ath11k_core_qmi_firmware_ready()` runs — i.e. the whole firmware-side contract of
  `ath11k` is satisfied.

Still blocking, in the order they must be solved:

1. **The WCSS UMAC/WBM/REO/TCL register block has no known address on this SNOC
   variant.** `ath11k_dp_srng_setup()` → `ath11k_hal_srng_setup()` writes DP ring
   registers at the `HAL_SEQ_WCSS_UMAC_*` offsets and dies with a synchronous external
   abort. Measured: `membase+0` dead, `+0x30000` (SR_APSS) live, `+0x240000` (CE) live,
   `+0x80000/0x100000/0x180000` dead, `+0xa34874` dead, and the vendor's static-window
   map (window selector `ADRASTEA_WINDOW_*`) does not exist on adrastea. The remaining
   candidates are a second physical base not described in the DT, or the SR_APSS shadow
   write port (`SHADOW_ADDRESS0..23` / `SHADOW_VALUE0..23`).
2. **Extended interrupt dispatch for SNOC** — `ath11k_ahb_config_irq()` returns early;
   the SNOC DT has no named ext-IRQ resources, so this needs MISC-status dispatch or
   per-group handling.
3. **Then** the ordinary driver work: DP ring programming, `ath11k_core_start()` /
   WMI-HTC, scan/associate, and finally Bluetooth (`hci_qca` + `qcom,wcn6750-bt`, same
   WPSS state, firmware from the vendor partition).

Realistic assessment: if step 1 shows the block simply lives at another physical base,
this is a few more sessions of ordinary work. If it shows the block is unreachable from
the APSS, the port needs the SR_APSS shadow write port — a much bigger and riskier job,
and that is the point at which "mainline Wi-Fi on this device" has to be re-evaluated.
