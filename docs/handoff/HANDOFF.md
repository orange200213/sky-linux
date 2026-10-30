# Panel handoff — Xiaomi "m19" 1080x2460 DSI panel on SM4450 (Redmi 12 5G / sky)

Status: **SOLVED — the panel works.** The image was corrupt because the DPU told the
DSC encoder it had four times more rate-buffer than the hardware provides; the fix
and the full working recipe are in section 0. Everything below it is the bring-up
record (vendor data, register values, and the experiments that led there) and stays
useful as reference for the remaining display work.

> Correction from later direct vendor register tracing: section5's assertion that
> DSC-info+3 is pkt_per_line is incorrect; it is slice_count. Actual vendor
> DSI producer consumes +144 packet_count and +152 bytes_per_packet, yielding
> **one1080-byte packet per line**, `VID_COMP=0x04380b01`, for DT slice-per-pkt2.
> See `vendor-packetization-followup.md`. This supersedes the two540-byte packet
> conclusion below. Later experiments: long writes,90Hz,EOT on,initial_lines3,
> and vendor14-byte PHY timings all failed to produce a clean picture.

Last updated after kernel image `sky-m6n-dsi-regs.img` + panel module with
`pkt_per_line = 2`.

---

## 0. Resolution: the working configuration (panel OK)

Working kernel: `build/twrp-dsc-buffer/improved-state/boot.img`
(sha256 `50846c5b4503e78302ed39e8ca388351e42243c4027cbe470ad31b95e6e0a840`),
panel module `b26ea6bbb0eddafa96b0654046697b44029f841470d59c09ab56fcee6bb2ee1f`.
dmesg of the working boot: `fixed-state-reference/dmesg.txt`.

Five things are needed together; the last one is the actual root cause:

1. **Vendor PPS.** DSC 1.1 with `initial_xmit_delay = 170` (Xiaomi lut_v1), which
   makes `drm_dsc_compute_rc_parameters()` derive dec 526 / final_offset 7072 /
   scale_increment 113 / nfl 1294 — i.e. the same 128-byte PPS the vendor sends
   (section 5).
2. **Vendor 14-byte DSI PHY timings** from the DTBO (`[00 18 06 06 15 20 06 06 07
   02 04 00 15 0b]`) applied for the m19 mode (`sky-phy: vendor m19 timings`),
   instead of letting mainline pick the values for the computed bit clock.
3. **One 1080-byte packet per line** (`slice_per_pkt = 2`, i.e.
   `MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT`), which is what the vendor producer really
   programs: `VID_COMP = 0x04380b01` (WC = 1080 = both 540-byte slices).
4. **Init sequence sent as generic long writes** (`0x29`-style "vendor long write"),
   verified by `sky-panel: vendor-long-write init`.
5. **Correct DSC rate-buffer size on SM4450.** `_dsc_calc_output_buf_max_addr()`
   in `dpu_hw_dsc_1_2.c` returns `2400 / num_softslice - 1` (= 1199 with two
   slices), but the SM4450 block holds only 600 bytes, so the field must be
   `600 / num_softslice - 1` (= 299). With 1199 the encoder believed it could
   buffer four times the real capacity, kept emitting data the block could not
   hold, and every line reached the DSI/DDIC malformed — the banded noise whose
   layout followed the slice geometry, with no DSI link errors at all.

   Patch: `kernel/patches/0003-dpu-dsc-sm4450-output-buffer-size.patch`
   (also `fixed-state-reference/startup-buffer.patch`).

   ```c
   /* SM4450 exposes a 600-byte DSC rate buffer, not 2400 */
   if (of_machine_is_compatible("qcom,sm4450")) {
   	max_addr = 600 / num_softslice;
   	pr_info("sky-dsc-buffer: max_addr=%d slices=%d\n", max_addr - 1, num_softslice);
   }
   ```

   **Timing matters:** writing the same field live over MMIO (0x1199 -> 0x299)
   after the encoder had started changed nothing; it has to be programmed before
   the first modeset, because the encoder latches it at start-up.

Working boot, from `fixed-state-reference/dmesg.txt`:

