# Touch handoff #2 — NT36672C on QUPv3 SE1, only the SPI **write** path is broken

This replaces the old `docs/touch-handoff/` (kept for history, but far too long to read).
Everything below was measured on the device.

---

## 1. One-paragraph status

The Novatek NT36672C touch controller is **detected and driven**: the QUPv3 SE at
`0xa84000` works in **GPI DMA mode**, the driver reads the chip ID and parameters, the
input device registers, and the interrupt line fires. What still fails is the **SPI write
path**: downloading the touch firmware to the chip returns `-EIO`, so the chip never
starts scanning and no touch coordinates are reported. **FIFO mode is unusable** on this SE
(the engine accepts the command and never executes it), so DMA mode is mandatory.

---

## 2. Working state (measured)

| Item | Value |
|---|---|
| Kernel | 7.3 + ROCKNIX SM4450 + local bring-up patches, builds and boots |
| Display / battery / USB / SSH | all working |
| SPI controller | `spi0` registers, `spi0.0` present, bound to `NVT-ts-spi` |
| SPI mode | **GPI DMA** (`dma1chan*` allocated; no "fall back to FIFO" message) |
| SE clocks | `gcc_qupv3_wrap1_s1_clk` 19.2 MHz → `a84000.spi se`; `gcc_qupv3_wrap1_core_clk` → `a84000.spi core` |
| Wrapper clocks | `gcc_qupv3_wrap_1_{m,s}_ahb_clk` → `ac0000.geniqup` |
| Pins | `gpio26 clk, gpio27 cs, gpio50 miso, gpio51 mosi`, function `qup1_se1` (verified in pinmux-pins) |
| IRQ | GIC SPI 354 (`a84000.spi`), and GPIO 13 for the touch interrupt (count climbs) |

Chip identification, from the driver log:

```
nvt_ts_check_chip_ver_trim: buf = 04 0C 00 00 72 66 03
This is NVT touch IC
Set default fw_ver=0, x_num=32, y_num=50, abs_x_max=1080, abs_y_max=2460
input: NVTCapacitiveTouchScreen as /devices/virtual/input/input3
request irq 154 succeed
```

Note `fw_ver=0`: the chip is in its ROM/boot state, which is exactly why it answers reads
but does not scan.

## 3. The failure

With `BOOT_UPDATE_FIRMWARE 1` and the vendor firmware installed
(`/lib/firmware/novatek/nt36672c.bin`, 139 264 B, md5 `5db5e09d6a867549e0e5fca266f17d22`,
found at `/vendor/firmware/novatek_ts_fw.bin` in the phone's vendor partition):

```
[NVT-ts-spi] update_firmware_request 304: filename is novatek/nt36672c.bin     (loaded OK)
[NVT-ts-spi] CTP_SPI_WRITE 179: error, ret=-5
[NVT-ts-spi] nvt_write_sram 384: set page failed, ret = -5
[NVT-ts-spi] nvt_write_firmware 449: sram program failed, ret = -5
[NVT-ts-spi] nvt_download_firmware_hw_crc 666: Write_Firmware failed. (-5)
```

`-5 = EIO`, produced by the SPI core when the controller reports a transfer error.
**Reads work, writes do not.**

The firmware end flag is compatible with the driver's scan (`"NVT"` at offset 131069 =
32*4096-3 → `fw_need_write_size = 0x20000`), so the image itself is fine.

## 4. Two dead ends, do not repeat

* **FIFO mode** (no `dmas` in the SPI node): the SE accepts the command, `SE_GENI_STATUS`
  stays 0, the driver times out and retries forever, which floods the console and looks
  like a hang. Measured twice, with and without the QUP firmware experiments.
* **Calling `geni_load_se_firmware()` from `spi_geni_probe()`**: panics on SM4450, both
  before and after `spi_geni_init()`. Reverted. Note mainline already calls it from
  `spi_geni_init()` when the SE reports `GENI_SE_INVALID_PROTO`; our SE reports SPI, so the
  firmware path is not used.

## 5. Prime suspect for the write failure

