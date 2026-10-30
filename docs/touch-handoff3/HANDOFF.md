# Touch handoff #3 — the QUPv3 SE cannot *transmit* (SPI TX times out, RX works)

Everything below was measured on the device. This file is self-contained: read it, then
`NEXT-PROMPT.md`. (Older history lives in `docs/touch-handoff/` and `docs/touch-handoff2/`;
ignore them unless you need vendor reverse-engineering detail.)

---

## 1. TL;DR — what is broken and what is not

The Novatek **NT36672E** touch controller sits on QUPv3 **wrapper 1, SE1** (`spi@a84000`).
The SPI controller probes, the chip answers **reads**, the input device registers and the
touch IRQ fires — but **every SPI write times out (`-110`)**. Because the firmware download
is a write, the chip never leaves ROM mode (`fw_ver=0`), never scans, and no touch events
are produced.

Two completely different drivers fail in exactly the same way:

| driver | reads | writes |
|---|---|---|
| vendor import (`nt36xxx-spi/`) | OK: chip id `04 0C 00 00 72 66 03` | `CTP_SPI_WRITE: ret=-110` |
| mainline regmap port (`nt36xxx-ml/`) | OK: payload `0C 00 00 72 66 03` | `SPI transfer timed out` → `transfer err -110` |

**Therefore the bug is in our QUPv3 SE / GPI-DMA transmit path, not in either touch driver.**
That is the single thing the next session has to fix; the touch drivers are ready.

---

## 2. Measured facts (do not re-derive these)

| Item | Value |
|---|---|
| SoC / device | SM4450 / Xiaomi Redmi 12 5G (`sky`) |
| Touch IC | Novatek NT36672E, chip-id payload `0C 00 00 72 66 03` |
| SPI node | `spi@a84000`, `compatible = "qcom,geni-spi"`, IRQ GIC SPI 354 |
| Wrapper | `ac0000.geniqup` (mainline name `qupv3_id_0` = vendor "wrapper 1") |
| SE clocks | `GCC_QUPV3_WRAP1_S1_CLK` (19.2 MHz) as `"se"`, `GCC_QUPV3_WRAP1_CORE_CLK` as `"core"` |
| Wrapper clocks | `GCC_QUPV3_WRAP_1_M_AHB_CLK`, `GCC_QUPV3_WRAP_1_S_AHB_CLK` |
| Pins | gpio26 clk / 27 cs / 50 miso / 51 mosi, TLMM function `qup1_se1` |
| Touch GPIOs | reset gpio12 (active low), IRQ gpio13 |
| DMA | wrapper-1 GPI DMA at `0xa00000`, `#dma-cells = <3>`, `dma-channel-mask = <0x3e>` |
| DMA mapping | `dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>, <&gpi_dma1 1 1 QCOM_GPI_SPI>` (TX chid 0, RX chid 1) |
| Xfer mode | **GENI_GPI_DMA** (FIFO mode never executes on this SE, see §4) |
| IRQ behaviour | `a84000.spi` IRQ 354 fires; touch IRQ (gpio13) count grows once the chip scans |

---

## 3. The failure, verbatim

Vendor driver, firmware download stage:

```
[NVT-ts-spi] update_firmware_request 304: filename is novatek/nt36672c.bin
[NVT-ts-spi] CTP_SPI_WRITE 179: error, ret=-110
[NVT-ts-spi] nvt_write_sram 393: write to sram failed, ret = -5
[NVT-ts-spi] nvt_write_firmware 449: sram program failed, ret = -5
[NVT-ts-spi] nvt_download_firmware_hw_crc 666: Write_Firmware failed. (-5)
[NVT-ts-spi] nvt_read_bld_hw_crc 593: crc_done = 0, ilm_crc_flag = 0, dlm_crc_flag = 0
```

Mainline regmap driver, same stage (after the read fix described in §6):

```
nt36xxx-ml spi0.0: SPI transfer timed out
nt36xxx-ml spi0.0: transfer err -110
... (repeats)
nt36xxx-ml spi0.0: Firmware reset failed.
```

With the `sky-spi-xfer` / `sky-spi-irq` probes in `drivers/spi/spi-geni-qcom.c`, a failing
write looks like:

```
sky-spi-xfer: speed=1000000 cur=1000000 se_clk=19200000 status=00000000 m_cmd0=00000000
NVT-ts-spi spi0.0: SPI transfer timed out
sky-spi-irq:  m_irq=00000021 status=00000000 m_cmd0=18000000 s_irq=00000000
```

`status = 0` means the SE never started executing; `m_irq = 0x21` is our own cancel
(DONE|ABORT) after the timeout; `m_cmd0 = 0x18000000` is the command we wrote (never
consumed).

