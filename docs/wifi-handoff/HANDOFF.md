# M8 handoff — Wi-Fi / Bluetooth (WCN6750 "adrastea" on SM4450)

> **Working Wi-Fi does not need any of this.** The shipped route is the out-of-tree
> `qcacld-3.0` port in [`sky-qcacld-3.0-mainline`](https://github.com/orange200213/sky-qcacld-3.0-mainline)
> — it works, and is still being optimised (bugs can appear at any time). The pages here
> describe the pure-mainline `ath11k` attempt, which was **closed on 2026-11-02** after
> hardware probing showed the APSS cannot reach the CE/UMAC registers on this SNOC variant.
>
> **This file is the starting point only.** It was written before the port was attempted.
> The work then went much further and is now documented in
> **`HANDOFF-snoc-mainline.md`** (working handoff, kernel #83) and `../m8-wifi.md`
> (project context + experiment log). Read those two for the current state; use this file
> for the firmware inventory and the first-boot recipe, which are still accurate.
>
> Current status in one paragraph: WPSS boots, the full WLFW/QMI contract is satisfied
> (WLFW instance 0, `host_cap`/`ddr_range`, MSA handshake, regdb + BDF download with the
> `total_size`-per-segment fix, the mandatory `CAL_REPORT`, CE0..CE11 rings) and
> `ath11k_core_qmi_firmware_ready()` runs. It is blocked on one hardware question: **where
> the WCSS UMAC/WBM/REO/TCL register block lives on this SNOC variant** — every offset
> documented upstream aborts, the vendor's window-selector registers do not exist on
> adrastea, and the two remaining candidates are a second physical base outside the DT
> region or the SR_APSS shadow write port. Everything else (ext-IRQ dispatch, DP rings,
> WMI/HTC, then BT) comes after that answer.

---

## 1. What is already working / present

| Piece | State |
|---|---|
| `ath11k` + `ath11k_ahb` with `qcom,wcn6750-wifi` | ✅ built into the running kernel (`CONFIG_ATH11K=y`, `CONFIG_ATH11K_AHB=y`) |
| `cfg80211` / `mac80211` | ✅ `=y` |
| `hci_qca` with `qcom,wcn6750-bt` | ✅ in tree and built |
| WPSS remoteproc node | ✅ `remoteproc@8a00000`, `compatible = "qcom,sm4450-wpss-pas"`, driver support present |
| WPSS remoteproc instance | ✅ probes as `/sys/class/remoteproc/remoteproc1`, name `wpss`, **state `offline`** (no firmware yet) |
| ADSP remoteproc | ✅ `remoteproc0` = `adsp`, `running` — proves the PAS/QRTR/glink/QMI machinery works |
| QRTR / glink / QMI / PDR / SMEM / SMP2P | ✅ all `=y` (`QRTR_SMD`, `RPMSG_QCOM_GLINK_SMEM`, `QCOM_QMI_HELPERS`, `QCOM_PDR_HELPERS`, …) |
| WiFi DT node | ⚠️ present (`wifi@22800000`, 12 IRQs, `iommus = <&apps_smmu 0x4c0 0x1>`, `memory-region = <&wlan_msa_mem>`, `qcom,msa-fixed-perm`, four supplies) but **`status = "disabled"`** and **missing `qcom,rproc`** |
| BT DT node | ⚠️ present (`qcom,wcn6750-bt`, supplies mapped, `status = "disabled"`) |
| Reserved memory | `wlan_msa_mem` 0x82a00000 + 3 MB; `wpss_mem` 0x85300000 + 25 MB; both `no-map` |

No kernel rebuild is required for phase 1 and 2 — only DTB + firmware files.

## 2. Firmware: everything is inside the stock `NON-HLOS.bin`

`NON-HLOS.bin` is a FAT16 image; the WLAN chip appears under the name **`adrastea`**
(that is WCN6750's platform name — the Android module is `qca_cld3_adrastea.ko`).

```bash
mkdir -p /tmp/nhlos
sudo mount -o ro,loop,offset=0 \
  sky_in_global_images_V14.0.7.0.TMWINXM_13.0/images/NON-HLOS.bin /tmp/nhlos
ls /tmp/nhlos/image/adrastea
```

Contents (verified on this device's V14.0.7.0.TMWINXM package):

| File(s) | Size | Purpose for mainline |
|---|---|---|
| `wpss.mdt`, `wpss.b00 … wpss.b08` (10 files) | ~6.1 MB total | **WPSS remoteproc firmware** → `/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.mdt` + `wpss.bXX` |
| `bdwlan.bin` | 26,328 B | raw board data (BDF) for this board → ath11k `board.bin`, or convert to `board-2.bin` |
| `bdwlan.<board-id>` (e.g. `bdwlan.102`), `bdwlans.*`, `bdwlant.*`, `bdwlan_{cn,eu,in,jp}.bin`, `bdwlans_*.bin`, `bdwlant_*.bin` | 26 KB each | per-board/per-region variants; pick the one matching the device's `qcom,board-id` |
| `regdb.bin` | 19,348 B | regulatory database → ath11k `regdb.bin` |

The per-board files are named by the low byte(s) of the board id; dump the vendor FDT to get
it (`backup/live-fdt.dtb` in the workspace, or `qcom,board-id` in `build/sky.dts`):

```bash
dtc -I dtb -O dts backup/live-fdt.dtb 2>/dev/null | grep -m2 "board-id"
```

## 3. Phase 1 — boot the WPSS firmware (do this first, it is independent of ath11k)

1. Install the split-MDT set (the ADSP in M6 uses exactly the same layout):

   ```bash
   FW=/lib/firmware/qcom/sm4450/xiaomi-sky      # in the rootfs on the phone
   scp /tmp/nhlos/image/adrastea/wpss.mdt  root@192.168.7.1:$FW/
   scp /tmp/nhlos/image/adrastea/wpss.b0*  root@192.168.7.1:$FW/
   ```

   `firmware-name` in the DT is already `qcom/sm4450/xiaomi-sky/wpss.mbn`; the PAS driver
   accepts either the `.mbn` name with split `wpss.bXX` siblings or an explicit `.mdt` — if
   it complains about `.mbn`, change `firmware-name` to `…/wpss.mdt` (DTB change, no kernel
   rebuild).

2. Start it and watch:

   ```bash
   echo start > /sys/class/remoteproc/remoteproc1/state
   cat /sys/class/remoteproc/remoteproc1/state          # expect: running
   dmesg | grep -iE "wpss|remoteproc|qcom_q6v5"
   ls /sys/bus/rpmsg/devices/ | grep -i wpss            # WLFW / IPCRTR channels appear
   ```

   Expected: `remoteproc remoteproc1: wpss is available`, `… remote processor wpss is now up`,
   then a `glink-edge` under `8a00000.remoteproc` with `WLFW` and `IPCRTR` channels.

   Note the ADSP firmware lives in the rootfs, not the initramfs, and `/init` retries the ADSP
   start (M6). WPSS has `auto_boot = true`, so it is attempted at ~0.4 s — **before
   `switch_root`**, i.e. before the rootfs firmware exists. Either add the WPSS set to the
   initramfs as well, or add a WPSS retry next to the ADSP retry in `/init`.

3. Failure triage: `-ENOENT` → firmware path/name; `-EINVAL`/`-EBUSY` → `wpss_mem` region or
   `qcom,qmp`; a crash without log → check `pstore` (ramoops) *before* rebooting anywhere else.

## 4. Phase 2 — ath11k

1. Install the ath11k firmware set:

   ```bash
   A=/lib/firmware/ath11k/WCN6750/hw1.0
   # board data: simplest form first
   scp bdwlan.bin  root@…:$A/board.bin
   # if the driver asks for the API2 container instead, build board-2.bin from
   # bdwlan.bin + bdwlan.<board-id> with ath11k-bdencoder (or reuse a WCN6750
   # board-2.bin from any SM4450/WCN6750 mainline device and append our BDF).
   scp regdb.bin   root@…:$A/regdb.bin
   # m3.bin: WCN6750 usually carries M3 inside the WPSS image; only provide this
   # file if the driver explicitly logs "failed to load …/m3.bin".
   ```

2. Device tree, in `tools/mksky_dts.py` (never hand-edit the generated DTS):

   ```dts
   &wifi {
           status = "okay";
           qcom,rproc = <&remoteproc_wpss>;   /* required by ath11k_ahb */
   };
   ```

   Keep `qcom,msa-fixed-perm` (the firmware owns its MSA region, the driver must not remap
   it), the 12 `interrupts`, `iommus = <&apps_smmu 0x4c0 0x1>` and the four supplies
   (`vdd-0.8-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`, `vdd-3.3-ch0` — all labels exist in our DTS).

3. Verify:

   ```bash
   dmesg | grep -iE "ath11k|wcn6750|qmi"
   ip link                     # expect wlan0
   iw dev                      # if iw is installed; otherwise /sys/class/ieee80211/
   ```

   ath11k log flow: `qmi service … WLFW` connect → `fw_version` → `board-2.bin`/`board.bin`
   loaded → `htt` init → `wlan0` registered.

## 5. Phase 3 — Bluetooth (separate, do after Wi-Fi)

`hci_qca` already has `qcom,wcn6750-bt`. Remaining work:

* DT: `status = "okay"`, plus `enable-gpios` (BT enable) and the supplies already mapped in
  the node by the generator (`qcom,bt-vdd-io/aon/dig/rfa1/rfa2`).
* Firmware: BT lives in the vendor partition, not `NON-HLOS.bin`; inventory it (TWRP:
  `ls /mnt/vendor/firmware | grep -iE "bt|qca|htbt|htnv"`) and install as
  `/lib/firmware/qca/htbtfw*.tlv`, `/lib/firmware/qca/htnv*.bin` (the exact names are the
  `qca/…` pair shipped for this chip; `hci_qca` logs the file it wants).
* BT needs the same WPSS/AON state as Wi-Fi, so phase 1 must be green first.

## 6. Gotchas (all learned the hard way on this board)

* **Firmware must exist before the remoteproc auto-boot** (`0.4 s`), i.e. put it in the
  initramfs or add a retry in `/init` (this is exactly what M6 had to do for ADSP).
* `/sys/kernel/debug/gpio` **hangs the box** (TZ-protected TLMM page) — never read it.
* `pstore` keeps panics but not hangs; read it before booting TWRP.
* DTB-only changes can be flashed with the current kernel: `python3 tools/mksky_dts.py`,
  `./tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts build`, repack with `shim/tools/mklinuxpayload.py`
  + `shim/build-sky.sh`, flash `boot_a`. Kernel rebuilds are only needed for config changes
  and are known-good now (the handoff13 image that runs touch is such a build), but still
  verify the Image mtime and the version banner before flashing — a failed build silently
  reuses the old Image and the compiler prints Chinese errors (`错误 1`).
* `wlan_msa_mem` is only 3 MB in our DTS; if the firmware fails to load MSA, compare against
  the vendor DT (`build/sky.dts`) — a too-small region shows up as a load failure, not a
  probe failure.

## 7. Definition of done

1. `remoteproc1` (`wpss`) is `running` and its glink channels exist.
2. `ath11k` probes, `wlan0` exists, `ip link set wlan0 up` works and `iw dev wlan0 scan`
   returns SSIDs (needs `CONFIG_CFG80211` + a regulatory domain: `iw reg set`).
3. Associating to a WPA2 AP with `wpa_supplicant` yields an IP address over DHCP.
4. Then start on BT.
