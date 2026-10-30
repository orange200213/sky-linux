# Next-session prompt — touch on sky (copy this as the opening message)

I am bringing up mainline Linux (7.3) on the Xiaomi Redmi 12 5G (`sky`, Qualcomm
SM4450) under `/home/orange/redmi125g/`. The device is currently booted into my kernel:
`sshpass -p sky ssh root@192.168.7.1` works over USB-NCM; `adb` only works in TWRP;
`fastboot` works in the bootloader. Host sudo password: `m912n67oo`.

## Task for this session

**Get the Novatek SPI touchscreen working.** Everything else is already fine:
display (DPU/DSI/m19 panel with DSC), battery + charging (`/sys/class/power_supply/`
shows real values), USB gadget, SSH with stable host keys.

Read first, in this order:

1. `docs/touch-handoff/HANDOFF-TOUCH.md` — hardware, vendor DT values, the panic we
   already hit and why, the I2C-only driver problem, and the step-by-step plan.
2. `docs/touch-handoff/vendor-touch-node.dts` — the vendor node (reset GPIO 12,
   IRQ GPIO 13, SPI at `0xa84000`, IRQ 354, 9.6 MHz).
3. `docs/touch-handoff/logs/panic-extract.txt` — what enabling the node standalone does
   (`geni_se_clks_on()` NULL deref).

## Three facts that shape the work

* The SE node **must** be a child of a `qcom,geni-se-qup` wrapper; standalone it panics.
  Reference shape: `arch/arm64/boot/dts/qcom/sm8550.dtsi` `geniqup@ac0000` +
  `spi1: spi@a84000`.
* `drivers/input/touchscreen/novatek-nvt-ts.c` is an **I2C** driver that validates the
  chip id — it cannot drive our SPI touch. A SPI driver (or a SPI front-end for that
  driver) has to be written. Vendor hints: `novatek,swrst-n8-addr = <0x3f0fe>`,
  `novatek,spi-rd-fast-addr = <0x3f310>`, page protocol `0xFF <page>` / `0xFB 0x01`.
* Our DTB node is currently `status = "disabled"` on purpose. Keep it that way until
  the wrapper, the SM4450 pin groups, and the correct IRQ (354) are in place, so a
  failed experiment does not cost a recovery cycle.

## Working rules

* Kernel/DTB changes need a flash:

  ```bash
  cd ~/redmi125g/sky-mainline/kernel/min-tree
  export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
  make -j10 Image
  cd ../../shim && python3 tools/mklinuxpayload.py \
      ../kernel/min-tree/arch/arm64/boot/Image ../build/sm4450-xiaomi-sky.dtb \
      out/payload.bin --load 0xc0000000
  ./build-sky.sh out/payload.bin
  fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
  ```

* If a change panics and the device bootloops, recover from TWRP without fastboot:

  ```bash
  adb push out/sky-shim-boot-v4.img /tmp/skyboot.img
  adb shell "test -b /dev/block/by-name/boot_a && \
             dd if=/tmp/skyboot.img of=/dev/block/by-name/boot_a bs=1M conv=fsync && sync"
  adb reboot
  ```

  Then pull the crash: `adb pull /sys/fs/pstore/dmesg-ramoops-0.enc.z` and decode with
  `zlib.decompress(data, -15)` (raw deflate, despite the `.enc.z` name).

* Known-good fallback image with touch disabled:
  `shim/out/sky-m7f-safe-no-touch.img`
  (sha256 `b268944d431568a8e484d535a18fbe774367841a9d2d8811d1d37ae2dc8dbcd7`).

* Verify with the device, not by reasoning: `dmesg | grep -iE "geni|spi|novatek"`,
  `/proc/bus/input/devices`, `/proc/interrupts`. And after every flash, confirm the
  display (`cat /proc/fb`) and battery (`cat /sys/class/power_supply/battery/capacity`)
  still work — those are working milestones we must not regress.

* Keep DTS changes in `tools/mksky_dts.py` (the DTS is generated, do not hand-edit
  `dts/sm4450-xiaomi-sky.dts`), keep comments minimal, and write every conclusion and
  dead end into `docs/touch-handoff/` so the next session does not repeat it.

## Suggested order

1. Decode the vendor pinctrl phandles in `build/sky.dts` to get the real SM4450 pins
   and the function names accepted by `pinctrl-sm4450.c`.
2. Add the QUPv3 wrapper 1 node + the SE node inside it (IRQ 354, `se` clock), keep the
   touch chip node disabled, and confirm the controller probes cleanly.
3. Enable the chip node with the SPI driver (new or extended), read the 12-byte
   parameter block, log it raw, and add the chip variant it reports.
4. Only then wire `touchscreen-size-x/y`, `vcc`/`iovcc`, and test with
   `/proc/interrupts` + raw `evdev` reads.