`gpi_dma1` currently has `dma-channel-mask = <0x3e>`, i.e. **only channels 1..5 are
usable**, while the TX channel is requested as **chid 0**:

```dts
dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>,   /* chid 0 → TX */
       <&gpi_dma1 1 1 QCOM_GPI_SPI>;   /* chid 1 → RX */
```

RX (chid 1) is inside the mask, TX (chid 0) is not — which matches "reads work, writes
fail". Candidate fixes, cheapest first:

1. `dma-channel-mask = <0xfff>` (or delete the property) so channel 0 is usable.
2. Keep the mask and move TX to `chid 2`: `<&gpi_dma1 2 1 QCOM_GPI_SPI>` for TX.
3. Both, then also verify with `spi-max-frequency` at 2.4 MHz (divides the 19.2 MHz SE
   clock by 8 exactly) rather than 1 MHz (non-integer divider).

## 6. Current DT (the node as built)

`spi-node.dts` and `gpi_dma1-node.dts` in this folder are the exact excerpts.
The generator that produces them is `tools/mksky_dts.py` (never hand-edit the generated
`dts/sm4450-xiaomi-sky.dts`).

Kernel side, keep these:

* `drivers/soc/qcom/qcom-geni-se.c`: `if (!wrapper) return -EPROBE_DEFER;` and a NULL
  check around `clk_prepare_enable(se->core_clk)` — without them a missing wrapper/clock
  is a NULL dereference at 0x30 (upstreamable bug).
* `drivers/spi/spi-geni-qcom.c`: the `sky-spi-xfer` / `sky-spi-irq` probes print
  `SE_GENI_STATUS`, `M_CMD0`, `S_IRQ`. Keep them until touch works: `status=00000000`
  means the SE is not executing, `m_irq=0x21` is our own cancel (DONE|ABORT).
* `drivers/input/touchscreen/nt36xxx-spi/`: the imported driver (patches 0018+0019 from
  `effeffe/nairoArchLinuxArmTools`), `BOOT_UPDATE_FIRMWARE 1`, plus a no-op
  `nvt_update_firmware()` in the `#else` branch so the tree still links with it disabled.

## 7. Images and how to build/flash

| Image | What it is |
|---|---|
| `shim/out/sky-t25-dma-2m4.img` | latest: DMA + 2.4 MHz + firmware enabled (untested) |
| `shim/out/sky-t21-touch-fw.img` | DMA at 1 MHz + firmware: reads OK, writes `-EIO` |
| `shim/out/sky-t20-nt36xxx-real.img` | DMA, no firmware download: chip detected, no scan |
| `shim/out/sky-t24-fifo-fw.img` | FIFO: unusable, console flood |
| `shim/out/sky-m7f-safe-no-touch.img` | fallback with the SPI node disabled entirely |

```bash
# build kernel + DTB + boot image
cd ~/redmi125g/sky-mainline/kernel/min-tree
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -j10 Image                      # check for "error:" AND "Error N" AND "错误"
cd ../../ && python3 tools/mksky_dts.py && ./tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts build
cd shim && python3 tools/mklinuxpayload.py ../kernel/min-tree/arch/arm64/boot/Image \
    ../build/sm4450-xiaomi-sky.dtb out/payload.bin --load 0xc0000000
./build-sky.sh out/payload.bin

# flash from fastboot
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot

# flash from TWRP (no fastboot needed)
adb push out/sky-shim-boot-v4.img /tmp/skyboot.img
adb shell "dd if=/tmp/skyboot.img of=/dev/block/by-name/boot_a bs=1M conv=fsync && sync"
adb reboot
```

**Gotcha that cost a whole cycle:** a failed kernel build silently leaves the old `Image`
in place, and the packer happily ships it. Always check that
`arch/arm64/boot/Image` is newer than the build start *and* that the version banner in the
packed image matches (`strings -a out/x.img | grep 'SMP PREEMPT'`). The Chinese build
errors read "错误 1", not "error:".

## 8. Useful runtime checks

