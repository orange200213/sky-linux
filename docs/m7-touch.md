# M7 — touchscreen (Novatek NT36672C, Tianma M19 module, over QUPv3 SPI)

**Status: 🚧 partial.** Touch reports real multi-touch events on `/dev/input/event3`, survives
long runs, and recovers from a wedged controller without a reboot — but read the scope of
what has actually been validated:

| What was verified | How | Caveat |
|---|---|---|
| the controller works end to end | raw events read with `dd if=/dev/input/event3 bs=24 … \| od -Ax -tx1` and `tools/decode_evdev.py` | **never yet exercised through a desktop / window manager** (no X, no Wayland, no Phosh/weston/libinput session has been run) |
| panel `m19` (Tianma M19 module, 1080×2460 IPS LCD) only | this unit | no other panel or regional variant has been tried; coordinates and the panel-follower link are specific to this panel |
| **45-minute continuous run** without a second firmware reset | `docs/handoff-touch-driver-20261104/evidence/boot-and-stability.log` | one recovery during the first initialisation is normal (see below) |
| display off → on re-initialises the IC | `echo 4 > /sys/class/graphics/fb0/blank; sleep 5; echo 0 > …` then a fresh `Touch IC fw loaded ok` | works on hardware; still new code |
| Japanese-market Redmi 12 5G (`sky`), stock `V14.0.7.0.TMWINXM` | whole project | other variants of the same model may use a different touch controller/panel |
| `sky-report` shows the IC in `NORMAL_RUN` (`state=a3…`), IRQ ≈ 12/s idle | `dmesg`, `/proc/interrupts` | — |

**Known risk: touch can still drop.** Two distinct causes were found and fixed in the
2026-11-04 driver: a **wrong firmware image** (FW_VER `0x12`, which resets after ~9.5 min;
the correct vendor image is FW_VER `0x18`) and the missing **DRM panel-follower link**, which
left the touch power state machine dead after a screen-on. Both are fixed and a 45-minute run
showed no second reset, but the driver still has to recover occasionally on the first
initialisation and the long-term behaviour (hours, many suspend/resume cycles) is not proven.
Treat a long session as "usually fine, it may heal itself once, and it has not been proven
under a desktop yet".

Because of the two points above this milestone is marked **partial**, not done.

Reference image: `shim/out/sky-shim-boot-v4-touch-usbdma4g.img`
(sha256 `006d8d0b440f6b9e2f88c83a014eaaa036b42a7bac659cbc1d5f3c607b5f9180`), DTB
`sm4450-xiaomi-sky-usbdma4g.dtb` (`b677968bbf2f9ac1…`), built with the driver in
`kernel/drivers/touchscreen/nt36xxx/` (core `9a8449eaa84b28ee…`, 2116 lines).
See `docs/handoff-20261106.md` for that session. Previous reference:
`shim/out/sky-shim-boot-v4-dt-final.img` (`831227eb…`, driver core `62bf354e0f0b7717…`,
2098 lines).
The handoff package with the technical write-up, the evidence logs and the DTS node is
`docs/handoff-touch-driver-20261104/`. Older references: the runtime-recovery build
`sky-shim-boot-v4-recover-hwreset.img`
(`8ed569ca617688e8037a48ff0b4e54531bda3c55e0e7b731fd33b1b571c56bac`) and the boot-time
fix `sky-shim-boot-v4-touch-ok.img` (`a16ce9e9…`).

## Hardware

| Item | Value |
|---|---|
| Touch IC | Novatek **NT36672C** in a **Tianma M19** module (trim `0C 00 00 72 66 03` → NT36675 register map, `hw_crc=2`) |
| Bus | QUPv3 **wrapper 1, SE1** at `0xa84000`, `qcom,geni-spi` |
| SPI interrupt | GIC SPI **354** (level high) |
| Touch interrupt | TLMM **gpio13**, active low (`irq-gpios`) |
| Reset | **SPI soft reset only** — as of the 2026-11-06 driver GPIO12 is *not* requested at all (`ts->reset_gpio = NULL`), matching the vendor driver, which also leaves the pin unclaimed |
| Pins | gpio26 SCLK, gpio27 CS, gpio50 MISO, gpio51 MOSI, function `qup1_se1` |
| SE clocks | `GCC_QUPV3_WRAP1_S1_CLK` (19.2 MHz), `GCC_QUPV3_WRAP1_CORE_CLK` |
| Wrapper clocks | `GCC_QUPV3_WRAP_1_M_AHB_CLK`, `GCC_QUPV3_WRAP_1_S_AHB_CLK` |
| DMA | wrapper-1 GPI DMA at `0xa00000` (`qcom,sm4450-gpi-dma`), `#dma-cells = <3>` |
| Bus clock | 4 MHz (`spi-max-frequency = <4000000>`) |
| Firmware | `novatek/nt36672c.bin`, **139,264 B, FW_VER `0x18`** (sha256 `87dba28ecbc7e81f…`), from the phone's `/vendor/firmware/novatek_ts_fw.bin` — a `0x12` image from another tree also passes the driver's `FW_VER+VER_BAR==0xFF` check but resets after ~9.5 min |
| Input device | `Novatek NT36XXX Touchscreen`, `/dev/input/event3`, abs 1080×2460 |