```
sky-m19:   mode 1080x2460@90 clock=276180 kHz
sky-phy:   vendor m19 timings, bitclk=671863152
sky-dscparams: v1.1 bpp=128 bpc=8 fl_bpg=12 ixd=170 dec=526 init_off=6144 final_off=7072 scale_inc=113 scale_dec=7 nfl=1294 chunk=540
sky-tsetup:   flags=0x2a03 dsc=set widebus=1
sky-dsi-dsc:  slice_per_intf=2 chunk=540 total_bytes=1080 bytes_per_pkt=1080 eol=0 pkt_per_line=1 slice_per_pkt=2
sky-dsiregs:  VID_COMP=04380b01 CMD_COMP=39003900 ... TOTAL=09e501ea ACT_H=01b00048 ACT_V=09b00014
sky-intf:     cfg2 wrote 00001011 read 00001011 (widebus=1 compression=1 width=360 dce_bytes=1080)
sky-panel:    vendor-long-write init; PPS sent, err=0
sky-panel:    dcs read id=3 [c0 11 00] pwr=1 [9c]
sky-dsc-buffer: max_addr=299 slices=2
Console:      switching to colour frame buffer device 135x153
```

User-visible result: console prints normally, no flicker, test pattern renders
correctly.

---

## 1. Panel identity

| Item | Value |
|---|---|
| Our DT compatible | `xiaomi,m19-36-02-0a-fhd-dsc` |
| Vendor DT panel-name | `xiaomi_m19_36_02_0a_fhd_dsc_video_dsi_panel` |
| Vendor DT node | `qcom,mdss_dsi_m19_36_02_0a_fhd_dsc_vid` |
| Xiaomi internal code | `m19` (`36_02_0a` = version/batch) |
| Service part number | **B16790013** (shared with Redmi 12 4G/5G, Redmi 13 4G/5G, Redmi Note 12R, POCO M6 Pro 5G) |
| Type | **IPS LCD** (not AMOLED), 6.79" |
| Resolution / refresh | 1080 x 2460, 90 Hz, 24 bpp |
| Density / size | 396 ppi; 69.33 x 157.92 mm (`width_mm = 69`, `height_mm = 158`) |
| Brightness / contrast | 550 cd/m^2, 1500:1 |
| DDIC identification (weak) | DCS read returns `c0 11 00`; init sequence uses Himax-style `0xFF` page + `0xFB` key writes, so likely a Himax HX83112/HX83121 class DDIC (unconfirmed) |

Panel init sequence: 163 commands, 1486 bytes, extracted from the vendor DT into
`panel-m19-init.json`. Command types: 161 x `0x39` (DCS long write), 2 x `0x05`.
Command 156 is the DSC parameter write:

```
39 01 00 00 00 00 11 c1 89 28 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16
```

The 16 payload bytes carry the DDIC's own copy of the DSC rate-control values
(see section 5) and are an independent confirmation of them.

---

## 2. Board wiring (from our DTB)

| Signal | Resource |
|---|---|
| Panel supplies | `vddio`, `extvdd`, `avdd`, `ibb` (all `regulator-boot-on; regulator-always-on;` during bring-up) |
| Reset | `reset-gpios = <&tlmm 92 GPIO_ACTIVE_LOW>` |
| DSI controller | `dsi@ae94000` (single DSI, 4 lanes) |
| DSI PHY | `qcom,sm4450-dsi-phy-5nm` -> uses `dsi_phy_5nm_8450_cfgs` |
| Backlight IC | **KTD3136** @ i2c1 `0x36`, `enable-gpios = <&tlmm 6 GPIO_ACTIVE_HIGH>` (HWEN) |
| Backlight driver | `drivers/video/backlight/ktd3136.c`, ported from the ginkgo project, `CONFIG_BACKLIGHT_KTD3136=m`, module `ktd3136.ko` |
| Backlight range | 0..2047, default 1024; live at `/sys/class/backlight/ktd3136-backlight/` |

