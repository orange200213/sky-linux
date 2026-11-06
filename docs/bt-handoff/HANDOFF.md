# M9 handoff — Bluetooth (WCN6750 "adrastea", `hci_qca` over geni UART)

Status: **not started.** Everything on the kernel side is already compiled in; what is missing
is a correct device-tree node and the BT firmware files. This document is written from the
tree and the stock images — nothing below has been run on hardware yet, and it says so where
it matters.

---

## 1. What already exists

| Piece | State |
|---|---|
| `CONFIG_BT`, `CONFIG_BT_HCIUART`, `CONFIG_BT_HCIUART_QCA`, `CONFIG_BT_QCA`, `CONFIG_RFKILL` | all `=y` in the running kernel |
| `hci_qca` support for `qcom,wcn6750-bt` (`qca_soc_data_wcn6750`) | present in `drivers/bluetooth/hci_qca.c` |
| transport UART | `uart2: serial@988000` (QUPv3 **wrapper 0, SE2**), `qcom,geni-uart`, **`status = "okay"`** |
| BT node | child of `uart2`: `compatible = "qcom,wcn6750-bt"`, **`status = "disabled"`** |
| regulators in the node | vendor-style names (`qcom,bt-vdd-io/aon/dig/rfa1/rfa2-supply`) — **`hci_qca` ignores these** |
| `max-speed` | `<3200000>` ✓ |

UART details as built today:

```dts
uart2: serial@988000 {
	compatible = "qcom,geni-uart";
	clocks = <&gcc GCC_QUPV3_WRAP0_S2_CLK>;
	interrupts-extended = <&intc GIC_SPI 603 IRQ_TYPE_LEVEL_HIGH>,
			      <&tlmm 17 IRQ_TYPE_EDGE_FALLING>;   /* BT wake / host-wake line */
	pinctrl-0 = <&qup_uart2_default>;   /* gpio14 CTS, gpio15 RTS, gpio16 TX, gpio17 RX */
	pinctrl-1 = <&qup_uart2_sleep>;
	status = "okay";

	bluetooth: bluetooth {
		compatible = "qcom,wcn6750-bt";
		status = "disabled";
	};
};
```

## 2. What `hci_qca` actually needs (and we do not have)

