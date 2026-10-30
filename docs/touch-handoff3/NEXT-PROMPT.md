# Next-session prompt — fix the QUPv3 SE transmit path (copy this as the opening message)

I am bringing up mainline Linux 7.3 on the Xiaomi Redmi 12 5G (`sky`, Qualcomm SM4450).
Working directory `/home/orange/redmi125g/`. Device access: `sshpass -p sky ssh
root@192.168.7.1` (USB-NCM) when my kernel is booted, `fastboot` in the bootloader, `adb` in
TWRP. Host sudo password: `m912n67oo`. Display, battery/charging, USB gadget and SSH all
work. **Touch is the last milestone and it is blocked on one concrete bug.**

Read `docs/touch-handoff3/HANDOFF.md` first — it is self-contained. Then work the problem
below; do not start by re-reading the older `docs/touch-handoff*/` folders.

## The bug in one line

The Novatek NT36672E on `spi@a84000` (QUPv3 wrapper 1, SE1) **reads fine but never
completes a write** — every SPI write times out (`-110`), so the touch firmware cannot be
downloaded and the chip never leaves ROM mode (`fw_ver = 0`).

```
sky-spi-xfer: speed=1000000 cur=1000000 se_clk=19200000 status=00000000 m_cmd0=00000000
NVT-ts-spi spi0.0: SPI transfer timed out
sky-spi-irq:  m_irq=00000021 status=00000000 m_cmd0=18000000 s_irq=00000000
```

`status = 0` → the SE never started executing; `m_irq = 0x21` is our own cancel; `m_cmd0`
still holds the command we wrote.

## Why I am confident it is the controller, not the driver

Two unrelated drivers fail identically at the same place:

* vendor import (`drivers/input/touchscreen/nt36xxx-spi/`): reads OK, `CTP_SPI_WRITE: -110`;
* mainline regmap port (`drivers/input/touchscreen/nt36xxx-ml/`, already fixed up so it
  identifies the chip): reads OK, `SPI transfer timed out` → `Firmware reset failed`.

## Start here, in this order

1. **DTB-only, 2 minutes (highest value per minute):** try the alternative GPI protocol
   value for the TX/RX entries — `dmas = <&gpi_dma1 0 1 1>, <&gpi_dma1 1 1 1>` instead of
   `QCOM_GPI_SPI` (= 2). The vendor DT passes 1. Rebuild only the DTB and use the
   "T20 kernel + new DTB" pipeline in the handoff (§8) — never a freshly built kernel.
2. **Instrument the TX path** in `drivers/spi/spi-geni-qcom.c` `setup_gsi_xfer()`: print the
   return of `dmaengine_slave_config(mas->tx, ...)`, the TX SG list (`dma_address`,
   `sg_dma_len`, `nents`), `peripheral.cmd/rx_len/clk_div/fragmentation`, and
   `SE_GENI_STATUS` + `M_CMD0` right after `dma_async_issue_pending(mas->tx)` and 1 ms later.
   Add `SE_DMA_TX_IRQ_STAT` to the ISR probe. Then classify: (i) descriptor never issued,
   (ii) issued but `M_CMD0` unchanged (GPI not fetching — likely seid/protocol/EE mismatch),
   (iii) `M_CMD0` consumed but no completion (SE-side stall).
   If you rebuild the kernel, verify it before flashing (mtime + version banner; a failed
   build silently ships the old Image — the build errors are printed in Chinese, "错误 1").
3. **Only if 1–2 fail:** compare against the vendor 5.10 QUP, which runs the SE in GSI DMA
   mode via `qcom,enable-gsi-dma` + the QUP firmware `qupv3fw.elf` (already in the initramfs
   at `lib/firmware/qcom/sm4450/xiaomi-sky/`). Warning: calling mainline's
   `geni_load_se_firmware()` from the probe **panics** on this SoC — split the loader into
   "parse ELF" and "program PROG_RAM" before trying again.

## Hard-won constraints (violating these wastes hours)

* **FIFO mode does not work on this SE** — keep `dmas`; without GPI DMA the engine accepts
  commands and never executes them, and the driver retries forever.
* **Do not change the GPI channel ids** — TX must stay `chid 0`, RX `chid 1`. Moving TX to
  `chid 2` breaks even the reads.
* **The DMA channel mask is irrelevant** — `0x3e` and `0xfff` behave identically.
* **Do not add `iommus`-less DMA / `dma-coherent` hacks** — that produced
  `gpi a00000.dma-controller: Unhandled interrupt status:0x40` and a hang.
* **Never read `/sys/kernel/debug/gpio`** — it hangs the box (TZ-protected TLMM page).
* **Iterate with modules, not flashes.** The touch driver builds as `nt36xxx-ml.ko`, and the
  vendor driver can be released with
  `echo spi0.0 > /sys/bus/spi/drivers/NVT-ts-spi/unbind`. A full cycle is ~20 s.
* Keep `spi_write()` test buffers `kmalloc()`ed, not on the stack — stack buffers return
  `-EINVAL` from `spi_map_buf()` and look like a bug that isn't there.

## Definition of done

A 3-byte and a 4 KB `spi_write()` both return 0; the touch firmware downloads (`fw_ver`
non-zero); the touch IRQ count grows while touching and `/dev/input/event*` reports
`ABS_MT_POSITION_X/Y` in 0..1079 / 0..2459. Then keep the mainline regmap driver, delete the
vendor one, fix the orientation if needed (`touchscreen-swapped-x-y`,
`touchscreen-inverted-x/y`), update `docs/m7-touch.md` + README, and push to
`github.com/orange200213/sky-linux`.