Backlight is proven working: `probed id=0x18 brightness=1024`, brightness writes take
effect, and `msm_drm.ko` calls into `ktd3136_bl` (`ktd3136_bl_set_brightness`,
`ktd3136_reg_init`), i.e. the vendor keeps it as a separate module too.

---

## 3. Video timing (vendor authoritative, from the DTBO dump)

From `vendor-dtbo-panel-node.dts` (timing@0, the default topology index):

```
qcom,display-topology          = <1 1 1>
qcom,mdss-dsi-panel-framerate  = 90
qcom,mdss-dsi-h-front-porch    = 59      qcom,mdss-dsi-h-back-porch  = 60
qcom,mdss-dsi-h-pulse-width    = 12      qcom,mdss-dsi-v-front-porch = 54
qcom,mdss-dsi-v-back-porch     = 10      qcom,mdss-dsi-v-pulse-width = 10
qcom,mdss-dsi-panel-width      = 1080    qcom,mdss-dsi-panel-height  = 2460
qcom,mdss-dsi-bpp              = 24
qcom,mdss-dsi-panel-type       = "dsi_video_mode"
qcom,mdss-dsi-traffic-mode     = "burst_mode"
qcom,mdss-dsi-panel-phy-timings= [00 18 06 06 15 20 06 06 07 02 04 00 15 0b]
qcom,compression-mode          = "dsc"
qcom,mdss-dsc-block-prediction-enable;      /* present => ENABLED */
qcom,mdss-dsc-bit-per-pixel    = 8
qcom,mdss-dsc-bit-per-component= 8
qcom,mdss-dsc-slice-per-pkt    = 2
qcom,mdss-dsc-slice-width      = 540
qcom,mdss-dsc-slice-height     = 20
```

No `qcom,mdss-dsi-panel-clockrate` in this timing node (computed by the driver).
No `qcom,mdss-dsc-version` either -> defaults to **DSC 1.1**.

Note: `panel-m19-init.json` recorded `block-prediction-enable: false`; that was an
extraction bug. The DTBO property is valueless-but-present, i.e. **enabled**.

---

## 4. What the mainline path programs today (verified at runtime)

```
sky-m19:     dsc v1.1, slice 540x20, count 2, bpp 8, bpc 8, block_pred on
sky-dsc12:   slice=540x20 count=2 chunk=540 bpp=128 bpc=8 pic=1080x2460 mode=0x4 initial_lines=2
sky-dscparams: v1.1 fl_bpg=12 ixd=170 dec=526 init_off=6144 final_off=7072 scale_inc=113 scale_dec=7 nfl=1294 chunk=540
sky-tsetup:  flags=0x2a03 (VIDEO|BURST|VSYNC_FLUSH|LPM|DSC_ALL_SLICES_IN_PKT) dsc=set widebus=1
sky-dsi-dsc: slice_per_intf=2 chunk=540 total_bytes=1080 bytes_per_pkt=540 eol=0 pkt_per_line=2
sky-dsiregs: VID_COMP=021c0b41  -> datatype 0x0b, 2 pkt/line, eol 0, EN 1, WC=540
sky-intf:    INTF_CONFIG2=0x00001011 (DATABUS_WIDEN|DATA_HCTL_EN|DCE_DATA_COMPRESS)
sky-panel:   PPS sent err=0; DCS read id=[c0 11 00], power mode 0x9c
```

Full dump in `register-and-probe-log.txt`.

Two local kernel patches make this happen:

1. `drivers/gpu/drm/msm/dsi/dsi_host.c : dsi_populate_dsc_params()`
   after `drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR)`:
   ```c
   dsc->initial_xmit_delay = 170;   /* Xiaomi lut_v1 override, see section 5 */
   ```
   then `drm_dsc_compute_rc_parameters()` derives dec=526, final_offset=7072,
   scale_increment_interval=113 — all matching the vendor.
2. several `pr_info("sky-…")` probes (dscparams / tsetup / dsi-dsc / dsiregs,
   plus `dpu_encoder.c` and `dpu_hw_intf.c` and `dpu_hw_dsc_1_2.c` probes).

