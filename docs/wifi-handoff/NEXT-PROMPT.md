# Next-session prompt — Wi-Fi on the Xiaomi Redmi 12 5G (WCN6750 / SM4450)

Copy this as the opening message of the next session.

---

I am bringing up mainline Linux 7.3 on the Xiaomi Redmi 12 5G (`sky`, Qualcomm SM4450 /
"ravelin"). Display, battery/charging, USB, SSH, storage and **touch** all work. The next
milestone is **Wi-Fi (WCN6750 "adrastea"), then Bluetooth**.

Working directory `/home/orange/redmi125g/`, kernel tree `sky-mainline/kernel/min-tree/`,
public repo `sky-linux/` (push to `github.com/orange200213/sky-linux`).
Device: `sshpass -p sky ssh root@192.168.7.1` when my kernel is booted, `fastboot` in the
bootloader, `adb` in TWRP; host sudo password `m912n67oo`.

**Read `docs/wifi-handoff/HANDOFF.md` first — it is self-contained.** Do not re-read the
touch handoffs unless you need the QUP/SPI background.

## Where things stand

* The running kernel already has everything: `ATH11K`, `ATH11K_AHB` (with
  `qcom,wcn6750-wifi`), `MAC80211`, `QRTR`, `RPMSG_QCOM_GLINK_SMEM`, `QCOM_QMI_HELPERS`,
  `QCOM_Q6V5_PAS` (with `qcom,sm4450-wpss-pas`).
* The WPSS remoteproc **already probes**: `/sys/class/remoteproc/remoteproc1`, name `wpss`,
  state **offline** — it just has no firmware.
* The ADSP remoteproc works (`running`), so the QRTR/glink/QMI plumbing is proven.
* Firmware for both phases is inside the stock `NON-HLOS.bin` FAT image, directory
  `image/adrastea/`: `wpss.mdt` + `wpss.b00..b08`, `bdwlan.bin`, per-board `bdwlan.<id>`,
  and `regdb.bin`. The handoff has the mount command and the full file table.
* `wifi@22800000` exists in our DTS with IRQs, IOMMU, `wlan_msa_mem` and four supplies, but
  is `status = "disabled"` and lacks `qcom,rproc = <&remoteproc_wpss>`.

## Do this, in order

1. **Phase 1 — WPSS boots.** Copy `wpss.mdt` + `wpss.bXX` into
   `/lib/firmware/qcom/sm4450/xiaomi-sky/`, `echo start > …/remoteproc1/state`, and get
   `state=running` plus a `glink-edge` with `WLFW`/`IPCRTR` channels. Remember the auto-boot
   happens at ~0.4 s, before `switch_root` — if the firmware is not found, add the files to
   the initramfs or a retry in `/init` (M6 did exactly this for ADSP).
2. **Phase 2 — ath11k.** Install `bdwlan.bin` as `ath11k/WCN6750/hw1.0/board.bin` (fall back
   to a proper `board-2.bin` if the driver asks for the API2 container) plus `regdb.bin`,
   enable the wifi node and add `qcom,rproc`, then verify `wlan0` appears and
   `iw dev wlan0 scan` lists SSIDs.
3. **Phase 3 — Bluetooth.** `hci_qca` supports `qcom,wcn6750-bt`; needs `status = "okay"`,
   `enable-gpios`, and the vendor-partition BT firmware under `/lib/firmware/qca/`. Only
   start after phase 1 is green.

## Constraints

* Change the DT through `tools/mksky_dts.py`, never the generated
  `dts/sm4450-xiaomi-sky.dts`. DTB-only changes need no kernel rebuild: regenerate the DTS,
  rebuild the DTB, repack with `shim/tools/mklinuxpayload.py` + `shim/build-sky.sh`, flash
  `boot_a`.
* Never read `/sys/kernel/debug/gpio` (TZ-protected TLMM page → box hangs).
* `pstore` records panics but not hangs — read it before booting TWRP.
* If you rebuild the kernel, verify the Image really changed (mtime + `SMP PREEMPT` banner);
  a failed build silently ships the previous Image and the compiler prints Chinese errors
  (`错误 1`), so grep for `error:|Error [0-9]|错误`.
* Keep commits small and push to `sky-linux` with a `M8 wifi: …` prefix.

## Definition of done

`remoteproc1` running, `ath11k` probes, `wlan0` up, `iw dev wlan0 scan` returns SSIDs, and a
WPA2 association with `wpa_supplicant` gets an address over DHCP. Then write
`docs/m8-wifi.md`, update the README milestone row, and push.