```bash
ls /sys/bus/spi/devices/                     # spi0.0 must exist
readlink /sys/bus/spi/devices/spi0.0/driver  # NVT-ts-spi
dmesg | grep -iE "NVT-ts|nvt_|sky-spi"       # driver + SE state
grep -E "154:" /proc/interrupts              # touch IRQ count (0 = chip not scanning)
ls /lib/firmware/novatek/                    # nt36672c.bin (139264 B)
```

Do **not** read `/sys/kernel/debug/gpio`: on this board that hits a TZ-protected TLMM page
and kills the box (same class of fault as the earlier `msm_gpio_get_direction` SEA).
`/sys/kernel/debug/pinctrl/*/pinmux-pins` is safe.

## 9. Definition of done

1. `nvt_download_firmware` completes, `fw_ver` becomes non-zero, `PID` is reported.
2. IRQ count grows while a finger is on the screen and `/dev/input/event*` emits
   `ABS_MT_POSITION_X/Y` in 0..1079 / 0..2459.
3. Coordinates match the panel orientation (1080x2460 portrait); fix with
   `touchscreen-swapped-x-y` / `touchscreen-inverted-x/y` in the DT if needed.
4. Commit the final DT + driver patches, update `docs/m7-touch.md` and the README row.

---

## 10. Reference implementations worth knowing

* **MediaTek mt6895 / tb375fc** uses the same controller:
  `https://github.com/u0n0u/tb375fc-linux` → `arch/arm64/boot/dts/mediatek/mt6895.dtsi`
  (node `spi@11012000`, child `novatek@0`). Useful facts from it:
  * `spi-max-frequency = <9600000>` (confirms the 9.6 MHz chip clock);
  * downstream uses `novatek,irq-gpio` + three pinctrl states
    (`pmx_ts_active`, `pmx_ts_suspend`, `pmx_ts_spimode`) instead of the generic
    `interrupts` property; our ported driver uses GPIO descriptors + `interrupts`, so
    no DT change is needed;
  * it selects firmware per panel variant with `novatek,cfg_0/cfg_1`
    (`novatek,fw-name`, `novatek,mp-name`, `novatek,tp-vendor`, `novatek,display-maker`),
    i.e. one image per panel supplier. Our vendor partition only ships the generic
    `novatek_ts_fw.bin`, which is what we use;
  * the firmware file names there are `novatek_nt36672e_l16_fw01/02.bin`, and our vendor
    partition carries `qdcm_calib_data_nt36672e_...json` — so **the DDIC is NT36672E**
    (the vendor module is named `nvt_36672c.ko`, but that is the driver, not the IC).
    The imported mainline driver maps NT36672C to the NT36675 memory map; if firmware
    download ever succeeds but the chip misbehaves, the memory map is the next thing to
    look at.
  No value for the current blocker: the MTK SPI controller is a different block, so it
  says nothing about the QUPv3 SE/DMA write failure.

## 11. Attempt log (newest first)

| Image | Change | Result |
|---|---|---|
| `sky-t29-mask-all.img` | TX chid 0, `dma-channel-mask = <0xfff>`, 1 MHz | built, not yet flashed |
| `sky-t28-stable-nofw.img` | `BOOT_UPDATE_FIRMWARE 0` (no write attempts, no retry loop) | built, not yet flashed |
| `sky-t27-dma-tx2-2m4.img` | TX moved to chid 2, 2.4 MHz | **regression**: SE stops executing at all (chip id read returns zeros), endless timeouts, console flood |
| `sky-t26-dma-mask-2m4.img` | mask changed, 2.4 MHz | untested |
| `sky-t25-dma-2m4.img` | DMA restored, 2.4 MHz | untested |
| `sky-t21-touch-fw.img` | DMA, 1 MHz, firmware enabled | **best state**: chip detected, IRQ fires, reads OK, writes `-EIO` |
| `sky-t24-fifo-fw.img` | FIFO (no dmas) | SE never executes, infinite retry loop, box appears hung |
| `sky-t22-dma-noiommu.img` | GPI DMA without `iommus` | `gpi a00000.dma-controller: Unhandled interrupt status:0x40`, hang |

Lesson from T27: the TX/RX channel ids must stay 0 and 1; moving TX to chid 2 breaks the
whole DMA path, not just writes.
