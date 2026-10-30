# Touch handoff — Novatek SPI touchscreen on SM4450 (Redmi 12 5G / sky)

Status: **not working yet.** The first attempt panicked the kernel at boot and has been
reverted. Nothing else on the device is affected: display, battery/charging, USB and
SSH all work with the touch node disabled.

Last updated after: panic investigation from pstore, revert to
`status = "disabled"`, recovery image flashed to `boot_a` from TWRP.

---

## 1. Hardware, as described by the vendor device tree

Source: `vendor-touch-node.dts` in this folder, extracted from the vendor base DTB
(`sky.dts`, node `spi@a84000`).

| Item | Vendor value |
|---|---|
| Controller node | `spi@a84000` (QUPv3 wrapper 1, SE1), `compatible = "qcom,spi-geni"` |
| Controller reg | `<0xa84000 0x4000>` |
| Controller IRQ | `<0x00 0x162 0x04>` = **GIC SPI 354, level high** |
| Controller clocks | (from the wrapper) `qcom,wrapper-core` phandle, `dmas = <&gpi_dma1 ...>` |
| Controller pinctrl | `pinctrl-names = "default\0sleep"`, `pinctrl-0` = 4 phandles, `pinctrl-1` = 2 phandles |
| Bus frequency | `spi-max-frequency = <0x2faf080>` = **50 MHz** |
| Touch chip node | `novatek@0`, `compatible = "novatek,NVT-ts-spi\0focaltech,fts"` |
| Chip SPI frequency | `spi-max-frequency = <0x927c00>` = **9.6 MHz** |
| Reset | `novatek,reset-gpio = <&tlmm 0x0c ...>` = **GPIO 12** (and `focaltech,reset-gpio` the same) |
| Interrupt | `novatek,irq-gpio = <&tlmm 0x0d 0x2001>` = **GPIO 13**, `interrupts = <0x0d 0x02>` (falling) |
| Vendor-only props | `novatek,swrst-n8-addr = <0x3f0fe>`, `novatek,spi-rd-fast-addr = <0x3f310>`, `novatek,mp-support-dt` + MP criteria tables |
| Panel size for the driver | `focaltech,display-coords = <0 0 0x2a30 0x6018>` = 10800x24600 (in 1/10 mm or similar), touchscreen 1080x2460 |
| Aliases in the vendor DT | `qupv3_se6_spi = "/soc/spi@a84000"` (SE6 naming downstream, SE1 of wrapper 1 in mainline numbering) |

So: `RESET = TLMM 12`, `IRQ = TLMM 13`, SPI1 SE1 at `0xa84000`, IRQ line 354,
9.6 MHz chip clock, 4 data/clock pins plus CS, and a separate sleep pin state.

## 2. What our DTB has today

`tools/mksky_dts.py` emits a standalone node under `&soc` (not inside a wrapper):

```dts
&soc {
	spi_touch: spi@a84000 {
		compatible = "qcom,geni-spi";
		reg = <0x0 0x00a84000 0x0 0x4000>;
		clocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>;
		clock-names = "se";
		interrupts = <GIC_SPI 602 IRQ_TYPE_LEVEL_HIGH>;      /* WRONG: should be 354 */
		pinctrl-0 = <&qup_spi1_default>;                    /* WRONG: gpio8..11 invalid on SM4450 */
		pinctrl-names = "default";
		#address-cells = <1>;
		#size-cells = <0>;
		status = "disabled"; /* needs a qupv3 wrapper node: panic in geni_se_clks_on() otherwise */

		touchscreen@0 {
			compatible = "novatek,nvt-ts-spi";
			reg = <0>;
			spi-max-frequency = <9600000>;
			interrupt-parent = <&tlmm>;
			interrupts = <13 IRQ_TYPE_EDGE_FALLING>;
			reset-gpios = <&tlmm 12 GPIO_ACTIVE_LOW>;
			touchscreen-size-x = <1080>;
			touchscreen-size-y = <2460>;
		};
	};
};
```

The node must stay disabled until the wrapper exists; see section 4.

## 3. Attempt 1 and the panic (full log in `logs/`)