Panel driver: `panel-xiaomi-m19.c` (snapshot in this folder) — built as a module
(`CONFIG_DRM_PANEL_XIAOMI_M19=m`), auto-loaded from `/init` with an optional
`clock_khz=` module parameter (`/etc/sky-panel-clock`).

---

## 5. Vendor DSC truth (from `vendor-dsc-analysis.md`, double/triple verified)

Decoded from `msm_drm.ko` (ARM64, symbols intact) and cross-checked against the
panel's own `0xC1` init command.

* DSC **1.1**, 444, `scr_ver = 0`, bpp 8, bpc 8, block prediction **on**.
* `sde_dsc_index_map[0]` -> RC table index 0; that table's
  `rc_range_params` group 0 is **byte-identical** to mainline
  `rc_parameters_pre_scr[bpp8/bpc8]`, and `rc_buf_thresh` = `0e 1c 2a 38 46 54 62 69 70 77 79 7b 7d 7e`.
* **Xiaomi override:** `strstr(panel_name, "m19_36_02_0a")` sets `is_panel = 10`,
  which selects `sde_dsc_rc_init_param_lut_v1` instead of the standard LUT.
  The only difference is `initial_xmit_delay`: **170** instead of 512.
* Vendor final values:

  | field | value |
  |---|---|
  | first_line_bpg_offset | 12 |
  | initial_xmit_delay | **170** |
  | initial_dec_delay | 526 |
  | initial_scale_value | 32 |
  | scale_increment_interval | **113** |
  | scale_decrement_interval | 7 |
  | nfl_bpg_offset | 1294 |
  | slice_bpg_offset | 1302 |
  | initial_offset | 6144 |
  | final_offset | **7072** |
  | rc_model_size | 8192 |
  | rc_edge_factor | 6 |
  | rc_quant_incr_limit0/1 | 11 / 11 |
  | rc_tgt_offset hi/lo | 3 / 3 |
  | flatness min/max | 3 / 12 |
  | slice_chunk_size | 540 |
  | line_buf_depth | 9 |
  | convert_rgb | 1 |

* Consequence: the 128-byte PPS differs from stock mainline in exactly bytes
  16/17 (initial_xmit_delay), 22/23 (final_offset) and 34/35
  (scale_increment_interval). **Our driver now emits the vendor values.**
* Vendor DSI private params: `pkt_per_line = 2`, `chunk = 540`,
  `(chunk*pkt)%3 = 0`, `(chunk*pkt+2)/3 = 360`, `flatness_det_thresh = 2`.
  -> the DSI must send **one 540-byte packet per slice** (2 per line), i.e.
  `slice_per_pkt = 1`. Our current build does exactly that
  (`VID_COMP WC=540`, `pkt_per_line=2`).
* Vendor INTF programming matches mainline: `INTF_CONFIG2` (0x060)
  bit0 = widebus, bit12 = compressed input; `byte_intf_div = 2` unconditionally.

---

## 6. DRM/DPU side is NOT the problem (audited)

From `mainline-dsc-analysis.md`:

* fbdev buffers are linear by construction (`msm_fbdev.c:109`
  `DRM_FORMAT_MOD_LINEAR`; `mdp_format.c:619-651` is a hard switch on the modifier,
  so a linear buffer can never be scanned out as UBWC).
* `dpu_plane_format_mod_supported()` accepts LINEAR unconditionally.
* The DPU holds no private DSC config: `dpu_encoder_get_dsc_config()` returns
  `msm_host->dsc`, the same struct the panel serialises into the PPS.
* Decisive: the artifact structure changes with `slice_width` / `slice_count` /
  `slice_per_pkt`, which no plane/format/tiling bug can do.
* Ordering is safe: `dsi_populate_dsc_params()` runs in DSI bridge pre_enable,
  before the PPS is packed and before `dce_bytes_per_line` is read.
* `bits_per_pixel = 128` (8 << 4) is the correct encoding.
* DPU and DSI share one widebus predicate, so they cannot disagree.

---

## 7. Experiments already run (all without a clean picture)