## The two defects that made this take many rounds

### 1. FIFO mode does not execute on this SE — GSI DMA is mandatory

With no `dmas` property the engine accepted the command and never ran it (`SE_GENI_STATUS`
stayed 0, `M_CMD0` kept our opcode), the driver retried forever and flooded the console.
The DT therefore forces GPI DMA:

```dts
qcom,enable-gsi-dma;
dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>, <&gpi_dma1 1 1 QCOM_GPI_SPI>;
dma-names = "tx", "rx";
```

The GPI channel ids are load-bearing: TX **chid 0**, RX **chid 1**. Moving TX to chid 2
breaks even the reads.

### 2. The GENI→DDR path needs a bandwidth vote ("qup-memory")

Without the third interconnect path the SE underruns: writes still *complete*, but the data
on the wire is stale and the SE can wedge in `M_GENI_CMD_ACTIVE` (only a reboot recovered
it). The fix is the same three-path ICC triple that `i2c8@a8c000` (SE3 of the same wrapper)
uses:

```dts
interconnects = <&clk_virt MASTER_QUP_CORE_1 QCOM_ICC_TAG_ALWAYS
                 &clk_virt SLAVE_QUP_CORE_1 QCOM_ICC_TAG_ALWAYS>,
                <&gem_noc MASTER_APPSS_PROC QCOM_ICC_TAG_ALWAYS
                 &config_noc SLAVE_QUP_1 QCOM_ICC_TAG_ALWAYS>,
                <&aggre2_noc MASTER_QUP_1 QCOM_ICC_TAG_ALWAYS
                 &mc_virt SLAVE_EBI1 QCOM_ICC_TAG_ALWAYS>;
interconnect-names = "qup-core", "qup-config", "qup-memory";
```

Earlier attempts with only `qup-core`/`qup-config` looked identical at the transfer level and
were wrongly discarded — the symptom only shows up as stale data or wedging, not as a
transfer error.

## Final device tree

```dts
spi_touch: spi@a84000 {
	compatible = "qcom,geni-spi", "qcom,spi-geni";
	reg = <0x0 0x00a84000 0x0 0x4000>;
	reg-names = "se_phys";
	clocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>, <&gcc GCC_QUPV3_WRAP1_CORE_CLK>;
	clock-names = "se", "core";
	qcom,enable-gsi-dma;
	interrupts = <GIC_SPI 354 IRQ_TYPE_LEVEL_HIGH>;
	interconnects = ...;			/* the three paths above */
	interconnect-names = "qup-core", "qup-config", "qup-memory";
	dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>, <&gpi_dma1 1 1 QCOM_GPI_SPI>;
	dma-names = "tx", "rx";
	pinctrl-names = "default", "sleep";
	pinctrl-0 = <&qup_spi1_default>;
	pinctrl-1 = <&qup_spi1_sleep>;
	#address-cells = <1>;
	#size-cells = <0>;
	status = "okay";

	touchscreen@0 {
		compatible = "novatek,nvt-ts-spi";
		reg = <0>;
		status = "okay";
		firmware-name = "novatek/nt36672c.bin";
		spi-max-frequency = <4000000>;
		novatek,swrst-n8-addr = <0x3f0fe>;
		novatek,spi-rd-fast-addr = <0x3f310>;
		irq-gpios = <&tlmm 13 GPIO_ACTIVE_LOW>;
		reset-gpios = <&tlmm 12 GPIO_ACTIVE_LOW>;
		touchscreen-size-x = <1080>;
		touchscreen-size-y = <2460>;
	};
};
```

`qup_spi1_default` muxes gpio26/27/50/51 to `qup1_se1`; `qup_spi1_sleep` returns them to
`gpio` with pull-down so the pads stop driving while the panel is off.

## Driver