Enabling `status = "okay"` with `compatible = "novatek,nt36672a-ts"`, with
`CONFIG_TOUCHSCREEN_NOVATEK_NVT_TS=y`, produced this at 0.29 s:

```
sm4450-tlmm f100000.pinctrl: invalid group "gpio8" for function "qup1_se1"
sm4450-tlmm f100000.pinctrl: invalid group "gpio9" for function "qup1_se1"
sm4450-tlmm f100000.pinctrl: invalid group "gpio10" for function "qup1_se1"
sm4450-tlmm f100000.pinctrl: invalid group "gpio11" for function "qup1_se1"

Unable to handle kernel NULL pointer dereference at virtual address 0x30
Internal error: Oops: 0000000096000004 [#1]  SMP
pc : geni_se_clks_on+0x24/0x140
lr : geni_se_resources_activate+0x54/0x94
Call trace:
 geni_se_clks_on+0x24/0x140 (P)
 geni_se_resources_activate+0x54/0x94
 spi_geni_runtime_resume+0x2c/0x68
 pm_generic_runtime_resume+0x2c/0x44
 __rpm_callback / rpm_resume / __pm_runtime_resume
 spi_geni_probe+0x260/0x77c
 platform_probe / really_probe / __driver_probe_device / deferred_probe_work_func
```

Three independent defects, all in the DTB:

1. **No QUPv3 wrapper parent.** `drivers/soc/qcom/qcom-geni-se.c` expects the SE node
   to be a child of a `qcom,geni-se-qup` device that supplies the `core` clock and the
   interconnect path (`se->core_clk`, `geni_se_resources_activate()`). Standalone, the
   clock lookup comes back empty and `geni_se_clks_on()` dereferences NULL at 0x30.
2. **Wrong interrupt.** GIC SPI 602 was invented; the vendor says 354.
3. **Wrong pins.** The TLMM driver in our tree has no `qup1_se1` function for
   gpio8..11; the touch bus pins for SM4450 have to be taken from the vendor pinctrl
   nodes (the 4 default + 2 sleep phandles in `vendor-touch-node.dts`).

Recovery: the panic left the device bootlooping, so the safe image
(`shim/out/sky-m7f-safe-no-touch.img`, sha256
`b268944d431568a8e484d535a18fbe774367841a9d2d8811d1d37ae2dc8dbcd7`) was written from
TWRP straight into `boot_a` and verified by md5 read-back. Both
`logs/pstore-ramoops-0.txt` (dmesg-ramoops record, raw-deflate decoded) and
`logs/panic-extract.txt` are in this folder.

## 4. Important: the mainline Novatek driver is I2C-only

`drivers/input/touchscreen/novatek-nvt-ts.c` is an **i2c driver**:

```c
static const struct nvt_ts_i2c_chip_data nvt_nt11205_ts_data = { .chip_id = 0x05 };
static const struct nvt_ts_i2c_chip_data nvt_nt36672a_ts_data = { .chip_id = 0x08 };
static const struct of_device_id nvt_ts_of_match[] = {
	{ .compatible = "novatek,nt11205-ts", .data = &nvt_nt11205_ts_data },
	{ .compatible = "novatek,nt36672a-ts", .data = &nvt_nt36672a_ts_data },
	{ }
};
...
static struct i2c_driver nvt_ts_driver = { ... };
```

It cannot bind to an SPI device, and it validates the chip id it reads back, so even on
I2C our panel's chip would have to match 0x05 or 0x08. **Our touch is on SPI**, so a
driver has to be written (or a SPI variant added). That is the main work item, not the
DTB.

Useful facts for writing it:

* the driver's probe sequence is a good template: reset the chip, `msleep(100)`, read
  the 12-byte parameter block, parse `width/height/max_touches/irq_type/chip_id`,
  then register an input device with `MT_B` slots;
* the vendor DT gives the two magic addresses needed for SPI reads on this chip:
  `novatek,swrst-n8-addr = <0x3f0fe>` (software reset) and
  `novatek,spi-rd-fast-addr = <0x3f310>` (fast read);