1. **`enable-gpios`** — for `QCA_WCN6750` the driver either finds an `enable-gpios` or looks
   for a `bluetooth` **power sequencer** (`devm_pwrseq_get(..., "bluetooth")`). Our node has
   neither, so `qca_init_regulators()`/probe will not complete. The vendor DT gives the two
   candidate pins:

   ```dts
   qcom,bt-reset-gpio    = <&tlmm 39 GPIO_ACTIVE_HIGH>;   /* vendor 0x27 */
   qcom,bt-sw-ctrl-gpio  = <&tlmm 56 GPIO_ACTIVE_HIGH>;   /* vendor 0x38 */
   ```

   One of them is `BT_EN`. Start with `enable-gpios = <&tlmm 56 GPIO_ACTIVE_HIGH>;` (the
   "switch control" pin is what the vendor's `wcnss`/`btqca` toggles as BT_EN) and keep 39 as
   a reset pulse if the first attempt does not answer.

2. **The nine mainline rail names**, exactly as `qca_soc_data_wcn6750` lists them:
   `vddio-supply`, `vddaon-supply`, `vddbtcxmx-supply`, `vddrfacmn-supply`,
   `vddrfa0p8-supply`, `vddrfa1p7-supply`, `vddrfa1p2-supply`, `vddrfa2p2-supply`,
   `vddasd-supply`. Our board only has five distinct rails; the mapping from the vendor's
   five `qcom,bt-vdd-*` entries (`io 1.8 V`, `aon 1.12 V`, `dig 1.12 V`, `rfa1 1.872 V`,
   `rfa2 1.352 V`) to the nine names has to be worked out — several of the nine are the same
   PMIC rail under different names (vendor-only loads), and `vddasd`/`vddbtcxmx` may not exist
   on this board at all, in which case a dummy/always-on fixed regulator is the way to go.

3. **Firmware.** For `QCA_WCN6750`, `btqca.c` requests
   `qca/msbtfw<rom>.mbn` (ELF patch; `.tlv` is the fallback) and `qca/msnv<rom>.bin`, where
   `<rom>` comes from the chip's version response (0x10/0x11/0x12 are the usual values).

   * `BTFM.bin` in the stock package is **not enough**: it is a FAT image with only the older
     `ap*`/`cm*`/`cr*` stems.
   * **Inventory done (2026-11-06, TWRP):** the phone has a dedicated **`bluetooth_a`
     partition** (`/dev/block/by-name/bluetooth_a`, FAT16, ~1.1 MB; Android mounts it at
     `/vendor/bt_firmware`). It contains 30 files and **no `ms*` files at all**:

     ```
     apbtfw10.tlv  apbtfw11.tlv  apnv10.bin  apnv11.bin
     cmbtfw12/13.tlv (+.ver)     cmnv12/13*.bin
     crbtfw11/20/21.tlv          crnv11/20/21.bin
     ```

     A copy now lives outside git at `~/redmi125g/firmware-jp-2024/bt/` (checksums in
     `SHA256SUMS.txt` next to it).

   * **Therefore the `qcom,wcn6750-bt` path cannot find firmware on this device** (it asks for
     `qca/msbtfw<rom>.mbn` + `qca/msnv<rom>.bin`). The pragmatic route is to bind the node as
     **`qcom,wcn3990-bt`** (or wcn3991/3998), for which `btqca` requests
     `qca/crbtfw%02x.tlv` + `qca/crnv%02x.bin` — exactly the files that exist here
     (`crbtfw11/20/21`, `crnv11/20/21`). Which rom version is used depends on the chip's
     version response; install all three pairs and let the driver pick:

     ```sh
     cp crbtfw1{1}.tlv crbtfw2{0,1}.tlv crnv1{1}.bin crnv2{0,1}.bin  /lib/firmware/qca/
     ```
   * Keep the `qcom,wcn6750-bt` compatible in the DT as a fallback experiment, but expect
     `-ENOENT` on the patch upload.

## 3. Iterating without a full flash cycle

* **Modules**: `CONFIG_MODULES=y`, `CONFIG_MODULE_SIG` *not set* (unsigned modules load), but
  **`CONFIG_MODULE_UNLOAD` is not set** — `insmod` works, `rmmod` does **not**; every module
  experiment costs a reboot. vermagic must match exactly
  (`7.3.0-g78458d4eea4e-dirty` at the time of writing). The BT stack itself is built-in, so
  modules are not needed for BT.
* **Firmware** can be dropped in at run time (`/lib/firmware/qca/…`) — no flash needed.
* **Device tree** normally needs a rebuild+flash, but `CONFIG_OF_OVERLAY=y` and
  `CONFIG_CONFIGFS_FS=y`, so an overlay can be applied at run time:

  ```sh
  mkdir /sys/kernel/config/device-tree/overlays/bt
  cat bt-enable.dtbo > /sys/kernel/config/device-tree/overlays/bt/dtbo
  # observe: dmesg | tail   (and rmdir the directory to remove it)
  ```

  This has **not been tried on this board** — it is worth ten minutes, because it would let
  us iterate the BT node without touching `boot_a`.

## 4. Planned steps

1. **Inventory the firmware** (TWRP): `ls /vendor/bt_firmware`, confirm the exact `msbtfw*` /
   `msnv*` names and sizes; copy them to the host.
2. **Rewrite the BT node** mainline-style in `tools/mksky_dts.py` (never the generated DTS):

   ```dts
   bluetooth: bluetooth {
           compatible = "qcom,wcn6750-bt";
           status = "okay";
           enable-gpios = <&tlmm 56 GPIO_ACTIVE_HIGH>;
           max-speed = <3200000>;
           vddio-supply = <&vreg_l21b_1p8>;
           vddaon-supply = <&vreg_s7b_1p12>;
           /* vddbtcxmx / vddrfacmn / vddrfa0p8 / vddrfa1p7 / vddrfa1p2 /
            * vddrfa2p2 / vddasd — map or stub, see §2.2 */
   };
   ```

3. **Install the firmware** into `/lib/firmware/qca/` (rootfs, not the initramfs — BT comes up
   long after `switch_root`).
4. **Boot and read the log**:

   ```sh
   dmesg | grep -iE "hci|bluetooth|qca|btqca|serdev"
   rfkill list
   hciconfig -a            # or: bluetoothctl -> "list", "show"
   ```

   Expected flow: `hci_qca` binds the serdev → regulators on → `enable` pulse → version
   request (fails with a "wrong version" style log if BT_EN/pins are wrong) → patch + NVM
   download (`qca/msbtfw…`, `qca/msnv…`) → `hci0` registered → `bluetoothctl scan on`.

5. Only after `hci0` exists: test pairing/audio-profile level behaviour; note that audio over
   BT needs the audio stack (WCD937x) which is still missing, so expect HID/A2DP-sink-side
   issues later.

## 5. Traps

* **Do not read `/sys/kernel/debug/gpio`** — TLMM is TZ-protected on this board and the box
  hangs.
* `enable-gpios` polarity matters; the vendor's `qcom,bt-sw-ctrl-gpio` is active-high in its
  DT, but confirm on the scope of the driver log rather than guessing twice.
* BT shares the WCN6750 supplies with Wi-Fi; the qcacld Wi-Fi stack already leaves some rails
  on. If BT fails only when Wi-Fi is up, look for a regulator refcount conflict
  (`regulator_bulk_get` with the same supply twice).
* The UART has CTS/RTS (gpio14/15) — if the firmware wants 3.2 Mbaud with flow control, keep
  those pins muxed and do not set `no-flow-control`.
* `hci_qca` needs the **serdev** binding: the `bluetooth` child must be a direct child of the
  UART node (it is) and the UART must not be used by the console.
* If `msbtfw*.mbn` is missing, `btqca` retries with `.tlv`; if both are missing the probe
  fails with `-ENOENT` after the version handshake, which looks like a hardware problem but
  is not.
* Firmware goes in the **rootfs**; the initramfs is only needed for the things that must run
  before `switch_root` (ADSP, WPSS).

## 6. Definition of done

`hci0` appears in `hciconfig -a` / `bluetoothctl list`, `rfkill` shows it unblocked, and
`bluetoothctl scan on` lists nearby devices. Then update the README (M9) and write
`docs/m9-bluetooth.md`.
