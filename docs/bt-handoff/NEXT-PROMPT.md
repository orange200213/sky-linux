# Next prompt — Bluetooth (WCN6750) on the Xiaomi Redmi 12 5G (`sky`)

Copy this as the opening message of the next session.

---

I am bringing up mainline Linux 7.3 on the **Japanese-market** Xiaomi Redmi 12 5G (`sky`,
SM4450 "ravelin", stock `sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`). Display, storage,
USB networking/SSH, power/charging, touch (partial) and **Wi-Fi** (out-of-tree `qcacld-3.0`)
work. Next milestone is **M9: Bluetooth**, the WCN6750's BT half driven by `hci_qca` over the
geni UART.

Working directory `/home/orange/redmi125g/`; kernel `sky-mainline/kernel/linux-7.3`; DTS
generator `sky-mainline/tools/mksky_dts.py`; public repo `sky-linux/` (push to
`github.com/orange200213/sky-linux`). Device: `sshpass -p sky ssh root@192.168.7.1`, `fastboot`
in the bootloader, `adb` in TWRP (that is where the vendor partition is readable); host sudo
password `m912n67oo`.

**Read `docs/bt-handoff/HANDOFF.md` first** — it is self-contained (node as built, what
`hci_qca` needs, firmware naming, iteration tricks, traps).

## Where it stands

* BT stack is already built in: `CONFIG_BT=y`, `BT_HCIUART=y`, `BT_HCIUART_QCA=y`,
  `BT_QCA=y`, `CONFIG_RFKILL=y`; `hci_qca` knows `qcom,wcn6750-bt`.
* The transport exists: `uart2: serial@988000` (QUPv3 wrapper 0 SE2) is `status = "okay"`,
  pins gpio14/15/16/17, wake line `&tlmm 17`, `max-speed = <3200000>`.
* The BT child node is `status = "disabled"` and carries **vendor-style** properties
  (`qcom,bt-vdd-*`, no `enable-gpios`) that mainline ignores.

## Do this, in order

1. **Find the firmware.** In TWRP: `ls /vendor/bt_firmware` (the `bt_firmware` partition is
   mounted under `/vendor/`). For `QCA_WCN6750`, `btqca` wants `qca/msbtfw<rom>.mbn`
   (+ `.tlv` fallback) and `qca/msnv<rom>.bin`; vendor references suggest rom `0x12`.
   `BTFM.bin` from the ROM package is **not enough** (it only has the older `ap*`/`cm*`/`cr*`
   stems). Copy whatever is there to the host and install it into the phone's
   `/lib/firmware/qca/` (rootfs — BT starts long after `switch_root`).
2. **Rewrite the BT node** in `tools/mksky_dts.py`: `status = "okay"`,
   `enable-gpios = <&tlmm 56 GPIO_ACTIVE_HIGH>` (vendor `qcom,bt-sw-ctrl-gpio`; keep gpio39,
   the vendor `qcom,bt-reset-gpio`, as a reset pulse if BT does not answer), `max-speed`,
   and the **nine** mainline rail names (`vddio`, `vddaon`, `vddbtcxmx`, `vddrfacmn`,
   `vddrfa0p8`, `vddrfa1p7`, `vddrfa1p2`, `vddrfa2p2`, `vddasd`) mapped onto our PMIC rails
   or stubbed with fixed always-on regulators.
3. **Try to avoid a flash cycle.** `CONFIG_OF_OVERLAY=y` + configfs means a DT overlay can be
   applied at run time (`/sys/kernel/config/device-tree/overlays/<name>/dtbo`); this has not
   been tried on this board and is worth ten minutes. Note `CONFIG_MODULE_UNLOAD` is **not**
   set: `insmod` works, `rmmod` does not, so module experiments cost a reboot. The BT stack is
   built-in anyway.
4. **Boot and read**: `dmesg | grep -iE "hci|qca|btqca|serdev"`, `rfkill list`,
   `hciconfig -a`. Success = `hci0` exists and `bluetoothctl scan on` lists devices.

## Constraints

* Never read `/sys/kernel/debug/gpio` (TZ-protected TLMM → box hangs).
* `hci_qca` requires either `enable-gpios` or a `bluetooth` power sequencer — the node has
  neither today.
* BT shares the WCN6750 rails with Wi-Fi (qcacld leaves some on); a refcount conflict shows up
  as BT failing only when Wi-Fi is up.
* Keep the UART CTS/RTS pins muxed; do not disable flow control.
* DT changes go through `tools/mksky_dts.py`; verify the Image/DTB timestamps and the
  `SMP PREEMPT` banner before flashing (a failed build silently ships the old Image, and the
  compiler prints Chinese errors — grep for `error:|Error [0-9]|错误`).
* Audio over BT will not work yet: the audio stack (WCD937x codec + LPASS) is still missing.

## Definition of done

`hci0` up, `rfkill` unblocked, `bluetoothctl scan on` lists nearby devices. Then write
`docs/m9-bluetooth.md`, update the README M9 row, and push.