* Novatek page protocol (also visible in the panel init sequence of this platform):
  write `0xFF <page>` to select a page, `0xFB 0x01` to unlock, then the register.

## 5. Plan for the next session

Step 1 — QUPv3 wrapper (prerequisite, in `tools/mksky_dts.py`):

```dts
qupv3_id_1: geniqup@ac0000 {
	compatible = "qcom,geni-se-qup";
	reg = <0 0x00ac0000 0 0x2000>;
	ranges;
	clock-names = "m-ahb", "s-ahb";
	clocks = <&gcc GCC_QUPV3_WRAP_1_M_AHB_CLK>,
	         <&gcc GCC_QUPV3_WRAP_1_S_AHB_CLK>;
	#address-cells = <2>;
	#size-cells = <2>;
	status = "okay";

	spi_touch: spi@a84000 {
		compatible = "qcom,geni-spi";
		reg = <0 0x00a84000 0 0x4000>;
		clock-names = "se";
		clocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>;
		interrupts = <GIC_SPI 354 IRQ_TYPE_LEVEL_HIGH>;
		pinctrl-names = "default", "sleep";
		pinctrl-0 = <&qup_spi1_data_clk>, <&qup_spi1_cs>;
		pinctrl-1 = <&qup_spi1_sleep>;
		status = "okay";
		...
	};
};
```

Reference shape (from `arch/arm64/boot/dts/qcom/sm8550.dtsi`, wrapper
`geniqup@ac0000` + `spi1: spi@a84000`): wrapper needs `m-ahb`/`s-ahb`, `ranges`,
`#address-cells = <2>`, `#size-cells = <2>`; the SE node needs `se` clock,
`interrupts = <GIC_SPI 354 IRQ_TYPE_LEVEL_HIGH 0>`, and for full functionality
`interconnects`/`interconnect-names = "qup-core", "qup-config", "qup-memory"` plus
`power-domains = <&rpmhpd RPMHPD_CX>`. SM4450 may not need all of them; add them one
at a time and watch `dmesg` for `geni-se` errors.

Step 2 — pinctrl, copied from the vendor DTB (4 default + 2 sleep phandles). Find the
real SM4450 pin numbers by decoding those phandles in `sky.dts` (`&tlmm` node,
`qcom,pins`/`pins = "gpioNN"` and `function = "qup1_se1"`), then mirror them into our
DTS with the SM4450 function names the TLMM driver actually accepts
(`grep -o 'qup1_se[0-9]' drivers/pinctrl/qcom/pinctrl-sm4450.c | sort -u`).

Step 3 — driver. Two options:

* add SPI support to `novatek-nvt-ts.c` (rename to a shared core, add `spi_driver`
  with `compatible = "novatek,nvt-ts-spi"`, `spi_sync` transfers, plus a new chip id
  for our panel);
* write a small dedicated SPI driver (`panel`-style) implementing reset, parameter
  read, and the report loop, using the vendor magic addresses above.

Either way, first get a chip id: after the wrapper works, the probe will read the
parameter block; log it raw (`%*ph`) and decode.

Step 4 — supplies. The mainline driver does `regulator_bulk_get` on `vcc` and `iovcc`.
Our touch node has neither; add them once the rails are identified (the vendor node
relies on always-on rails, so `regulator-always-on` fixed regulators are acceptable for
bring-up if the real ones are not documented).

## 6. Verification targets

| Check | Command | Expected |
|---|---|---|
| SPI controller probed | `dmesg \| grep -i geni` | `spi_geni` probe success, no NULL deref |
| Touch device | `cat /proc/bus/input/devices` | `Name="novatek-nvt-ts"`, `Handlers=event*` |
| Interrupts | `grep -i "tlmm.*13\|nvt" /proc/interrupts` | count increases when touching |
| Events | `od -An -tu1 -N24 /dev/input/eventN` | values change on touch |
| No regressions | `cat /proc/fb`, `/sys/class/power_supply/battery/capacity` | display and battery still fine |

## 7. Tooling reminders

Build + flash loop (panel/other modules are on the UFS rootfs; kernel/DTB need a flash):