---

## 4. Hypotheses already ruled out (with the evidence)

| Hypothesis | Test | Result |
|---|---|---|
| Wrong wrapper clocks (vendor "wrapper 0 vs 1" naming) | use `WRAP_1_*` AHB clocks | required to boot at all; still fails |
| Missing AHB clock voting | add `m-ahb`/`s-ahb` on the wrapper | no change |
| Missing interconnect votes | add `qup-core`/`qup-config` ICC paths | no change, and it created a stuck supplier link |
| SE clock too slow | 19.2 MHz vs 9.6/2.4/1 MHz requests | no change; 2.4 MHz divides exactly and still fails |
| Wrong pinmux | single function `qup1_se1`, no sleep state | pins verified in `pinmux-pins`, still fails |
| DMA channel mask excludes TX | `dma-channel-mask = <0xfff>` | **identical failure** → mask is not the cause |
| TX on another channel | TX moved to `chid 2` | **worse**: even reads return zeros; chids must stay 0/1 |
| GPI DMA without IOMMU | removed `iommus`, added `dma-coherent` | `gpi a00000.dma-controller: Unhandled interrupt status:0x40`, box hangs |
| FIFO mode instead of DMA | removed `dmas` | SE never executes anything; driver retries forever, console flood |
| QUP firmware (`qupv3fw.elf`) | (a) `firmware-name` only, (b) call `geni_load_se_firmware()` from the probe | (a) no effect — mainline exports the loader but nothing calls it; (b) **panics** on this SoC, before and after `spi_geni_init()` |
| Driver bug | two independent drivers | both fail identically → **not a driver bug** |

Feature that *does* work: **reads** of any size we tried (7, 12, 64 bytes via the SPI core's
`spi_write_then_read()` path), including full-duplex messages where the RX length dominates.

---

## 5. The code that matters

`drivers/spi/spi-geni-qcom.c` — `setup_gsi_xfer()` (full excerpt in
`src/spi-geni-gsi-path.c`). Key points:

```c
	if (xfer->tx_buf && xfer->rx_buf)      peripheral.cmd = SPI_DUPLEX;
	else if (xfer->tx_buf) {               peripheral.cmd = SPI_TX;
	                                       peripheral.rx_len = 0; }
	else if (xfer->rx_buf)                 peripheral.cmd = SPI_RX;
	...
	if (peripheral.cmd & SPI_RX) {
		dmaengine_slave_config(mas->rx, &config);
		rx_desc = dmaengine_prep_slave_sg(mas->rx, xfer->rx_sg.sgl, ...);
	}
	/* TX is always prepared, per GSI spec */
	dmaengine_slave_config(mas->tx, &config);
	tx_desc = dmaengine_prep_slave_sg(mas->tx, xfer->tx_sg.sgl, ..., DMA_MEM_TO_DEV, flags);
	tx_desc->callback_result = spi_gsi_callback_result;
	if (peripheral.cmd & SPI_RX) dmaengine_submit(rx_desc);
	dmaengine_submit(tx_desc);
	if (peripheral.cmd & SPI_RX) dma_async_issue_pending(mas->rx);
	dma_async_issue_pending(mas->tx);
	return 1;
```

Observations to chase:

* For a pure RX transfer the TX descriptor is still submitted, but with an **empty SG list**
  (`xfer->tx_sg` has no buffers when `tx_buf` is NULL). That dummy TX works. For a real TX the
  same call carries real buffers — if `xfer->tx_sg` were malformed the driver would return
  `-EIO` from `dmaengine_prep_slave_sg()` immediately, and we see a *timeout* instead, so the
  descriptor is accepted but never consumed.
* `get_spi_clk_cfg()` calls `dev_pm_opp_set_rate()`; there is no OPP table for this device,
  yet reads work, so the clock path is fine.
* GPI DMA driver: `drivers/dma/qcom/gpi.c`, `gpi_of_dma_xlate()` takes exactly **3 cells**
  (`chid`, `seid`, `protocol`) — mainline's `QCOM_GPI_SPI` lives in
  `include/dt-bindings/dma/qcom-gpi.h`. The vendor's DT used a 5-cell private format.
* SE core: `drivers/soc/qcom/qcom-geni-se.c` — `geni_se_select_mode()` is a no-op for
  `GENI_GPI_DMA`; the vendor instead boots the SE in GSI mode through the QUP firmware.

---

## 6. Driver-side work already done (reusable, do not redo)

