# Handoff13: runtime firmware crash and recovery that works without a reboot

Date: 2026-11-01
Device: Xiaomi Redmi 12 5G (`sky`), Qualcomm SM4450/ravelin
Touch IC: Novatek NT36672C/NT36xxx on `spi0.0`
Follows: `handoff12-touch-working-20261031/` (the boot-time fix, hardware
confirmed) — that binary stays the last hardware-validated known-good.

---

## 0. What the user observed

Touch worked after the handoff12 fix, then one run died at ~571 s:

```text
[   10.031] Touch IC fw loaded ok
[   10.038] sky-report[0]: ffffffffffff state=a106e45480b8 irq_line=0
[  10.302] sky-report[1]: 0a138b5b4208 state=a30c01000000 irq_line=0   <- live, NORMAL_RUN
[ 583.221] sky-recover[5]: reports all 0xFD/0xFE, re-running IC init
[ 583.9..] (full download: CRC ok, FWINFO ok, "Touch IC fw loaded ok")
[ 584.020] sky-report[0]: ffffffffffff state=a10601010000 irq_line=0
[ 584.417] sky-report[1]: fefefefefefe state=fefefefefefe irq_line=1
[ 584.469] sky-recover: giving up after 5 re-inits
irq: 21677 -> 23763 in 10 s (~209/s storm, IC dead)
```

So: the readiness gate works (`report[1]` carried a real frame with the IC in
`a3`), the IC later fell back into its reset state, and **every re-init brought
the firmware up only for ~0.4 s** (`state=a1` = REK, then all `0xFE`). Only a
reboot restored touch.

---

## 1. Root cause: two independent defects

### 1.1 The SPI bus was never serialised

`grep mutex_lock` in the driver found **nothing** — `ts->lock` existed but was
never taken, while these can run concurrently:

* the threaded IRQ handler (`nt36xxx_report()`), and
* the 2 s health poll added in handoff12 (`nt36xxx_health_work()`), and
* the init/download worker.

SPI has no arbitration. `nt36xxx_spi_read()` issues a page-select write and
*then* a separate read message, so two interleaved users put the page select
and the data phase out of order on the wire. The vendor serialises every access
with `ts->lock` (`nvt_ts_work_func()`, `nvt_esd_check_func()`,
`nvt_ts_suspend()`); our driver did not. The health poll fires ~300 times in
10 minutes, which matches the "worked for a while, then died once" pattern far
better than anything else.

### 1.2 Recovery could not work without the reset pin

The vendor's download path (`nt36xxx_fw_update.c`,
`nvt_download_firmware_hw_crc()`) starts with a hardware reset:

```c
gpio_set_value(reset_gpio, 0);   /* assert TP_RST */
mdelay(1);
nvt_eng_reset();                 /* 0x5A to ENG_RST_ADDR, pin still held */
gpio_set_value(reset_gpio, 1);   /* release */
mdelay(10);                      /* tRT2BRST */
nvt_bootloader_reset();          /* 0x69 to SWRST_N8 */
```

Our driver never touched `reset-gpios` (GPIO12). A wedged MCU only comes back
through that pin, which is precisely why a reboot was the only cure: it is a
hardware reset. The vendor device tree confirms the pin
(`backup/live-fdt.dtb`: `novatek,reset-gpio = <&tlmm 0x0c 0x00>`), and our DTS
declares it as `GPIO_ACTIVE_LOW`, so `gpiod_set_value(…, 1)` asserts TP_RST.

---

## 2. Changes (only `nt36xxx_core.c`)

1. **`ts->lock` is now used for every SPI user**: the report path (whole
   exchange), the health poll, the init worker (whole sequence, including the
   firmware download) and the suspend-time sleep command. Suspend cancels the
   works *before* taking the lock, so the worker cannot deadlock against it.
2. **`nt36xxx_hw_reset()`** implements the vendor reset pulse. It is used on
   every init attempt after the first (`ts->recover_tries || sky_fw_attempts`),
   so the boot path stays byte-for-byte identical to the handoff12 binary that
   was validated on hardware. It logs `sky-recover: TP_RST pulsed`.