```bash
cd ~/redmi125g/sky-mainline/kernel/min-tree
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -j10 Image
cd ../../shim
python3 tools/mklinuxpayload.py ../kernel/min-tree/arch/arm64/boot/Image \
        ../build/sm4450-xiaomi-sky.dtb out/payload.bin --load 0xc0000000
./build-sky.sh out/payload.bin
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
```

If the device boots to a panic instead of Linux: `adb` works in TWRP, so the image can
be written without fastboot:

```bash
adb push out/sky-shim-boot-v4.img /tmp/skyboot.img
adb shell "test -b /dev/block/by-name/boot_a && dd if=/tmp/skyboot.img of=/dev/block/by-name/boot_a bs=1M conv=fsync && sync"
adb reboot
```

Reading a panic afterwards: `adb pull /sys/fs/pstore/dmesg-ramoops-0.enc.z` and decode
with raw deflate (`zlib.decompress(data, -15)`); see `logs/`.

## 8. File inventory

| File | Contents |
|---|---|
| `HANDOFF-TOUCH.md` | this document |
| `NEXT-PROMPT-TOUCH.md` | prompt to paste into a fresh session |
| `vendor-touch-node.dts` | vendor `spi@a84000` + `novatek@0` node from the vendor base DTB |
| `logs/pstore-ramoops-0.txt` | full decoded pstore dmesg record of the panic boot |
| `logs/panic-extract.txt` | just the oops and call trace |

Related material: `docs/m7-touch.md` (public summary), `docs/handoff/HANDOFF.md`
(display), `docs/m6-power.md` (power), `docs/partitions-sky.txt` (GPT names, useful if
firmware for the touch controller ever turns out to live in a vendor partition).

---

# Update: attempt 2 — the SPI controller works, the SE does not execute commands

Everything below was measured on the device, not inferred.

## What now works

The SPI controller probes and registers:

```
geni_spi a84000.spi: error -ENODEV: Failed to get tx DMA ch
geni_spi a84000.spi: FIFO mode disabled, but couldn't get DMA, fall back to FIFO mode
/sys/class/spi_master/spi0
```

Verified configuration (see section "verified DTS recipe" below): wrapper 1 AHB clocks,
SE clock, QUP core clock, interrupt 354, pin mux.

## What still fails

```
sky-spi-xfer: speed=9600000 cur=9600000 se_clk=19200000 status=00000000 m_cmd0=18000000
novatek-nvt-ts-spi spi0.0: SPI transfer timed out          (-110, 200 ms)
sky-spi-irq:  m_irq=00000021 status=00000000 m_cmd0=18000000 s_irq=00000000
```

Decoded:

| Field | Value | Meaning |
|---|---|---|
| `se_clk` | 19200000 | SE clock is running |
| `status` (`SE_GENI_STATUS`) | 0 | the SE never enters the "command executing" state |
| `m_irq` | `0x21` = `M_CMD_DONE_EN \| M_CMD_ABORT_EN` | produced by our own cancel after the timeout |
| error bits (OVERRUN / ILLEGAL_CMD / FAILURE) | not set | the command is neither rejected nor executed |

So the SE accepts the command into `SE_GENI_M_CMD0` and then does nothing.

## Verified DTS recipe (all of it measured)

```dts
&qupv3_id_0 {                       /* = geniqup@ac0000, the vendor's "wrapper 1" */
	status = "okay";
	clocks = <&gcc GCC_QUPV3_WRAP_1_M_AHB_CLK>,
	         <&gcc GCC_QUPV3_WRAP_1_S_AHB_CLK>;   /* the dtsi wrongly uses WRAP_0 */
	clock-names = "m-ahb", "s-ahb";

	spi_touch: spi@a84000 {
		compatible = "qcom,geni-spi";
		reg = <0 0x00a84000 0 0x4000>;
		clocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>,
		         <&gcc GCC_QUPV3_WRAP1_CORE_CLK>;
		clock-names = "se", "core";
		interrupts = <GIC_SPI 354 IRQ_TYPE_LEVEL_HIGH>;
		interconnects = <&clk_virt MASTER_QUP_CORE_1 QCOM_ICC_TAG_ALWAYS
				 &clk_virt SLAVE_QUP_CORE_1 QCOM_ICC_TAG_ALWAYS>,
				<&gem_noc MASTER_APPSS_PROC QCOM_ICC_TAG_ALWAYS
				 &config_noc SLAVE_QUP_1 QCOM_ICC_TAG_ALWAYS>;
		interconnect-names = "qup-core", "qup-config";
		pinctrl-names = "default";     /* a "sleep" state muxes the pins to GPIO */
		pinctrl-0 = <&qup_spi1_default>;
		#address-cells = <1>;
		#size-cells = <0>;

		touchscreen@0 { compatible = "novatek,nvt-ts-spi"; reg = <0>; ... };
	};
};
```