| # | Change | Result |
|---|---|---|
| 1 | DSC off, pclk 282.4 MHz (vendor uncompressed rate) | vertical stripes |
| 2 | DSC on, slice 540x20 x2, slice_per_pkt=1, 671.9 Mbps | horizontal noise, left/right split at x=540 |
| 3 | all slices in one packet (slice_per_pkt=2), 671.9 Mbps | no change |
| 4 | vendor rate 706.2 Mbps (clock 290300) | artefact changed, still noise |
| 5 | single slice 1080x20, count 1 | artefact changed to vertical banding |
| 6 | DSC 1.2 + `DRM_DSC_1_2_444` table + fl_bpg 13 | still noise (and msm later proved to force its own table) |
| 7 | `initial_lines` forced to 20 | worse, reverted |
| 8 | vendor PPS (ixd 170 etc.) + DSC 1.1 | still noise |
| 9 | `pkt_per_line = 2` (WC 540, vendor private param) | still noise (current build) |

Test pattern used throughout: `make-test-pattern.py` (red/green/blue/white
quadrants, white border, centre line). Best artefact description so far, from
frame analysis of the user's videos:

* with slice 540x20 x2 / pkt 1: **left/right split exactly at x = 540** (the slice
  boundary), left half with black horizontal bands, right half smoother noise;
* with a single 1080-wide slice: full-width vertical banding;
* with pkt_per_line = 2: uniform horizontal colour bands.

The top few rows of the pattern were correct in the earliest (pre-DSC) attempts,
which is why "the first bytes of each line are right, then it drifts" is the
working hypothesis.

---

## 8. Hypotheses from the pre-fix era (all resolved by section 0)

Kept for the record; the actual cause was the DSC rate-buffer size (section 0,
item 5). Hypotheses 1-4 turned out to be contributors or red herrings:

* vendor DDIC command sets: not needed — the DT sequence was right, but it must be
  sent as generic long writes (section 0 item 4);
* line time / `h_total`: mainline's values were fine;
* `initial_lines`: fine at 2 once the rate buffer was correct;
* PHY timing bucket: mattered, so we now force the vendor's 14-byte timings;
* EOT: not required.

1. **The DDIC never enters DSC decode mode.** All our compares are host-side; the
   panel only tells us `power mode 0x9c`. Missing possibility: a DDIC-side
   "compression on" page write that the DT sequence does not contain because
   downstream sends it from `dsi_panel.c` (e.g. a `TX_DSC_ENABLE` command set)
   rather than from DT. Next step: dump the vendor's command sets from
   `msm_drm.ko` (`dsi_panel_tx_cmd_set`, `panel_cmd_set` tables) and diff against
   the 163 DT commands we send.
2. **DSI/DPU line-time mismatch in DSC mode.** `TOTAL` reads `h_total = 490`,
   `v_total = 2533`. Verify how the vendor computes h_total for a compressed
   line (their `sde_encoder_phys_vid` builds `cfg[12]/cfg[16]`, which the
   subagent could not fully trace). If the line time is wrong the DDIC's DSC
   rate buffer starves every line — matching "start of line correct, then drift".
3. **`initial_lines = 2` is too small for this DDIC** even though mainline's
   formula says 2. Retest with the *vendor-correct PPS* in place (experiment 7
   was run while the 1.2 table was active, so it is not conclusive).
4. **PHY timing bucket**: mainline derives PHY timings from the computed bit
   clock; the vendor DT gives explicit bytes
   `[00 18 06 06 15 20 06 06 07 02 04 00 15 0b]` (t_clk_pre 0x15, t_clk_post 0x0b).
   If our computed bit clock falls in another bucket the link is marginal. Try
   forcing the rate so the PHY lands on the vendor bucket, or patch the PHY
   timings.
5. **EOT suppression**: `MIPI_DSI_MODE_NO_EOT_PACKET` keeps TX_EOT_APPEND clear, so
   compressed packets carry no EOT. Candidate A/B test: drop that flag.

---

## 9. How to iterate (fast paths)

Device access: `sshpass -p sky ssh root@192.168.7.1` (USB NCM, host profile
`sky-usb`, static 192.168.7.20/24). sudo password on the host: `m912n67oo`.

Panel driver is a **module on the UFS rootfs**, so most experiments need no flashing:

