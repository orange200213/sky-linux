# Handoff: finishing the WCN6750 **SNOC / adrastea** port in mainline `ath11k`

Status as of kernel **#83** (`wifi-backup/out/sky-shim-boot-v4-wifi-k83-snoc.img`),
device: Xiaomi Redmi 12 5G (`sky`), SM4450 "ravelin", WCN6750 "adrastea".
Goal of this thread: **mainline `ath11k` only** - no downstream kernel, no
`qcacld-3.0`. Everything below is measured on hardware unless marked otherwise.

Read `../m8-wifi.md` first for the project context (§2.3/2.4 hybrid vs SNOC,
§2.7..#2.17 experiment log). This file is the working handoff.

---

## 1. What already works

| piece | evidence |
|---|---|
| boot chain (shim + `SWEETLNX` payload, `load_pa=0xc0000000`, `dtb_pa=0xc7800000`) | `wifi-backup/repack-boot.py`, used for every build #61..#84 |
| WPSS remoteproc start with split-MDT firmware (`wpss.mdt` + `b00..b08,b10`) | `/sys/class/remoteproc/remoteproc1/state = running` |
| WLFW QMI over QRTR with **instance 0** (patch 0001) | `chip_id 0x4130 chip_family 0x4007 board_id 0xff soc_id 0x406a0000`, `fw_version 0x343b815a …CI_WLAN.HL.3.4.3-00346.1-QCAHLSWMTPLZ-23` |
| `host_cap` with `ddr_range` (TLV 0x1F, 3 entries) | `host cap ddr_range: 0+100000000, 82a00000+300000` accepted |
| MSA handshake `msa_mem_info` (0x2D) / `msa_ready` (0x2E), MSA = `0x82a00000+0x300000` | `msa 0x82a00000+0x300000, 2 region(s)` |
| `MSA_READY_IND` (0x2B) received, registered by the driver | `waiting for the MSA ready indication` then the indication arrives ~16 ms later |
| **regdb + BDF download complete** (19348 B type 4, 26328 B type 0, 6144 B segments) | `bdf seg 0..4 ok … end 1` |
| cal data download skipped, **`cal report` (0x26) sent** | `cal report sent`, then the firmware lets `ath11k_core_qmi_firmware_ready()` run |
| CE0..CE11 ring init (register block at `membase+0x240000`, one 0x1000 block per CE) | no abort, DP init reached |

## 2. The blocker (where every session ends today)

`ath11k_dp_srng_setup()` -> `ath11k_hal_srng_setup()` writes DP ring registers
whose offsets are the `HAL_SEQ_WCSS_UMAC_*` constants (`WBM 0xa34000`,
`REO 0xa38000`, `TCL 0xa44000`) and dies with a **synchronous external abort**
(`ESR 0x96000010`) inside the register write. Measured facts:

| address tried | result |
|---|---|
| `membase + 0x0` | DEAD (kills the caller - see §6 warning about PID 1) |
| `membase + 0x30000` (SR_APSS) | live |
| `membase + 0x240000` (CE0 block) | live |
| `membase + 0x80000 / 0x100000 / 0x180000` (stock static windows) | abort |
| `membase + 0xa34874` direct | abort (also with a 16 MiB mapping: pages OK, no hardware) |
| `membase + 0xb4874` (UMAC via stock window 1) | abort |

Stock-side facts that were established while chasing this:

* the stock `icnss2` maps exactly `resource_size(reg)` = **8 MiB** and its
  `hif_write32_mb()` is a plain `iowrite32()` (`hif_io32.h`: the window helper is
  `#if defined(HIF_REG_WINDOW_SUPPORT) && defined(HIF_PCI)`);
* the **host does program the rings** (`qca-wifi-host-cmn/hal/wifi3.0/hal_srng.c`),
  so this is not a "firmware owns the rings" design by construction;
* for `TARGET_TYPE_QCA6750` the HAL sets `use_register_windowing = true;
  static_window_map = true;` and `hal_get_window_address_6750()`
  (`hal/wifi3.0/qca6750/hal_6750.c`) maps:
  `window = (offset >> 19) & 0x3f`, UMAC `0x14` -> 1, CE `0x37` -> 2, CMEM `0x02` -> 3,
  `addr = scale*0x80000 + (offset & 0x7ffff)` - **does not work on this device**;
* `ADRASTEA_WINDOW_{DATA,READ_ADDR,WRITE_ADDR}_ADDRESS` are all **MISSING**, so
  there is no window-selector register on adrastea (kernel #82 tried 0x310C and
  the write itself aborted);
* `adrastea_reg_def.h` never describes an offset above `0x002F1008`; the only
  remaining host->WCSS write port documented there is the **SR_APSS shadow
  register block**: `SHADOW_ADDRESS0..23 @ 0x00030020+4n`,
  `SHADOW_VALUE0..23 @ 0x00032000+4n`, `ADDRESS_VALID @ 0x000300E0`,
  `INVALID_ADDR_ACCESS`/`DIRTY`/`SR_CONTROL`/`SR_INTERRUPT_CLEAR`,
  `FORCE_WAKE @ 0x32060`, `SW_SCRATCH @ 0x32064`, `WCSSAON_SR_LSB @ 0x32070`
  (`SHADOW_ADDRESS2__ADDRESS_REGISTER___M = 0x3FFFFF`, i.e. a 4 MiB window);
* while the host pokes the CE/misc area the firmware asserts:
  `cmnos_thread.c:4003: Asserted in copy_engine.c:_CE_misc_handler:2227`
  (WPSS then recovers via remoteproc) - the firmware is watching those registers.

## 3. Next steps, in the order they should be attempted

1. **Move the register probe out of the bind path** (`ath11k_ahb_snoc_probe_regs()`
   is already written and behind `snoc_probe`, default off). Put it in a
   workqueue scheduled from probe (or a debugfs/sysfs trigger) so a dead address
   kills a kworker instead of PID 1. Sweep candidates *outside* the DT region:
   the UMAC block may live at a second physical base that the DT does not
   describe (`0x22a00000`, `0x23000000`, `0x23200000`, …). Use
   `copy_from_kernel_nofault()` for every access.
2. **Find out what `hal_soc->dev_base_addr` is for the SNOC build.** If the stock
   stack ioremaps a *second* region (or receives a "BAR" address from the
   firmware) and passes that to the HAL, then the UMAC block has a different
   physical base and step 1 will find it. Grep the stock kernel tree for
   `dev_base_addr`/`pld_get_...` wiring in the SNOC path
   (`icnss2` -> `hif` -> `hal`), and compare with `pld_get_icnss_hw_info()`.
3. **If the UMAC block is genuinely unreachable by MMIO**, implement the SR_APSS
   shadow write port and verify it against a harmless UMAC register read
   (e.g. a version/scratch register) before touching ring registers. Expect a
   handshake; `DIRTY`/`ADDRESS_VALID`/`INVALID_ADDR_ACCESS` are the only status
   bits available.
4. **Then** the rest of the bring-up: extended interrupts (`ath11k_ahb_config_irq()`
   currently returns early for SNOC - the SNOC DT has no ext IRQ resources, so
   this needs a MISC-status dispatch or per-`grp` handling), `ath11k_dp_alloc()`
   ring programming, `ath11k_core_start()`/WMI-HTC, and finally scan/assoc.
5. Keep `regdb`/BDF variants handy: `wifi-backup/firmware/image/adrastea/` has
   `bdwlan.bin`, `bdwlan_{cn,eu,in,jp}.bin` and the numbered per-board files
   (`bdwlan.102`, `.104`, …); `bdf_name=` selects any of them at run time.

## 4. How to work on it (the loop that produced #63..#84)

```bash
# build (in-tree, kernel already configured)
cd ~/redmi125g/sky-mainline/kernel/linux-7.3
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j12 Image
# check: 0 errors, banner build number bumped, then
cd ~/redmi125g/wifi-backup
python3 repack-boot.py <Image> wifi-snoc-fdt.dtb out/sky-shim-boot-v4-wifi-kNN-snoc.img
# verify: only DTB/Image/image_size/kernel_size differ from the original image
```

Flashing needs the user to enter fastboot (no fastboot from device Linux):

```bash
fastboot flash boot_a out/sky-shim-boot-v4-wifi-kNN-snoc.img
fastboot reboot
```

**Iterate without flashing**: `/etc/sky-wifi-params` (`key=value` per line) is
applied by `/usr/local/sbin/sky-wifi-up.sh` at boot *before* the device is
bound; then `sysrq-b` reboots (~70 s per iteration):

```bash
ssh root@192.168.7.1 'printf "bdf_total_full=y\nsnoc_reg_mode=2\n" > /etc/sky-wifi-params;
                      sync; (sleep 1; echo b > /proc/sysrq-trigger) &'
```

Always keep `bdf_total_full=y` in that file - without it the BDF download
fails (see §5.1).

Crash logs survive reboots: the helper mounts pstore. After a crash read
`/sys/fs/pstore/console-ramoops-0` (and `dmesg-ramoops-0/1`); ramoops is 2 MiB
at `0x97700000` and doubles as a legacy console.

## 5. Hard-won knowledge (do not re-derive)

1. **`total_size` semantics** - the firmware accepts a BDF/regdb segment only if
   `total_size` is the **size of the whole file** on *every* segment (that is
   what stock `icnss2` sends). ath11k sends "remaining". This was the real cause
   of `board file download request failed: 1 19` for two days of work. Knob:
   `bdf_total_full=true`.
2. **cal report** - the ADRASTEA branch sends `QMI_WLFW_CAL_REPORT_REQ_V01`
   (`0x26`, `meta_data_len = 0`) right after the BDF. ath11k never does; without
   it the firmware stays silent after the BDF and nothing else happens. Knob:
   `snoc_cal_report=true`.
3. **cal data file** - after the BDF ath11k wants `cal-ahb-22800000.wifi.bin`;
   this platform has none, and `ftm_mode` cannot be used (registered 0444).
   Knob: `cal_download=false`.
4. **QMI error numbers**: `19` = the firmware's *service-specific* rejection
   (we now know it meant the `total_size` mismatch, not "wrong state");
   `48` = `QMI_ERR_INVALID_ARG`; `90` = `INCOMPATIBLE_STATE`;
   `94` = `NOT_SUPPORTED` (e.g. `device_info` on this firmware - it is an
   SNOC-variant firmware, not the hybrid one).
5. **WLFW instance** must be `0` for this firmware (patch 0001), not the
   upstream `3` for WCN6750.
6. **MSA_READY_IND (0x2B)** has to be *requested* in `ind_register`
   (`msa_ready_enable`) before the firmware sends it.
7. **CE register map on adrastea**: one 0x1000 block per CE at
   `0x240000 + n*0x1000`; src ring regs at `+0x00`, dst ring regs at `+0x0c`
   (from `adrastea_reg_def.h`). `mem_ce` must equal `mem` so
   `ATH11K_CE_OFFSET()` is 0. Mainline's `0x01b80000` CE bases are PCI-window
   space and must not be used here.
8. The AHB hif ops pointer to the PCI MSI helper
   (`ath11k_pcic_get_user_msi_assignment`) dereferences `ab->pci.msi.config`,
   which is NULL on SNOC; `#79` adds `ath11k_ahb_get_user_msi_vector()` that
   returns `-ENOPROTOOPT` in SNOC mode.
9. `ath11k_ahb_free_irq()` used to free the extended IRQ groups that SNOC never
   sets up -> NULL deref on `unbind`; guarded.

## 6. Traps that cost real time

* **Never run a register probe from the bind path.** The bind is issued by
  `/init` (PID 1); a bus abort there kills init and panics the box
  (`Attempted to kill init!`) - that is what happened to #84.
* **Never `unbind` a device whose probe failed** before patch 0002 is applied
  (`ath11k_ahb_remove()` used to run `ath11k_core_deinit()` without
  `ATH11K_FLAG_REGISTERED`). With 0002 it is safe and is the fastest iteration
  tool, but see the next item.
* Binding/unbinding from a shell is fine; **do not** unbind from PID 1 either.
* **Do not read `/sys/kernel/debug/gpio`** - TLMM is TZ-protected and the box
  hangs.
* dmesg keeps only ~1000 extra lines after boot; per-boot logs are in `/log/`.
* `msa 0x82a00000+0x300000` is accepted by the firmware; `qcom,msa-fixed-perm`
  means TZ owns the MSA permissions, no `qcom_scm_assign_mem()`.
* Never overwrite the existing project files: everything new lives in
  `wifi-backup/` and the `sky-linux` repo.

## 7. Artifacts

* kernel tree: `~/redmi125g/sky-mainline/kernel/linux-7.3` (patches in
  `sky-linux/kernel/patches-m8/000{1,2,3}-*.patch`, 0003 refreshed to #83)
* boot images: `wifi-backup/out/sky-shim-boot-v4-wifi-k61..k84-snoc.img`;
  **#83 is the last one that boots** (#84 panics in probe)
* rollback: `fastboot flash boot_a wifi-backup/boot/boot_a-original.img`
  (note: this file now contains our own build - the *stock* boot image has to
  come from the stock ROM package)
* device-side: `/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.*`,
  `/lib/firmware/ath11k/WCN6750/hw1.0/{board.bin,regdb.bin,bdwlan_*.bin}`,
  `/usr/local/sbin/sky-wifi-up.sh`, `/etc/sky-wifi-params`, `/init`
  (backup `/init.pre-m8`)
* host-side helper: `wifi-backup/{patch-fdt-snoc.py,repack-boot.py,wifi-retry.sh,
  wifi-bind.sh,logs/build-m8-wifi-k*.log}`

## 8. Runtime knobs (all `module_param`s, `/sys/module/ath11k/parameters/*`
unless noted)

```
snoc_host_cap=0|1|2      where to send host_cap (default 1: before MSA)
snoc_ddr_range=0|1|2     host_cap ddr_range contents (default 1: wide + MSA)
snoc_ind_vendor=Y|N      stock ADRASTEA ind_register bit set (default Y)
snoc_bdf_wait_ind=Y|N    download BDF only after MSA_READY_IND (default Y)
snoc_bdf_download=Y|N    download the BDF at all
snoc_bdf_delay_ms=N      delay before the BDF download
snoc_bdf_type=N          override bdf_type (-1 = auto: 0 bin / 1 elf / 4 regdb)
snoc_cal_report=Y|N      send QMI_WLFW_CAL_REPORT_REQ_V01 after the BDF
cal_download=Y|N         download the separate cal data file (default N)
msa_handshake=Y|N        msaa_info/msa_ready handshake
regdb_download=Y|N       download regdb.bin before the BDF
bdf_chunk=N              segment size (<= 6144)
bdf_file_id=N            file_id override (-1 = board_id from cap)
bdf_total_full=Y|N       total_size = whole file on every segment  (KEEP Y)
bdf_name=<file>          use this file from fw.dir as the board file
snoc_probe=Y|N           register probe (dangerous, see §6)
/sys/module/ath11k_ahb/parameters/snoc_fixed_mem=Y|N   firmware owns its MSA
/sys/module/ath11k_ahb/parameters/snoc_membase_mib=N   membase window size
/sys/module/ath11k_ahb/parameters/snoc_reg_mode=N      0 mmio / 1 selector /
                                                       2 CE direct+selector /
                                                       3 stock static map /
                                                       4 static map + CE direct
```

## 9. Honest assessment

A lot of the bring-up is done: the firmware is up, the whole WLFW/QMI contract
(including the BDF and the cal report) is satisfied, and the driver runs the real
`ath11k_core_qmi_firmware_ready()` path. What remains is the data path, and it is
blocked by one unknown: **where the WCSS UMAC/WBM/REO/TCL register block is on
this SNOC variant**. Once that is answered the rest is ordinary driver work
(rings, ext IRQ dispatch, WMI/HTC). Until it is answered, no amount of ring or
firmware debugging helps.

If step 1/2 of §3 shows the block is at another physical base, expect a few more
sessions. If it shows the block is genuinely not reachable from the APSS, this
port needs the SR_APSS shadow port - a much bigger, riskier job - and that is the
point at which "mainline WiFi on this device" should be re-evaluated.