Pins: gpio26 (clk), gpio27 (cs), gpio50 (miso), gpio51 (mosi), all with function
`qup1_se1` — confirmed applied:

```
pin 26 (GPIO_26): device a84000.spi function qup1_se1 group gpio26
pin 27 (GPIO_27): device a84000.spi function qup1_se1 group gpio27
pin 50 (GPIO_50): device a84000.spi function qup1_se1 group gpio50
pin 51 (GPIO_51): device a84000.spi function qup1_se1 group gpio51
```

Clock summary after the fix: `gcc_qupv3_wrap_1_{m,s}_ahb_clk` are consumed by
`ac0000.geniqup`, `gcc_qupv3_wrap1_core_clk` by `a84000.spi core`,
`gcc_qupv3_wrap1_s1_clk` by `a84000.spi se`.

## Kernel patches that are needed regardless (keep them)

```c
/* drivers/soc/qcom/qcom-geni-se.c */
	if (!wrapper)
		return -EPROBE_DEFER;            /* was a NULL deref at 0x30 */

	if (se->core_clk) {                      /* "core" is optional in DT */
		ret = clk_prepare_enable(se->core_clk);
		...
	}
```

## Ranked hypotheses for "SE accepts command, never executes"

1. **QUP firmware (`qupv3fw.elf`) for wrapper 1.** The stock fastboot images contain
   `qupv3fw.elf`; Android loads it into the QUP core. Wrapper 0 clearly runs (i2c1
   drives the KTD3136 backlight), so the *core* firmware is up, but wrapper 1 has never
   been exercised by anything in our tree. Try: boot the stock Android boot image once
   (or stock recovery) with touch working, then immediately kexec/reboot into our kernel
   and retry — if the SE then works, the bitstream is initialised by the stock loader.
2. **The SE is left in I2C protocol by the bootloader.** The vendor DT describes both
   `i2c@a84000` and `spi@a84000` on the same SE. Test: add an `i2c@a84000` node, scan the
   bus (expect a NAK but check whether transfers *complete*). If I2C on this SE also
   hangs, the SE itself is unconfigured and hypothesis 1 wins; if I2C completes, the
   protocol switch is the problem.
3. **`qcom,disable-dma` / GPI differences.** The driver falls back to FIFO mode; adding
   the property explicitly changes the code path (`geni_se_select_mode` ordering).
4. **Cross-check with `uart7`** (same wrapper, SE2, `GCC_QUPV3_WRAP1_S2_CLK`, IRQ 355):
   enable it with its own pinctrl and send a byte. If UART7 also hangs, the wrapper is
   not usable and hypothesis 1 is almost certainly right; if UART7 works, the problem is
   SPI-specific on SE1.

## Practical notes for the next session

* The debug probes live in `drivers/spi/spi-geni-qcom.c` (`sky-spi-xfer`, `sky-spi-irq`)
  and print `SE_GENI_STATUS`, `M_CMD0`, `S_IRQ` — keep them until touch works.
* Never put the SE node outside a wrapper and never use the WRAP_0 clocks for
  `ac0000`: both cause immediate hangs (bus hang, no oops, no pstore record).
* A hang leaves **no pstore record**: the screen log freezes instead. Photograph the
  screen, or boot a known-good image from fastboot and read `/sys/fs/pstore` before
  TWRP overwrites it.