* `nt36xxx-ml/` — the sm7150-mainline driver with three fixes: our compatible
  (`novatek,nvt-ts-spi`) added to `of_match_table`; IRQ made optional (the TLMM refuses to
  map gpio13: `irq: type mismatch, failed to map hwirq-13 for pinctrl@f100000`, so the driver
  needs an `irq-gpios` property); chip-id read rewritten to the vendor sequence
  (**one full-duplex message of `val_size + 2` bytes, payload from `rx + 2`**) plus the
  chip-id compare fixed (`buf[i]`, and skip the `{ }` sentinel entry with `mapid == 0`).
  With those, the mainline driver identifies the chip and reaches the firmware step.
* `nt36xxx-spi/` — vendor driver, working reads, `BOOT_UPDATE_FIRMWARE` toggled during
  experiments, a failure-streak guard added in `nvt_ts_work_func()` (disables the IRQ after
  5 consecutive SPI failures so a bad experiment cannot flood the console).
* `sky-spi-xfer` / `sky-spi-irq` probes in `drivers/spi/spi-geni-qcom.c` — keep them.
* `qcom-geni-se.c` NULL guards (`if (!wrapper) return -EPROBE_DEFER;`, `se->core_clk`
  check) — without them the SE dereferences NULL at 0x30 / panics.

---

## 7. Ranked next experiments

**A. Instrument the TX descriptor path (highest value).**
Add prints (or better, use `trace_geni_spi_irq` + ftrace) around `setup_gsi_xfer()`:

* return value of `dmaengine_slave_config(mas->tx, &config)` — currently ignored;
* address/len/nents of the TX SG list (`xfer->tx_sg.sgl[0].addr/dma_address`, `sg_dma_len`);
* `peripheral.cmd`, `peripheral.rx_len`, `peripheral.clk_div`, `peripheral.fragmentation`;
* `SE_GENI_STATUS` / `SE_GENI_M_CMD0` **immediately after** `dma_async_issue_pending(mas->tx)`
  and again 1 ms later, to see whether the SE picked the command up;
* `readl(se->base + SE_DMA_TX_IRQ_STAT)` in the ISR (the probe already prints `m_irq` only).

Expected outcomes: (i) TX descriptor never issued → DMA/GPI problem; (ii) issued but
`M_CMD0` unchanged → GPI is not fetching, likely a protocol/seid/EE mismatch; (iii) `M_CMD0`
consumed but no completion → SE-side stall.

**B. Try the other protocol value for the TX GPI entry (DTB only, 2 minutes).**
`QCOM_GPI_SPI` is `2` in mainline; the vendor's 5-cell entry passed `1`. Renumber the `dmas`
entries (`<&gpi_dma1 0 1 1>` / `<&gpi_dma1 1 1 1>`) and see whether TX starts working.

**C. Compare against the vendor 5.10 QUP driver.**
The vendor runs the SE in **GSI DMA mode** with `qcom,enable-gsi-dma` + QUP firmware
(`qupv3fw.elf`, already in the initramfs at `lib/firmware/qcom/sm4450/xiaomi-sky/`). Dis-
assembling/porting their `msm_geni_serial`/`spi-geni` TX setup is the fallback if A and B
fail. Note that calling mainline's `geni_load_se_firmware()` panics here — if this path is
taken, first split the loader into "parse ELF" and "program PROG_RAM" to find the faulting
step.

**D. Ask whether TX needs `SE_GENI_CFG_SEQ_*`/GPII "EE" setup.**
The GPI driver has `qcom,ee` (execution environment) and channel-mask knobs; the vendor DT
may pin them. Compare `gpi_dma1` against the vendor's `dma-controller@a00000` node property
by property.

Cheap tests are B (DTB only) and A (needs a kernel rebuild — see the pipeline in §8 that
keeps the kernel bootable).

---

## 8. Environment, build and flash recipes

### Device access

```bash
sshpass -p sky ssh -o StrictHostKeyChecking=no root@192.168.7.1     # our kernel (USB-NCM, host 192.168.7.20/24)
fastboot devices                                                     # bootloader
adb devices                                                          # TWRP only
echo m912n67oo | sudo -S nmcli connection up sky-usb                 # bring the USB net up on the host
```

### The only kernel we flash: the T20 binary

Every kernel built from source in this session failed to reach the root filesystem, while
the **T20 kernel + any DTB** boots every time. Do not rebuild the kernel for experiments that
only need a DT change, and when you must rebuild, verify before flashing.