3. **Recovery never gives up permanently**: 5 attempts 2 s apart, then one per
   minute for as long as the IC stays dead (`sky-recover[N]` keeps counting).
   After a full healthy minute the counter resets and the fast phase is
   available again.
4. **Quiet re-inits**: from the slow phase on, the per-chunk `sky-fw:` /
   `sky-repair:` / `sky-crc:` / chip-id logs are suppressed so a dead IC cannot
   flush the ring buffer; one `sky-recover[N]` line per attempt remains, plus
   the three `sky-report[0..2]` lines of each attempt. Verbose logging returns
   on panel wake.

---

## 3. Artifact

```text
images/sky-shim-boot-v4-recover-hwreset.img
sha256: 8ed569ca617688e8037a48ff0b4e54531bda3c55e0e7b731fd33b1b571c56bac
size:   120086528
```

also kept as `shim/out/sky-shim-boot-v4-recover-hwreset.img`
(`shim/out/sky-shim-boot-v4.img` is the live slot `build-sky.sh` rewrites).

```text
images/Image                 sha256 9909e411ac042a84811452786934e8e4a8783288fe9857fa0d71d452cbf7d1a9
dts/sm4450-xiaomi-sky.dtb    sha256 0faf91e9291e980bbbe5326acf43dd970175fea23f1c68a7e5832bb7f0a68586  (unchanged)
source/nt36xxx_core.c        sha256 4876f68e524574f332dc6be1372895a6cd4df17d7a75b8c49f447e6f95cc0201
source/nt36xxx-runtime-recovery.patch   diff vs the handoff12 working source
```

Handoff12's validated binary is untouched:
`shim/out/sky-shim-boot-v4-touch-ok.img` (`a16ce9e9…`).

---

## 4. What to check on the device

1. Normal boot - must be identical to handoff12:

```sh
dmesg | grep -E "Touch IC fw loaded|sky-report|sky-irq"
grep spi0.0 /proc/interrupts; sleep 10; grep spi0.0 /proc/interrupts   # ~12 IRQ/s
dd if=/dev/input/event3 bs=24 count=100 2>&1 | od -Ax -tx1 -v          # real EV_ABS
```

2. If the IC dies again, the log must now show recovery *working*:

```text
sky-recover[1]: reports all 0xFD/0xFE, re-running IC init
sky-recover: TP_RST pulsed
Touch IC fw loaded ok
sky-report[0]: <frame> state=a1..          <- firmware up
sky-report[1]: 0a.. state=a3..             <- real report, recovery succeeded
```

and touch works again **without a reboot**. If `sky-report` goes back to
`fe…` within a second, the reset pulse did not help and the next step is the
power/analog side (see §5).

3. Note the timestamps of the first `sky-recover` line: it tells us how long
   the IC survived, i.e. whether the bus-collision fix changed the failure rate.

---

## 5. Still open

* **Why the firmware dies at all.** The bus-collision window is closed now; if
  it still dies, the remaining candidates are an ESD event or an analog supply
  glitch. Evidence to collect: full dmesg (`dmesg`), the first
  `sky-recover` timestamp, `sky-report` `state=` bytes, and whether
  `TP_RST pulsed` recovers it.
* The health poll now holds the lock, so it can block a report read for one SPI
  transaction (~100 µs) - acceptable, and it can no longer interleave.
* The 2 s health poll frequency is a guess; if the IC proves to die rarely, it
  could be dropped entirely.
* Untested on hardware as of this archive: touch after display off/on, four
  corner coordinates.

---

## 6. Do not repeat

* Do not remove the lock from any SPI user; the bus has no arbitration.
* Do not remove the reset pulse from the retry path - software reset alone
  leaves a crashed MCU wedged (this is the "only a reboot fixes it" symptom).
* Do not re-introduce a permanent "give up" - a transient cause must be able to
  heal on its own.
* Do not pulse TP_RST on the first boot attempt without re-validating boot.