The driver started as the **mainline-oriented regmap port** from `sm7150-mainline/linux`
(`nt36xxx_core.c` + `nt36xxx_spi.c` + `nt36xxx.h`, ~1230 lines) and was extended twice; the
current version is the **2026-11-04** one (`nt36xxx_core.c`, 2098 lines). The files live at
`kernel/drivers/touchscreen/nt36xxx/` and the validated copies are also in
`docs/handoff-touch-driver-20261104/` (sha256 in `SHA256SUMS`). Kconfig symbol:
`CONFIG_TOUCHSCREEN_NT36XXX_ML`.

Changes on top of upstream, in the order they were needed:

1. our `compatible = "novatek,nvt-ts-spi"` added to `of_match_table`;
2. **read path** rewritten to the vendor sequence: one full-duplex SPI message of
   `val_size + 2` bytes, payload copied from `rx + 2` (upstream used `spi_write_then_read()`
   with no dummy bytes and read zeros);
3. **chip-id compare** fixed: `buf[i]` instead of `buf[i + 1]`, and the empty sentinel table
   entry (`mapid == 0`, which matches everything) is skipped;
4. IRQ made optional at probe so a refused GPIO mapping is not fatal (`irq-gpios` is the
   property that actually works on this board);
5. firmware download / verification helpers: `nt36xxx_download_firmware_hw_crc()`,
   `nt36xxx_sky_verify()`, `nt36xxx_sky_dump_crc()`, `nt36xxx_sky_write_part()`,
   `nt36xxx_sky_bad_blocks()`, plus the diagnostic `nt36xxx_tx_size_sweep()`;
6. reporting and scaling: `nt36xxx_wait_first_report()`, `nt36xxx_report()`,
   `nt36xxx_scale_coord()`, `sky-report[0..2]` diagnostics;
7. runtime recovery: `nt36xxx_health_work()`, `nt36xxx_sky_schedule_recover()`,
   `nt36xxx_hw_reset()` and the `ts->lock` serialisation
   (`kernel/patches/0004-nt36xxx-lock-and-hw-reset-recovery.patch`);
8. DRM panel-follower hooks (`panel_prepared` / `panel_unpreparing` / enabled / disabling) so
   the touch controller follows the panel: it is re-initialised on wake and told to sleep
   (`ENTER_SLEEP` to `EVENT_BUF_ADDR | 0x50`) on suspend. **This requires `panel = <&panel>;`
   in the touch node** — `drm_is_panel_follower()` only looks for that phandle, and without
   it the whole display power state machine is dead code (screen-on then never re-initialises
   the IC).

Behaviour that differs from the vendor driver on purpose (all measured):

| Item | Vendor | Ours | Why |
|---|---|---|---|
| SPI clock | 9.6 MHz | **4 MHz** | 1 MHz wedges the firmware download; 4 MHz gives stable write+read-back (9.6 MHz was never validated on large transfers) |
| firmware chunk | 63 KiB | **16 KiB** | the mainline GPI/SPI path dies on ~64 KiB transfers |
| write verification | relies on the IC's BLD CRC | **read-back compare + bad-block rewrite** | SRAM writes were observed corrupted; rewriting converges to `bad=0` |
| report length | 109 B (super-resolution) | **66 B** (10 points × 6 B + header) | in this firmware the bytes from offset 98 are the `0x22D60` RESET_COMPLETE register, not pressure |
| pressure | `point_data[99+i]` | 5th byte of each point | as above |
| `0x77` idle pattern | treated as a firmware reset | **ignored** | on this IC the idle frame really is `0x77`; treating it as a reset re-flashed every idle frame |
| gestures / double-tap wake | yes | not implemented | needs `wakeup-source` + an input-event hook |
| stylus / factory test | yes | not implemented | not needed for basic touch |

First-initialisation handling was reworked on 2026-11-06 (`docs/handoff-20261106.md`):

* `nt36xxx_report_is_live()` — `0x77` is this firmware's **valid idle frame**, while
  `0xfd`/`0xfe` (and uniform `0xff`) mean the IC is still in reset. Before this, *any*
  non-`fd/fe` read counted as "usable", so a half-initialised controller could be published
  as downloaded.
* `nt36xxx_wait_first_report()` returns a bool and waits up to 100 × 5 ms (≈500 ms) for a live
  frame; on timeout the download is **not** marked complete, so the next panel cycle retries
  instead of leaving an IRQ storm behind.
* the `ReK` (reset-state) check is fatal now: `sky: IC failed to reach ReK` aborts the
  download rather than continuing with a half-initialised IC.
* the panel's 1.8 V rail (`vreg_panel_1p8`, gpio97) is `regulator-always-on`, so the touch
  supply does not disappear between init attempts.

One runtime recovery per long session is still observed and heals itself in ≈1.3 s
(`sky-recover[1]: reports all 0xFD/0xFE` → fresh download → `fw_ver=0x18` → `state=a3…`), with
`errors=0 timedout=0` in the SPI statistics — i.e. the IC entering its own reset state, not a
transport error.

