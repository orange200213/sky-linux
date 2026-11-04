# WCN6750/adrastea —— ath11k 移植线【已关闭，2026-11-02】

> ⚠️ 本线已终止：APSS 无法访问 CE/UMAC 寄存器（硬件实测，见
> `FINDINGS-session9-shadow-reg-confirmed.md` §19/§20），mainline ath11k 模型在此平台不可行。
> 后续方向请看 **`HANDOFF-qcacld-mainline.md`**。以下为历史记录，保留供参考。

# Handoff: finishing the WCN6750 **SNOC / adrastea** port in mainline `ath11k`

> **Scope (English summary of the notice above).** The pure-mainline `ath11k` line is
> **closed**: hardware probing showed the APSS can only reach the SR_APSS block
> (`membase+0x20000..0x52000`) and 12 CE write-pointer shadow slots — the CE ring base/size
> and the UMAC/WBM/REO/TCL registers are unreachable in the 8 MiB window, so ath11k's
> "host programs the rings" model cannot work here (evidence:
> `FINDINGS-session9-shadow-reg-confirmed.md`). Working Wi-Fi comes from the out-of-tree
> `qcacld-3.0` port in
> [`sky-qcacld-3.0-mainline`](https://github.com/orange200213/sky-qcacld-3.0-mainline);
> see `HANDOFF-qcacld-mainline.md` and `STATUS-qcacld-mainline-2026-11-03.md`. Everything
> below is kept as the historical record of the ath11k attempt.

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

---

## 10. Session 3 update: public Adrastea SR_APSS definition

A public Qualcomm-derived definition is available at:

<https://github.com/SpaceExplorationTechnologies/starlink-wifi/blob/1d585ee8909bf7bab4391c0c52d8a9d44137b3bb/qca/src/qca-wifi/cmn_dev/hif/src/adrastea_reg_def.h>

Confirmed APSS-local offsets relative to the `0x22800000` membase:

| register | address |
|---|---:|
| `SR_CONTROL` | `0x30000` |
| `SHADOWREG_STATUS` | `0x30008` |
| `SR_INTERRUPT_STATUS` | `0x3000c` |
| `SR_INTERRUPT_CLEAR` | `0x30014` |
| `SHADOW_ADDRESS0..23` | `0x30020 + 4*n` |
| `DIRTY` | `0x30080` |
| `ADDRESS_VALID` | `0x300e0` |
| `SHADOW_VALUE0..23` | `0x32000 + 4*n` |
| `FORCE_WAKE` | `0x32060` |
| `SW_SCRATCH` | `0x32064` |
| `WCSSAON_SR_LSB/MSB` | `0x32070/0x32074` |
| `INVALID_ADDR_ACCESS` | `0x32078` |

Address fields use mask `0x003fffff`; `ADDRESS_VALID` and `DIRTY` are 24-bit slot bitmaps. `0x300e4` is not a defined status register; the previous experiment reading it caused an external abort.

### Critical distinction

Mainline ath11k HAL shadow-ring configuration (`hal_shadow_base_addr`, `shadow_reg_v2[]`, QMI WLAN CFG) is for CE/DP ring head/tail shadows. Adrastea SR_APSS shadow is a separate APSS-to-WCSS MMIO transport and is the missing SNOC backend.

### Session 3 measured state

WPSS/QMI instance 0/host_cap/MSA/chip/BDF/regdb/cal report and CE direct initialization all work. The remaining failure is:

```text
ath11k_hal_srng_setup() -> write offset 0xa34874
synchronous external abort 0x96000010
WPSS copy_engine.c:_CE_misc_handler:2227
```

Safe runtime configuration remains:

```text
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
```

### Next implementation gate

Recover the stock host-common sequence before enabling transport: establish SR_CONTROL/full APSS access and wake semantics; use `SHADOWREG_STATUS` at `0x30008`; test one slot with harmless WCSS scratch/version access; inspect `DIRTY`, `SR_INTERRUPT_STATUS`, and `INVALID_ADDR_ACCESS` only at defined addresses; determine readback before redirecting DP writes. Detailed notes are in `docs/wifi-handoff/FINDINGS-session3-adrastea-shadow.md`.

---

## 11. Session 4 update: stock Adrastea CE shadow evidence

The user supplied a public Qualcomm-derived Starlink/QCA source tree. Relevant
files include `adrastea_reg_def.h`, `ce/ce_reg.h`, `ce/ce_main.c`,
`regtable.c`, and `dispatcher/multibus_snoc.c`:

<https://github.com/SpaceExplorationTechnologies/starlink-wifi/tree/1d585ee8909bf7bab4391c0c52d8a9d44137b3bb/qca/src/qca-wifi/cmn_dev/hif/src>

Stock code under `ADRASTEA_SHADOW_REGISTERS` does **not** redirect every CE or
UMAC register to one guessed shadow slot. It maps only CE write-index accesses
to configured `SHADOW_VALUE` slots:

```text
CE source 0,3,4,5,7  -> slots 0,3,4,5,7
CE dest 1,2,5,7,8,9,10,11 -> slots 13,14,17,19,20,21,22,23
```

It reads the configured `SHADOW_ADDRESS[n]` and prints the result shifted left
by two, indicating that the shadow address field is likely a word address.
The source tree does not show the complete initialization of
`SHADOW_ADDRESS`/`ADDRESS_VALID`; that may be performed by firmware, another
vendor layer, or an omitted source component.

This is a critical correction: the mainline ath11k HAL shadow list
(`shadow_reg_v2[]`) and the Adrastea SR_APSS hardware shadow port are related
but not identical. The stock CE shadow mechanism cannot automatically solve the
current crash at UMAC/DP offset `0xa34874`.

Detailed evidence is recorded in
`docs/wifi-handoff/FINDINGS-session4-stock-adrastea.md`.

### Current gate

Do not enable the experimental `snoc_reg_mode=5` transport. Keep the device at
`snoc_reg_mode=4` until the slot initialization and read/write semantics are
verified. The next useful work is to correlate ath11k's QMI `shadow_reg_v2[]`
addresses with stock Adrastea slot mappings, and find the missing
`SHADOW_ADDRESS`/`ADDRESS_VALID` initialization path.

---

## 12. Session 5 update: read-only SR_APSS probe on hardware

A read-only diagnostic was added behind the module parameter
`snoc_shadow_probe=y`. It reads only offsets explicitly present in the public
Adrastea register definition and leaves the normal register path at
`snoc_reg_mode=4`.

The diagnostic image was flashed as k91:

```text
wifi-backup/out/sky-shim-boot-v4-wifi-k91-snoc-shadow-status.img
```

The device booted successfully and SSH returned. Kernel:

```text
7.3.0-g78458d4eea4e-dirty #90
```

Runtime parameters used:

```text
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
snoc_shadow_probe=y
```

### Hardware readings

```text
+0x30000 SR_CONTROL          = 0x00000000
+0x30008 SHADOWREG_STATUS    = 0x00000008
+0x3000c SR_INTERRUPT_STATUS = 0x00000000
+0x30010 SR_INTERRUPT_ENABLE = 0x000007ff
+0x30014 SR_INTERRUPT_CLEAR  = 0x00000000
+0x30080 DIRTY               = 0x00000000
+0x300e0 ADDRESS_VALID       = 0x00000000
+0x32060 FORCE_WAKE          = 0x00000000
+0x32064 SW_SCRATCH          = 0x11530000
+0x32068 PMM_SR_LSB          = 0x32002002
+0x3206c PMM_SR_MSB          = 0xbb01f000
+0x32070 WCSSAON_SR_LSB      = 0x0010200c
+0x32074 WCSSAON_SR_MSB      = 0x01bcf808
+0x32078 INVALID_ADDR_ACCESS = 0x00000000
```

Interpretation:

- The SR_APSS block is readable through the `0x22800000` DT membase.
- `SHADOWREG_STATUS=0x8` indicates the WCSS core wake/sleep status bit is set.
- `SR_INTERRUPT_ENABLE=0x7ff` is already configured by firmware/boot code.
- `DIRTY=0`, `ADDRESS_VALID=0`, and `INVALID_ADDR_ACCESS=0` immediately before
  the normal DP failure: no host shadow slots are currently valid or dirty.
- `SW_SCRATCH=0x11530000` provides a useful nonzero APSS-local sanity value.
- This validates the correct status addresses and disproves the old `0x300e4`
  status guess.

The normal path still reaches `cal report` and then aborts at DP setup with the
WPSS `_CE_misc_handler:2227` assertion. The read-only probe itself does not
cause the abort.

### Handoff for next session

The next session should not blindly redirect all UMAC accesses to shadow slot 0.
The hardware starts with `ADDRESS_VALID=0`; a valid shadow transport must first
configure slot address/value semantics. Stock Starlink/QCA source maps only CE
write-index operations to selected shadow slots and reads `SHADOW_ADDRESS[n] <<
2`; it does not expose the complete slot initialization path.

Useful next experiment, only after source-level review: configure one harmless
slot using the exact stock ordering, then read back `DIRTY`,
`SR_INTERRUPT_STATUS`, `INVALID_ADDR_ACCESS`, and `SHADOWREG_STATUS`. Do not
write `0xa34874` or enable mode 5 until that harmless test is understood.

Detailed records:

- `docs/wifi-handoff/FINDINGS-session3-adrastea-shadow.md`
- `docs/wifi-handoff/FINDINGS-session4-stock-adrastea.md`

---

## 13. Session 6 update: APSS 侧到底有没有解码 CE/UMAC（答案：没有）

完整记录见 `docs/wifi-handoff/FINDINGS-session6-apss-aperture.md`。要点：

1. **原厂源码路径已确认**（`sky-lineage/android_kernel_xiaomi_sm8450-modules@lineage-23.2`）：
   SNOC 的 target type 是 `TARGET_TYPE_ADRASTEA`，而 `hal_target_based_configure()`
   的 case 表里**没有** ADRASTEA → `use_register_windowing=0`，`hif_write32_mb()`
   在 SNOC 下是裸 `iowrite32`。所以原厂若跑 DP ring setup，写的就是
   `membase + 0xa34874`（=0x23234874）。
2. **该地址在本机未解码**：membase 8 MiB 内做 4 KiB 页扫描，只有
   `0x22820000/0x22822000/0x22830000/0x22832000` 四页有解码；`0x22824000`(CE)、
   `0x22880000/0x228B4874`(窗口 1)、`0x23234000`(直连 UMAC) 全部 abort。
3. **不是时序问题**：探针挂在 bind 之前，以 100 ms 采样覆盖整个 bring-up（WPSS→QMI
   →MSA→BDF→cal report→DP），CE/UMAC 地址始终 DEAD，`SHADOWREG_STATUS` 在 256 ms
   起就是 `0x8`（WCSS awake）。
4. **SR_APSS 权限实测**：`SW_SCRATCH`/`SHADOW_VALUE` 可写；`SHADOW_ADDRESS`、
   `ADDRESS_VALID`、`SR_CONTROL`（含 `ENABLE_APSS_FULL_ACCESS` bit2）对 APSS **只读**。
   `FORCE_WAKE=1` 能引发中断但不会让 UMAC 可见。
5. **纠错**：CE 阶段从未发生寄存器访问（0x22824000 实测 DEAD 且 DP 前无 abort），
   所以 §1 里"CE 直接映射可用"的里程碑不成立。
6. **`snoc_pre_bdf=5` 会整机挂死**并随后落到 fastboot。恢复镜像：
   `wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`（内核不变，DT 里
   wifi 节点 `status="disabled"`），进 Linux 改回参数后再刷 k91。

新增可用工具（无需刷机迭代）：`wifi-backup/regtool/`（`skyreg.ko` + `skyprobe`，
mmap 任意物理区间，EL0 上 bus abort → SIGBUS 可捕获）。注意 `copy_from_kernel_nofault()`
在 arm64 兜不住 external abort，内核态探针会 oops。

下一步：拿到原厂 `qca_cld3_*.ko`（`vendor_dlkm.img`）反汇编，确认 profile 与
`hal_srng_setup` 的真实写入地址/窗口初始化；在此之前不再盲改寄存器路径。

---

## 14. Session 7 update: 原厂日版基线（改变了整个判断）

详细记录：`docs/wifi-handoff/FINDINGS-session7-stock-jp-baseline.md`

### 14.1 基线纠正

本机是**日版**（`river_jp`），此前一直刷的欧版 MIUI 13
（`sky_in_global_images_V14.0.7.0.TMWINXM_13.0`）刷 super 会中断，是错的基线。
正确基线是 **`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`（HyperOS 14）**，
可完整刷入，WiFi 正常。

### 14.2 三条硬结论

1. **DTS 没变**：两版 `dtbo.img` 的 `qcom,icnss@22800000` 节点逐字段一致
   （`reg = <0x22800000 0x800000>`，同名中断）；`qcom,wcn6750`（msi_addr 变体）
   两版都是 disabled。我们 port 的 DTB 方向正确，不需要按日版重做。
2. **原厂跑的是 `qca_cld3_adrastea.ko`**（`/proc/modules` 里 `icnss2 ... adrastea`），
   不是 `qca6750.ko`（后者 insmod 失败）。这与 session 6 从**欧版 ROM** 得出的
   "跑 qca6750" 的推论相反——欧版 ROM 里的 `adrastea.ko` 与日版实质相同，
   真正的分野是 EU/JP ROM 都用 adrastea profile，而**两个 .ko 都没有
   `hal_attach`/`hal_srng_setup`/`hw_srng_table`/`TARGET_TYPE`**。
3. **adrastea 驱动不编程 WBM/REO/TCL**：从正在运行的 .ko 的 `.data` 读出的
   `adrastea_ce_targetdef` / `adrastea_targetdef` 里只有
   CE wrapper `0x24C000`、SI `0x84000`、RTC `0x45000`、SOC_CORE `0x113000`，
   **没有 0xa00000 的 UMAC 项**。原厂 ioremap 也只有
   `0x22800000`(8 MiB+4 KiB) 和 MSA `0x82a00000`(3 MiB+4 KiB)，无第二个 BAR。

⇒ mainline ath11k 的 `ath11k_ce_init_ring()` / `ath11k_dp_srng_setup()` 在本硬件上
访问的是"主机不该碰"的寄存器，abort 是**架构性**的，不是地址算错。
DP 数据面在这颗芯片上由固件自己管理（CE-only 主机侧）。

### 14.3 剩下的真实缺口

我们的 mainline 在 8 MiB 里只有 4 页可读（`0x22820000/0x22822000/0x22830000/
0x22832000`，后两者与 SR_APSS 互为别名），连原厂 CE wrapper `0x22824C000` 都是 DEAD，
而原厂同一硬件可用 ⇒ **缺一个环境使能（供电/时钟）**。

下一轮（成本低、结论强）：
1. 刷回 k91/k93，采集与本轮 `backup/stock-jp-2026-11-02/stock_regulators.txt`
   同格式的 regulator 快照，逐条 diff；stock `enabled` 而 mainline `disabled` 的
   就是嫌疑人（重点：icnss2 消费的 `rpmh-regulator-ldob11/14/23/ldoe7` +
   `regulator.18/21/31/44`）。
2. 查 mainline `ath11k_ahb_power_up()` 对 wcn6750 是否缺 `regulator_bulk_enable()`
   与 `cxo_ref_clk_pin` 时钟。
3. 补上后先只读 `0x22824C000`（CE wrapper）验证是否变活，再谈 CE/DP。
4. 架构评估：即使寄存器变活，mainline 的 DP ring 模型在此芯片上无意义；
   要评估 CE+HTC/WMI/HTT-only 的最小改造，或重新评估该路线的可行性。

### 14.4 k93 之前的工作全部保留

k61..k93 镜像、`wifi-backup/regtool/`（免刷机寄存器探针）、原厂模块、
session 3–6 的全部分析与本次日版基线证据都在仓库里；
恢复原厂前的设备备份在 `backup/pre-stock-2026-11-02/`（boot_a / rootfs / misc，md5 已校验）。

---

## 15. Session 8：原厂 QMI 顺序取证完成（详见 FINDINGS-session8-stock-qmi-order.md）

要点（源码 + 两个原厂 .ko 反汇编双向确认）：

1. 原厂 ADRASTEA(0xabcd) 分支**不发 HOST_CAP、不发 DEVICE_INFO**；
2. `WLAN_INI/CFG/MODE` **必须在 `WLFW_FW_READY_IND` 之后**（我们提前发 → 90 INCOMPATIBLE_STATE）；
3. **首次 CE 寄存器访问在 `WLAN_MODE` ACK 之后**（`hif_wlan_enable` → `hif_config_ce`）；
4. icnss2 没有任何"解锁 APSS 寄存器访问"的代码 ⇒ **门由 WPSS 固件自己开**，
   host 唯一的钥匙是把 QMI 走到 WLAN_MODE；
5. **关键新机制**：原厂 ADRASTEA build 启用了 `ADRASTEA_SHADOW_REGISTERS`——
   CE 的 write-index 写的是 **SR_APSS 影子槽**（`SHADOW_VALUE[n]`），而且
   `WLAN_CFG` 必须带 **`shadow_reg`(v1)** 的 24 槽表（我们发的是 `shadow_reg_v2`/空）
   ⇒ 这解释了 session 3–5 实测到的 `ADDRESS_VALID=0`（固件没收到表，槽位从未配置）。

下一步（k95）：关掉所有实验开关、CFG 改发 `shadow_reg`(v1) 的 12 条 `{ce_id,offset}` 表、
CE/DP 前加日志并在 `firmware_start` 失败时立即停止；起来后读 `0x228300e0` 验证
`ADDRESS_VALID` 是否非零。DP ring 另论（原厂根本不编程 DP）。

---

## 16. Session 9：SR_APSS 影子通道**完全打通**（详见 FINDINGS-session9-shadow-reg-confirmed.md）

| 里程碑 | 结论 |
|---|---|
| 机制 | 主机在 `WLAN_CFG` 里发 **`shadow_reg`(v1)** 表 → 固件配置 `SHADOW_ADDRESS[n]`（字地址）+ `ADDRESS_VALID`；主机写 `SHADOW_VALUE[n]`（`membase+0x32000+4n`）由硬件转发到对应 CE 寄存器 |
| 关键前提 | **CE 配置必须是原厂 adrastea 的 12 管表**（`ce_count=12`、`target_ce_count=12`）；用 qca6390 的 9 管表会让固件在 `platform_config_shadow_reg:241` 断言 |
| 证据 | k98 后 `ADDRESS_VALID=0xf860b9`（12 槽，与原厂映射逐位一致）；写 `SHADOW_VALUE0` → `DIRTY` 被消费、`SR_INTERRUPT_STATUS` bit0 触发、`INVALID_ADDR_ACCESS=0` |
| 固件 | 已换成日版原厂 2024 版（`fw_version 0x3431fb72`），来源 `NON-HLOS.bin` 的 `image/adrastea/wpss.*`；旧版备份在设备 `/root/wpss-old/` |
| 剩余 | ① CE 写指针改走影子通道（k99，见 §13.1）；② ring base/size 是否由固件预配置待确认；③ **DP ring 不可达**，`ath11k_dp_srng_setup()` 需禁用/改造 |

镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k98-adrastea-ce.img`（md5 `0e53a0b1…`）
安全参数：`bdf_total_full=y / snoc_reg_mode=4 / snoc_membase_mib=0 / snoc_probe=n /
snoc_shadow_probe=y / snoc_device_info=n / snoc_shadow_reg_v1=n`
