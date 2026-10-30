# Next-session prompt — finish the NT36672C touch driver (copy this as the opening message)

I am bringing up mainline Linux 7.3 on the Xiaomi Redmi 12 5G (`sky`, Qualcomm SM4450).
Working directory `/home/orange/redmi125g/`. Device access: `sshpass -p sky ssh
root@192.168.7.1` over USB-NCM when my kernel is booted, `fastboot` in the bootloader,
`adb` in TWRP. Host sudo password: `m912n67oo`.

Display, battery/charging, USB gadget and SSH all work. **One thing is left: the touch
controller works for reads but the SPI write path fails, so the chip never starts
scanning.**

Read these two files first, nothing else:

1. `docs/touch-handoff2/HANDOFF.md` — measured state, the exact failure, the prime
   suspect, dead ends not to repeat, build/flash recipe.
2. `docs/touch-handoff2/spi-node.dts` and `gpi_dma1-node.dts` — the current DT excerpts.
   (`docs/touch-handoff/` is the older, very long history; consult only if you need the
   vendor reverse-engineering detail.)

## The problem in one screenshot of dmesg

```
[NVT-ts-spi] update_firmware_request: filename is novatek/nt36672c.bin      (loaded OK)
[NVT-ts-spi] CTP_SPI_WRITE 179: error, ret=-5
[NVT-ts-spi] nvt_write_sram 384: set page failed, ret = -5
[NVT-ts-spi] nvt_download_firmware_hw_crc 666: Write_Firmware failed. (-5)
```

Reads work (chip id `04 0C 00 00 72 66 03`, `abs_x_max=1080`, `abs_y_max=2460`, input
device registered, IRQ 154 fires). Writes return `-EIO`.

## Prime suspect, try this first

`gpi_dma1` has `dma-channel-mask = <0x3e>` (only channels 1..5), but TX is requested as
**chid 0**:

```dts
dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>,   /* TX, chid 0 — outside the mask */
       <&gpi_dma1 1 1 QCOM_GPI_SPI>;   /* RX, chid 1 — inside the mask */
```

Cheapest experiments, in order:

1. `dma-channel-mask = <0xfff>` (or drop the property).
2. Keep the mask, move TX to chid 2.
3. Combine with `spi-max-frequency = <2400000>` (exact /8 of the 19.2 MHz SE clock) —
   already set in the current build.

Then confirm with:

```bash
dmesg | grep -iE "NVT-ts|nvt_"          # firmware download / fw_ver
grep -E "154:" /proc/interrupts          # count grows while touching
```

If writes still fail, the next things to look at are the GPI DMA channel configuration in
`drivers/dma/qcom/gpi.c` (what `gpi_of_dma_xlate`/`gpi_send_cmd` expect for
`QCOM_GPI_SPI`), and the geni SPI TX descriptor path in
`drivers/spi/spi-geni-qcom.c` (`spi_geni_gsi_tx`/`gsi_tx_config`). The `sky-spi-xfer` and
`sky-spi-irq` probes in that file print `SE_GENI_STATUS`/`M_CMD0` and are your best
instrument.

## Hard constraints learned the expensive way

* **FIFO mode does not work on this SE** — the engine accepts the command and never
  executes it, the driver retries forever and the console floods. Always keep `dmas`.
* **Never call `geni_load_se_firmware()` from the SPI probe** — it panics on SM4450.
  Mainline already calls it when the SE reports an invalid protocol; ours reports SPI.
* **Never read `/sys/kernel/debug/gpio`** — it hits a TZ-protected TLMM page and kills the
  box. `/sys/kernel/debug/pinctrl/*/pinmux-pins` is safe.
* **Always verify the kernel actually rebuilt** before packing: check
  `arch/arm64/boot/Image` mtime and `strings -a out/img | grep 'SMP PREEMPT'`. A failed
  build leaves the old Image behind and the packer will not tell you. The build errors are
  printed in Chinese ("错误 1"), so grep for `error:|Error [0-9]|错误`.
* Use `tools/mksky_dts.py` to change the DT (it generates `dts/sm4450-xiaomi-sky.dts`).
* Keep the code comments short and factual; the goal is upstreamable patches.

## Fallbacks

* `shim/out/sky-t21-touch-fw.img` — boots, chip detected, writes fail (`-EIO`).
* `shim/out/sky-m7f-safe-no-touch.img` — boots with the touch node disabled entirely.
* Recovery without fastboot: from TWRP `adb push` + `dd if=… of=/dev/block/by-name/boot_a
  bs=1M conv=fsync`, or from fastboot `fastboot flash boot_a …`.

## Definition of done

`fw_ver` non-zero, IRQ count grows while touching, `/dev/input/event*` reports
`ABS_MT_POSITION_X/Y` in 0..1079 / 0..2459, orientation corrected if needed
(`touchscreen-swapped-x-y`, `touchscreen-inverted-x/y`), then commit the DT + driver
patches, update `docs/m7-touch.md` and the README milestone row, and push to
`github.com/orange200213/sky-linux`.