Why the regmap port rather than the vendor import: it caps transfers at
`spi_max_transfer_size()` instead of handing the controller one multi-kilobyte buffer, which
is what the QUP dislikes, and it is small enough to reason about.

## Runtime recovery (why the box no longer needs a reboot)

The controller occasionally falls back into its reset state (`sky-report` frames become
`0xFD`/`0xFE`, IRQ storm ~200/s). Two independent defects caused the original "only a reboot
fixes it":

1. **the SPI bus was never serialised** — the threaded IRQ handler, the 2 s health poll and
   the init worker could interleave a page-select write with another transfer's data phase.
   Every SPI user now takes `ts->lock`.
2. **recovery could not reset the IC** — the reset pulse (`TP_RST` gpio12,
   `nvt_eng_reset()` 0x5A, release, 10 ms, `nvt_bootloader_reset()` 0x69) is now
   `nt36xxx_hw_reset()` and runs on every retry after the first.

Recovery never gives up permanently: 5 attempts 2 s apart, then one per minute; after a
healthy minute the fast phase is available again. Per-chunk logs are suppressed during the
slow phase so a dead IC cannot flush the ring buffer.

## Verification

```sh
dmesg | grep -E "Touch IC fw loaded|sky-report|sky-recover|sky-irq"
# expect: fw loaded, then report[1] carrying a real frame with state=a3.. (NORMAL_RUN)

grep spi0.0 /proc/interrupts; sleep 5; grep spi0.0 /proc/interrupts     # ~12 IRQ/s idle
grep -A5 -i novatek /proc/bus/input/devices                             # /dev/input/event3
dd if=/dev/input/event3 bs=24 count=200 2>/dev/null | od -Ax -tx1 -v   # EV_ABS while touching
```

`tools/decode_evdev.py` decodes the raw `input_event` stream.

## Known open items (stability testing)

0. **Desktop / window-manager validation** — the only verification so far is raw
   `/dev/input/event3` output. Run a real input stack (libinput + a Wayland compositor or X)
   and confirm clicks, drags, edge coordinates and gestures. **This is the reason the
   milestone is still `partial`.**
1. **Corner-to-corner coordinate check** with `tools/decode_evdev.py` (four corners and the
   panel edges) on the final image — the ranges are verified (0..1079 / 0..2459), the mapping
   at the extremes is not.
2. **Long-run behaviour** beyond the 45-minute evidence run: hours, and many suspend/resume
   cycles. One recovery during the first initialisation is still expected
   (`sky-recover[1]`), followed by stability. Debug hook: writing `1` to
   `/sys/bus/spi/devices/spi0.0/sky_reinit` replays the resume re-flash without a real
   screen-off.
3. **pinctrl sleep state** — the DTS wires only `default`/`ts_active`; the driver has no
   `pinctrl_select_state()` for `ts_int_suspend`/`ts_reset_suspend` yet.
4. **Gestures / double-tap wake** — needs `wakeup-source`, an input-event hook,
   host command `0x13` and `enable_irq_wake()` on gpio13.
5. Optional clean-up: downgrade the touch `dev_info`s to `dev_dbg` and drop the
   `sky-report`/`sky-state` noise once nothing is being debugged.

## Do not repeat

* Do not remove `qcom,enable-gsi-dma` / `dmas` — FIFO mode never executes on this SE.
* Do not change the GPI channel ids (TX 0, RX 1).
* Do not drop the `qup-memory` interconnect path — it is not optional under load.
* Do not remove the lock from any SPI user; the bus has no arbitration.
* Do not remove the reset pulse from the retry path; a software reset alone leaves a crashed
  MCU wedged.
* Do not read `/sys/kernel/debug/gpio` on this board (TZ-protected TLMM page, hangs the box);
  use `/sys/kernel/debug/pinctrl/*/pinmux-pins`.
* A failed kernel build silently leaves the old `Image` in place — the compiler prints
  Chinese errors (`错误 1`), so grep for `error:|Error [0-9]|错误` and check the Image mtime
  and version banner before flashing.

## History

`docs/handoff-touch-driver-20261104/` is the current package (technical write-up,
`DRIVER.md`, evidence logs, DTS node, firmware notes).
`docs/touch-handoff/`, `docs/touch-handoff2/`, `docs/touch-handoff3/` and
`docs/handoff{6..13}/` record the whole path: the vendor-driver import, the QUP wrapper
naming trap (`qupv3_id_0` = vendor "wrapper 1"), the FIFO dead end, the QUP-firmware panic,
the module-based iteration workflow, and finally the two defects above.