```bash
# extract the known-good kernel Image from the T20 boot image
python3 - <<'EOF'
import struct
d=open('shim/out/sky-t20-nt36xxx-real.img','rb').read()
for off in range(0, len(d)-4096, 4096):
    if d[off:off+8]==b'SWEETLNX':
        h=struct.unpack_from('<8sII7Q', d, off)
        open('/tmp/t20-Image.bin','wb').write(d[off+h[3]:off+h[3]+h[4]])
        print('Image', h[4], 'bytes'); break
EOF

# DTB + boot image
python3 tools/mksky_dts.py
KDIR=$PWD/kernel/min-tree ./tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts build
cd shim
python3 tools/mklinuxpayload.py /tmp/t20-Image.bin ../build/sm4450-xiaomi-sky.dtb out/payload.bin --load 0xc0000000
./build-sky.sh out/payload.bin          # -> out/sky-shim-boot-v4.img

# flash (fastboot)
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
# flash (TWRP, no fastboot)
adb push out/sky-shim-boot-v4.img /tmp/skyboot.img
adb shell "dd if=/tmp/skyboot.img of=/dev/block/by-name/boot_a bs=1M conv=fsync && sync"
adb reboot
```

If a kernel rebuild is unavoidable:

```bash
cd kernel/min-tree
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -j10 Image
# MANDATORY checks before packing - a failed build silently leaves the old Image:
grep -cE "error:|Error [0-9]|错误" ../build-kernel-*.log     # note the Chinese "错误 N"
ls -la arch/arm64/boot/Image                                  # mtime must be new
strings -a <packed image> | grep 'SMP PREEMPT'                 # banner must be new
```

### Driver iteration without flashing (use this!)

```bash
# build
cd kernel/min-tree && export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- && make -j10 modules

# load on the device (the vendor driver is built-in, so unbind it first)
scp drivers/input/touchscreen/nt36xxx-ml/nt36xxx-ml.ko root@192.168.7.1:/lib/modules/$(uname -r)/extra/
ssh root@192.168.7.1 'echo spi0.0 > /sys/bus/spi/drivers/NVT-ts-spi/unbind'
ssh root@192.168.7.1 'insmod /lib/modules/$(uname -r)/extra/nt36xxx-ml.ko'
ssh root@192.168.7.1 'dmesg | grep -E "nt36xxx-ml"'
ssh root@192.168.7.1 'rmmod nt36xxx_ml'                 # roll back in seconds
```

Module name is `nt36xxx_ml`, driver name `nt36xxx-ml`; it registers alongside the vendor
driver because its Kconfig entry builds `nt36xxx-ml.o` from `nt36xxx_core.o` +
`nt36xxx_spi.o` (`CONFIG_TOUCHSCREEN_NT36XXX_ML=m`).

### Image inventory

| Image | Content |
|---|---|
| `shim/out/sky-t20-nt36xxx-real.img` | **baseline kernel source of truth** (73 333 248 B Image) |
| `shim/out/sky-t32-t20kernel.img` | T20 kernel + `mask 0x3e`, 1 MHz — boots, vendor driver works for reads |
| `shim/out/sky-t33-mask-fff.img` | same kernel, `dma-channel-mask = <0xfff>` — boots, still fails |
| `shim/out/sky-m7f-safe-no-touch.img` | fallback with the touch node disabled |
| `shim/out/sky-t21-touch-fw.img` | older kernel (73 MB) with firmware download enabled |
| `shim/out/sky-t24-fifo-fw.img`, `sky-t22-dma-noiommu.img`, `sky-t27-*`, `sky-t30-*`, `sky-t31-*` | failed experiments, do not flash |

### Gotchas that cost real time

* `/sys/kernel/debug/gpio` **hangs the box** (TZ-protected TLMM page). Use
  `/sys/kernel/debug/pinctrl/*/pinmux-pins` instead.
* `pstore` records panics but **not** hangs; read it before booting TWRP
  (`adb pull /sys/fs/pstore/dmesg-ramoops-0.enc.z`, then `zlib.decompress(data, -15)`).
* A driver that keeps failing SPI will retry forever and flood the console until the box
  looks hung. The vendor driver now stops after 5 failures; keep that guard.
* `spi_write()` with a **stack** buffer of ≥ 8 bytes returns `-EINVAL` from
  `spi_map_buf()` — that is an artifact of the test, not the bug.

---

## 9. Definition of done

1. A write to the touch chip completes (`spi_write()` of 3 bytes and 4 KB both return 0).
2. `nvt_download_firmware_hw_crc` (vendor) or `nt36xxx_download_firmware` (mainline) succeeds
   and `fw_ver` is non-zero.
3. Touch IRQ count grows while a finger is on the screen and `/dev/input/event*` emits
   `ABS_MT_POSITION_X/Y` in 0..1079 / 0..2459 (add `touchscreen-swapped-x-y` /
   `touchscreen-inverted-x/y` if the orientation is wrong).
4. Pick one driver (the mainline regmap port is the better long-term choice), delete the
   other, update `docs/m7-touch.md` and the README milestone row, push to
   `github.com/orange200213/sky-linux`.
