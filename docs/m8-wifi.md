# M8 — Wi-Fi (WCN6750 "adrastea" on SM4450)

Status at the end of this session: **WPSS and the WLFW QMI handshake work, the
WLAN interface does not come up yet.** The remaining blocker is a chip-variant
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

## 6. Next steps

1. Port the SNOC/adrastea variant into ath11k (items 1–4 of §2.3). The cheapest
   first experiment: keep the current source and add a DT opt-in (e.g.
   `qcom,snoc-direct-mem`) that, after `ath11k_core_pre_init()`, sets
   `ab->hw_params.hybrid_bus_type = false`, `hif_ops = ath11k_ahb_hif_ops_ipq8074`
   and `pci_ops = NULL`; `ab->hw_params` is a struct copy in `ath11k_base`, so
   it can be overridden per device.
2. Resolve the 12 unnamed interrupts for the non-hybrid path
   (`platform_get_irq_byname()`), most likely by comparing with the vendor's
   `icnss2` irq indices.
3. If the firmware then still refuses `wlfw_msa_mem_info`/`msa_ready`, add those
   two QMI messages (they are in the vendor's `wcnss` WLFW headers).
4. Only after `wlan0` appears: `iw reg set`, scan, `wpa_supplicant`, DHCP — and
   then Bluetooth (same WPSS state, `hci_qca` + `qcom,wcn6750-bt`).
