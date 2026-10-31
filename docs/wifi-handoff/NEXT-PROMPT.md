# Next-session prompt — Wi-Fi on the Xiaomi Redmi 12 5G (WCN6750 / SM4450)

Copy this as the opening message of the next session.

---

I am bringing up mainline Linux 7.3 on the Xiaomi Redmi 12 5G (`sky`, Qualcomm
SM4450 / "ravelin"). Display, battery/charging, USB, SSH, storage and touch all
work. The current milestone is **Wi-Fi (WCN6750 "adrastea")**.

Working directory `/home/orange/redmi125g/`, kernel tree
`sky-mainline/kernel/linux-7.3/`, public repo `sky-linux/`. Device:
`sshpass -p sky ssh root@192.168.7.1`, `fastboot` in the bootloader on the host,
host sudo password `m912n67oo`.

**Read `sky-linux/docs/m8-wifi.md` first** (previous session's full record) and
`sky-linux/docs/wifi-handoff/FINDINGS-session2.md` (what the first handoff got
wrong).

## Where things stand

* WPSS remoteproc works: `qcom/sm4450/xiaomi-sky/wpss.mdt` + `wpss.bXX` from the
  stock `NON-HLOS.bin` (`image/adrastea/`), started by `/init` via
  `/usr/local/sbin/sky-wifi-up.sh` (the auto-boot at 0.4 s is before
  `switch_root`, so it always fails).
* ath11k probes (`17210040.wifi`), the WLFW QMI service is found (needs kernel
  patch `kernel/patches-m8/0001`, instance 0 instead of upstream's 3), and the
  firmware answers the host-cap/target-cap requests:
  `chip_id 0x4130 … fw_version 0x343b815a CI_WLAN.HL.3.4.3-00346.1`.
* **Blocker:** `QMI_WLANFW_DEVICE_INFO` returns `QMI_ERR_NOT_SUPPORTED` (94).
  This chip is the **SNOC/adrastea** variant (the stock DT enables
  `qcom,icnss@22800000`, not the disabled hybrid `qcom,wcn6750` node), while
  mainline `ath11k` only implements the hybrid/PCIE variant, where that request
  is what supplies the register BAR (`ab->mem`). No `wlan0` yet.
* Kernel `#62` (patches 0001 + 0002) and a hybrid-layout DTB are flashed;
  `wifi-backup/boot/boot_a-original.img` restores M7, `wifi-backup/out/` holds
  the rebuilt boot images and the scripts that produced them.

## Do this, in order

1. **Port the SNOC variant** into `drivers/net/wireless/ath/ath11k/ahb.c`:
   after `ath11k_core_pre_init()` (hw params are a *copy* in `ath11k_base`),
   when the DT asks for it, set `ab->hw_params.hybrid_bus_type = false`,
   `hif_ops = &ath11k_ahb_hif_ops_ipq8074`, `pci_ops = NULL`. That makes
   `ath11k_ahb_setup_resources()` ioremap `reg[0]` (`membase`, 0x22800000)
   and makes `ath11k_qmi_request_device_info()` a no-op.
2. Give the 12 stock interrupts names so the non-hybrid
   `ath11k_ahb_config_irq()` (`platform_get_irq_byname()` with
   `misc-pulse1/misc-latch/sw-exception/watchdog/ce0..ce11`) can request them;
   the vendor `icnss2` uses them by index, so the order still has to be
   established.
3. If the firmware then refuses the MSA handshake, add the vendor's
   `wlfw_msa_mem_info` + `wlfw_msa_ready` requests (ath11k has no equivalent).
4. Check BDF: `board.bin` is currently `bdwlan.bin`; per-board variants are
   `bdwlan.<id>` in `NON-HLOS.bin` (`image/adrastea/`) and `board-2.bin`
   (API2) can hold them all. Board id of the device: `0x1000b`.
5. `dts/sm4450-xiaomi-sky.dts` already carries the SNOC-correct wifi node
   (membase, 12 IRQs, SID 0x4c0, `l11/l23/l14/l7` rails, `qcom,rproc`, wlan
   smp2p state) — build the DTB from it for the port, and remember the DTB in
   use on the phone is still the hybrid one.

## Constraints

* Never hand-edit the *generated* DTS assumptions blindly: the checked-in
  `dts/sm4450-xiaomi-sky.dts` was verified against the live DT node by node for
  the wifi part, and it builds (`KDIR=… tools/dtsbuild.sh`).
* DTB+Image changes are packed with `wifi-backup/repack-boot.py` (it keeps the
  shim prefix, ramdisk, `load_pa`/`dtb_pa` byte-identical and only swaps the
  DTB / Image — verify with the diff check it prints).
* Never read `/sys/kernel/debug/gpio` (TZ-protected TLMM page → box hangs).
* `unbind`ing a *stalled* ath11k used to panic (NULL deref in
  `ath11k_ce_cleanup_pipes()`); patch 0002 is the upstream fix and is already
  in the tree, but a reboot is still the safe way to retry a probe.
* Rebuilds: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j12 Image` in
  the tree, then verify the banner/build number and `grep -cE "error:|错误"` the
  log.
* Keep commits small, `M8 wifi: …` prefix, push to `sky-linux`.

## Definition of done

`wlan0` exists, `iw dev wlan0 scan` returns SSIDs (`iw reg set` first), a WPA2
association with `wpa_supplicant` gets an address over DHCP; then update
`docs/m8-wifi.md` and start on Bluetooth (`hci_qca`, `qcom,wcn6750-bt`).