```bash
cd ~/redmi125g/sky-mainline/kernel/min-tree
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -j10 modules                                    # ~45 s
scp drivers/gpu/drm/panel/panel-xiaomi-m19.ko \
    root@192.168.7.1:/lib/modules/$(uname -r)/extra/  # note: use the real version string
ssh root@192.168.7.1 "sync; echo b > /proc/sysrq-trigger"   # reboot applies new mode
```

The module auto-loads from `/init`:
`insmod /lib/modules/$(uname -r)/extra/panel-xiaomi-m19.ko clock_khz=$(cat /etc/sky-panel-clock)`.
A DSI rate sweep only needs `echo <kHz> > /etc/sky-panel-clock` + reboot.

Kernel-side changes (dsi_host / dpu / DTB) need:

```bash
make -j10 Image
cd ../../shim
python3 tools/mklinuxpayload.py ../kernel/min-tree/arch/arm64/boot/Image \
        ../build/sm4450-xiaomi-sky.dtb out/payload.bin --load 0xc0000000
./build-sky.sh out/payload.bin            # -> out/sky-shim-boot-v4.img
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
```

Test pattern:

```bash
python3 docs/panel-handoff/make-test-pattern.py     # -> pattern.rgb
scp pattern.rgb root@192.168.7.1:/tmp/
ssh root@192.168.7.1 "dd if=/tmp/pattern.rgb of=/dev/fb0 bs=4096"
```

Recovery: only `boot_a` is ever written; stock boot image for fastboot recovery is
`~/redmi125g/sky_in_global_images_V14.0.7.0.TMWINXM_13.0/images/boot.img`.
If a bad panel module crashes the panel probe, boot TWRP and delete
`/data/skyroot/lib/modules/*/extra/panel-xiaomi-m19.ko`.

Useful current images in `shim/out/`:
`sky-m6n-dsi-regs.img` (register probes), `sky-m6m-vendor-pps.img`,
`sky-m6j-dsc11-vendor.img`.

---

## 10. File inventory in this folder

| File | Contents |
|---|---|
| `HANDOFF.md` | this document |
| `NEXT-PROMPT.md` | self-contained prompt for a fresh session |
| `fixed-state-reference/` | text copies of the working snapshot: `dmesg.txt`, `dpu_hw_dsc_1_2.c`, `startup-buffer.patch`, `README.md`, `SHA256SUMS` (binaries stay in `build/twrp-dsc-buffer/improved-state/`) |
| `vendor-dsc-analysis.md` | 59 KB reverse-engineering report of `msm_drm.ko` (DSC tables, PPS, INTF, evidence appendix) |
| `mainline-dsc-analysis.md` | 31 KB audit of the mainline DSI/DPU path with file:line evidence |
| `vendor-dtbo-panel-node.dts` | the vendor panel node copied out of the Ravelin DTBO dump |
| `panel-m19-init.json` | the 163 command init sequence + DSC properties as parsed |
| `panel-xiaomi-m19.c.snapshot` | the panel driver as of the handoff |
| `register-and-probe-log.txt` | runtime register values and probe output |
| `make-test-pattern.py` | generator for the 1080x2460 BGRA test pattern |
| `*-followup.md` | the debugging notes written while chasing the corruption: vendor command sets, line timing, initial-lines, PHY timing, packetization, DDIC readback, slice boundary, lane path, vendor DSI video registers, TWRP vendor buffer analysis |

Patches in the repository: `kernel/patches/0001-*` (TLMM protected GPIOs),
`0002-*` (UFS `qcom,sm4450-ufshc`), `0003-dpu-dsc-sm4450-output-buffer-size.patch`
(the display fix), `patches-debug/*` (probes).

Reference material outside this folder: `docs/D-panel-m19-init.md`,
`build/twrp-dsc-buffer/improved-state/` (full working artefacts), and the vendor
DTBO dump (re-downloadable from `Jiovanni-dump/redmi_river_dump`, path
`dtbo/dts/29_dtbdump_Qualcomm_Technologies%2C_Inc._Ravelin_RUMI.dts`).
