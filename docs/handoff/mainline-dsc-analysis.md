# Mainline MIPI-DSI + DSC path audit — Xiaomi "m19" panel (SM4450 / DPU 8.6)

Tree: `/home/orange/redmi125g/sky-mainline/kernel/min-tree` (Linux 7.3 + ROCKNIX SM4450 +
local "sky" bring-up patches). Panel driver: `drivers/gpu/drm/panel/panel-xiaomi-m19.c`.
**Analysis only — no kernel source was modified.** Every claim below was read out of the tree;
file:line references are to that tree.

---

## 0. Executive summary

1. The DSC *configuration plumbing* in mainline is self-consistent for this panel: the same
   `struct drm_dsc_config` object is written by the DSI host, read by the PPS builder, and read by
   the DPU DSC block, and — verified below — the DSI populates it **before** both the DPU programming
   and the PPS transmission. So "PPS says A, hardware does B" is *not* happening.
2. Three real deviations from what the vendor configuration asks for **are** present in the tree,
   and all three are in the DSC/DSI-packing half of the path, not in DRM/KMS:
   * `dsc_slice_per_pkt` = 1 at runtime, vendor says 2 (packetisation of each compressed line).
   * the rate-control parameter set that reaches both the PPS and the DPU is mainline's
     `DRM_DSC_1_2_444` table, **not** the table the driver author transcribed from the vendor
     (which is byte-identical to mainline's DSC **1.1** `rc_parameters_pre_scr`), and the
     intended `first_line_bpg_offset = 13` is silently discarded.
   * `block_pred_enable` is set to `true` while the extracted vendor DT says `false`.
3. The DRM/DPU plane side is **not** implicated: fbdev/dumb buffers are linear, the DPU's format
   lookup maps modifier 0 to the linear format table only, so UBWC can never be programmed for
   them; and the observed artifact varies with DSC slice geometry, which no plane-side bug can do.

---

## 1. End-to-end trace of the compression path

### 1.1 Boot / attach

| Step | Code | What it does |
|---|---|---|
| panel probe | `panel-xiaomi-m19.c:434-450` | `memcpy` static `m19_dsc`, `drm_dsc_setup_rc_params(&ctx->dsc, DRM_DSC_1_2_444)` (435), override `first_line_bpg_offset = 13` (439), `dsi->lanes = 4` (445), `dsi->format = RGB888` (446), `dsi->mode_flags = VIDEO|VIDEO_BURST|NO_EOT_PACKET|LPM|DSC_ALL_SLICES_IN_PKT` (447-449), `dsi->dsc = &ctx->dsc` (450) |
| attach | `dsi_host.c:1727-1775` | `msm_host->mode_flags = dsi->mode_flags` (1739); if `dsi->dsc` (1740): `msm_host->dsc = dsi->dsc` (1741) and **`dsc_slice_per_pkt = slice_count` iff `mode_flags & MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT`, else 1** (1742-1745) |

`MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT` is `BIT(13)` (`include/drm/drm_mipi_dsi.h:142`).

### 1.2 Modeset-enable ordering (this is what makes the PPS trustworthy)

Linux 7.3 `drm_atomic_helper_commit_modeset_enables()` (`drm_atomic_helper.c:1766-1772`) runs, in order:

1. `drm_atomic_helper_commit_crtc_enable()` (`:1666`) → `dpu_crtc_enable()` (`dpu_crtc.c:1269`) — only
   vblank/bandwidth bookkeeping; it does **not** program INTF timing.
2. `drm_atomic_helper_commit_encoder_bridge_pre_enable()` (`:1624`) → bridge chain from the encoder:
   `dsi_mgr_bridge_pre_enable()` (`dsi/dsi_manager.c:278`) → `dsi_mgr_bridge_power_on()` (`:294`) →
   `msm_dsi_host_power_on()` (`dsi_host.c:2516`) → **`dsi_timing_setup()` (2559)** →
   **`dsi_ctrl_enable()` (2561)**.
   The panel bridge sits *behind* the DSI bridge in the chain (`dsi_manager.c:439-447` attaches
   `msm_dsi->next_bridge` with the DSI bridge as `previous`), and it sets
   `prepare_prev_first = true` (`panel-xiaomi-m19.c:452`), which makes
   `drm_atomic_bridge_chain_pre_enable()` (`drm_bridge.c:992-1032`) run the panel's **prepare**
   (regulators only, `panel:302-318`) before the DSI bridge's pre_enable. The panel's **enable**
   (which is what sends the PPS) is still the last step of the whole chain.
3. `drm_atomic_helper_commit_encoder_bridge_enable()` (`:1706`): **encoder `atomic_enable` first**
   (`:1736-1743` → `dpu_encoder_virt_atomic_enable`, `dpu_encoder.c:1339` → `phys->ops.enable` →
   `dpu_encoder_phys_vid_setup_timing_engine`, `dpu_encoder_phys_vid.c:257/299`), **then**
   `drm_atomic_bridge_chain_enable()` (`:1745`) → panel bridge enable → `m19_enable()` →
   `m19_on()` → PPS (`panel-xiaomi-m19.c:293-294`).

Consequences (all verified, they matter for the ranking below):

* `dsi_populate_dsc_params()` (step 2) runs **before** the PPS is packed (step 3) and **before**
  `dce_bytes_per_line` is read for the INTF (`dpu_encoder_phys_vid.c:140`, step 3). So
  `slice_chunk_size`, `initial_scale_value`, `final_offset`, `nfl/slice_bpg_offset`,
  `scale_inc/dec_interval`, `rc_bits`, `initial_dec_delay` **are** valid in both places.
  → *A "panel sent a zeroed PPS" bug does not exist here.*
* DPU DSC register programming happens even later, at `dpu_encoder_prepare_for_kickoff()`
  (`dpu_encoder.c:2110-2111`) → `dpu_encoder_prep_dsc()`.

### 1.3 `dsi_timing_setup()` — `dsi_host.c:1007-1144`

* `dsc->pic_width/height` forced from the mode (1052-1053).
* `dsi_populate_dsc_params()` (1059) — see 1.5.
* `h_total -= hdisplay` (1079); with widebus + `MIPI_DSI_MODE_VIDEO`:
  `bits_per_pclk = bpc * 3` (1080-1082, i.e. 24, *not* 48), else 24 (1086).
* `hdisplay = DIV_ROUND_UP(msm_dsc_get_bytes_per_line(dsc) * 8, bits_per_pclk)` (1089) =
  `DIV_ROUND_UP(1080*8, 24)` = **360**; `ha_end = ha_start + hdisplay` (1092).
* Video branch (1099-1119): `dsi_update_dsc_timing(msm_host, false)` (1100-1101), then
  `REG_DSI_ACTIVE_H/V`, `REG_DSI_TOTAL`, `REG_DSI_ACTIVE_HSYNC`, `REG_DSI_ACTIVE_VSYNC_*`.
* Clock side: `dsi_adjust_pclk_for_compression()` (`:603-629`) →
  `pclk = mult_frac(clock*1000, htotal-hdisplay+new_hdisplay, htotal)`; `dsi_byte_clk_get_rate()`
  (`:653-674`) → `byteclk = pclk*bpp/(8*lanes)`.
  Check against the runtime log: `276180000 * 491 / 1211 = 111 977 192` ✔, `*24/32 = 83 982 894` ✔,
  `*8 = 671 863 152` ✔.

### 1.4 `dsi_update_dsc_timing()` — `dsi_host.c:948-1005`

```
slice_per_intf        = dsc->slice_count                       (960)
total_bytes_per_intf  = slice_chunk_size * slice_per_intf      (962)
bytes_per_pkt         = slice_chunk_size * dsc_slice_per_pkt   (963)
eol_byte_num          = total_bytes_per_intf % 3               (965)
pkt_per_line          = slice_per_intf / dsc_slice_per_pkt     (966)
```
Video mode register `REG_DSI_VIDEO_COMPRESSION_MODE_CTRL` (0x29c) gets (972-1003):

| Field | Value | Source line |
|---|---|---|
| `DATATYPE` [15:8] | `MIPI_DSI_COMPRESSED_PIXEL_STREAM` = 0x0b | 975 |
| `PKT_PER_LINE` [7:6] | `pkt_per_line >> 1` (log2 encoding: 1→0, 2→1, 4→2) | 985 |
| `EOL_BYTE_NUM` [5:4] | `total_bytes_per_intf % 3` | 986 |
| `EN` [0] | 1 | 987 |
| `WC` [31:16] (16 bits) | **`bytes_per_pkt`** | 1002 |

Register field definitions: `drivers/gpu/drm/msm/generated/dsi.xml.h:735-760`.
Command mode instead writes `REG_DSI_COMMAND_COMPRESSION_MODE_CTRL` + `..._CTRL2.STREAM0_SLICE_WIDTH =
slice_chunk_size` (997) and `WC = slice_chunk_size * dsc_slice_per_pkt + 1` (1131).

### 1.5 `dsi_populate_dsc_params()` — `dsi_host.c:1893-1950`

| Line | Action |
|---|---|
| 1897-1900 | reject `bits_per_pixel & 0xf` (i.e. 4.4 fixed point required) |
| 1919-1921 | `simple_422 = 0`, **`convert_rgb = 1`**, `vbr_enable = 0` |
| 1923-1924 | `drm_dsc_set_const_params()` (`drm_dsc_helper.c:282-295`: rc_model_size 8192, edge factor, tgt offsets, mux word 48) and `drm_dsc_set_rc_buf_thresh()` (`:309-330`) |
| **1932-1935** | **local "sky:" patch**: `drm_dsc_setup_rc_params(DRM_DSC_1_2_444)` if `dsc_version_minor >= 2 && !420 && !422`, else `DRM_DSC_1_1_PRE_SCR` |
| 1946 | `initial_scale_value = drm_dsc_initial_scale_value(dsc)` = `8*8192/(8192-initial_offset)` = 32 |
| 1947 | `line_buf_depth = bits_per_component + 1` = 9 |
| 1949 | `drm_dsc_compute_rc_parameters()` (`drm_dsc_helper.c:1313-1439`) |

`drm_dsc_compute_rc_parameters()` is the function that computes `slice_chunk_size`
(`drm_dsc_helper.c:1338-1340`: `DIV_ROUND_UP(slice_width * bpp, 8*16)` = 540 for our numbers), plus
`nfl_bpg_offset` (1389), `slice_bpg_offset` (1398), `scale_inc/dec_interval` (1366-1370, 1410-1421),
`final_offset` (1372), `rc_bits`/`initial_dec_delay` (1433-1435).
**`drm_dsc_setup_rc_params()` alone does *not* set `slice_chunk_size`** — it only fills
`first_line_bpg_offset`, `initial_xmit_delay`, `initial_offset`, flatness/quant limits and the 15
`rc_range_params` (`drm_dsc_helper.c:1277-1297`). The panel driver's own
`drm_dsc_setup_rc_params()` call at `panel-xiaomi-m19.c:435` therefore produces a config with
`slice_chunk_size == 0`; it only works because the DSI host re-derives everything at power-on.

### 1.6 DPU side

* Topology: catalog has **one** DSC block (`catalog/dpu_8_6_sm4450.h:86-92`, `DSC_0` @0x80000,
  `dsc_count = 1`, mdss ver 8.6) → `dpu_encoder_setup_display()` picks `num_dsc = 1`
  (`dpu_encoder.c:692-695`). Ops binding: `dpu_rm.c:171-174` selects the **1.2** driver because
  `core_major_ver >= 7` → `dpu_hw_dsc_init_1_2()` (`dpu_hw_dsc_1_2.c:384`).
* `dpu_encoder_prep_dsc()` (`dpu_encoder.c:2019-2066`):
  `dsc_common_mode` = `SPLIT_PANEL` if `num_dsc > 1` (2050-2051), `MULTIPLEX` if
  `dpu_encoder_use_dsc_merge()` (2052-2053), `VIDEO` if `intf_mode == INTF_MODE_VIDEO` (2054-2055).
  With `dsc_count == 1`: **`mode = DSC_MODE_VIDEO` (0x4)** — matches the runtime `mode=0x4`.
  `this_frame_slices = pic_width / slice_width` = 2 (2057), `intf_ip_w` = 1080, `enc_ip_w = 1080/1`
  (2058-2060).
* `initial_lines = dpu_encoder_dsc_initial_line_calc(dsc, 1080)` (`dpu_encoder.c:1968-1991`):
  `ssm_delay = 84` (8bpc), `total = 84*3 + 512 + 47 = 811`, `soft_slice_per_enc = 2 > 1` → `+252`
  = 1063, `DIV_ROUND_UP(1063, 540)` = **2** — matches `initial_lines=2`.
  It depends on `bpc` and `initial_xmit_delay` only.
* `dpu_hw_dsc_config_1_2()` (`dpu_hw_dsc_1_2.c:87-245`) register map:

| Register (offset) | Content | Line |
|---|---|---|
| `DSC_CMN_MAIN_CNF` (0x00) | `SPLIT_PANEL`→BIT0, `MULTIPLEX`→BIT1, `num_active_slice_per_enc<<7` (=`slice_count`, or `/2` under multiplex) | 106-124 |
| `ENC_DF_CTRL` (0x00) | `initial_lines & 0xff`, `BIT(9)` if `DSC_MODE_VIDEO`, `(2400/nslice-1)<<18` | 126-133 |
| `DSC_MAIN_CONF` (0x30) | `dsc_version_minor<<28`, native_422 BIT22 / native_420 BIT21, **`bits_per_pixel<<10`**, `block_pred_enable`→BIT20, `convert_rgb`→BIT4, `line_buf_depth<<6`, `bits_per_component` [3:0] | 135-161 |
| `DSC_PICTURE_SIZE` (0x34) | `pic_width | pic_height<<16` | 163-166 |
| `DSC_SLICE_SIZE` (0x38) | `slice_width | slice_height<<16` | 168-171 |
| **`DSC_MISC_SIZE` (0x3C)** | **`slice_chunk_size`** | 173-174 |
| `DSC_HRD_DELAYS` (0x40) | `initial_xmit_delay | initial_dec_delay<<16` | 176-179 |
| `DSC_RC_SCALE` (0x44) | `initial_scale_value & 0x3f` | 181-182 |
| `DSC_RC_SCALE_INC_DEC` (0x48) | `scale_increment_interval | scale_decrement_interval<<16` | 184-187 |
| `DSC_RC_OFFSETS_1` (0x4C) | `first_line_bpg_offset | second_line_bpg_offset<<5` | 189-192 |
| `DSC_RC_OFFSETS_2` (0x50) | `nfl_bpg_offset | slice_bpg_offset<<16` | 194-197 |
| `DSC_RC_OFFSETS_3` (0x54) | `initial_offset | final_offset<<16` | 199-202 |
| `DSC_RC_OFFSETS_4` (0x58) | `nsl_bpg_offset | second_line_offset_adj<<16` — **never set by anyone in msm → 0** | 204-207 |
| `DSC_FLATNESS_QP` (0x5C) | flatness min/max QP + `drm_dsc_flatness_det_thresh()` | 209-214 |
| `DSC_RC_MODEL_SIZE` (0x60), `DSC_RC_CONFIG` (0x64), `DSC_RC_BUF_THRESH_0..3`, `DSC_RC_MIN/MAX_QP_*`, `DSC_RC_RANGE_BPG_OFFSETS_*` | RC table from `dsc->rc_range_params[15]` / `rc_buf_thresh[14]` | 216-350 |
| `DSC_CFG` (ctl+0x04) | encoder enable BIT0, native bits, `!convert_rgb`→BIT10, `bpc==8`→BIT11, split/panel bits, `!(VIDEO)`→BIT17 (cmd mode) | 227-244 |
| `DSC_CTL` (ctl+0x00) | pingpong binding | 352-367 |

* INTF side (`dpu_hw_intf.c`): `INTF_CFG2_DATABUS_WIDEN` if `wide_bus_en` (170-171);
  `dce_bytes_per_line = msm_dsc_get_bytes_per_line(dsc)` (`dpu_encoder_phys_vid.c:140`, =1080);
  `data_width = DIV_ROUND_UP(dce_bytes_per_line, 6)` when widebus (191-195) = 180;
  `INTF_CFG2_DCE_DATA_COMPRESS` when `compression_en && core_major_ver >= 7` (201-203, SM4450 = 8);
  `INTF_CFG2_DATA_HCTL_EN` unless `compression_en && !wide_bus_en` (260-267).
  `timing->width` is *already* scaled by `bpp_int/(bpc*3)` before this:
  `dpu_encoder_phys_vid.c:130-141` → 360.

### 1.7 Widebus: one source of truth

`msm_dsi_host_is_wide_bus_enabled()` (`dsi_host.c:818-825`) = `msm_host->dsc != NULL &&
DSI >= 6G v2.5.0`. **Both** sides call it:
* DSI: `DSI_VID_CFG0_DATABUS_WIDEN` (`dsi_host.c:852-853`), and `bits_per_pclk` (1022, 1080-1087);
* DPU: `dpu_encoder_is_widebus_enabled()` (`dpu_encoder.c:275-291`) → `msm_dsi_wide_bus_enabled()`
  (`dsi/dsi.c:20-23`) → same function → `timing->wide_bus_en`
  (`dpu_encoder_phys_vid.c:98`) → `INTF_CFG2_DATABUS_WIDEN`.
  ⇒ **The DPU and DSI cannot disagree about widebus in this configuration.** There is no
  "SSPP decides widebus" code path at all; the SSPP has no widebus concept (`dpu_plane.c` /
  `dpu_hw_sspp.c` contain no widebus handling), the bus width is an INTF property.

---

## 2. Configuration agreement matrix

Legend: **PD** = panel driver, **DSI** = `dsi_host.c`, **DPU** = `dpu_hw_dsc_1_2.c`.
"runtime" = can it be confirmed from a running system?

| # | Value | PD (`panel-xiaomi-m19.c`) | DSI | DPU | Effective value in this tree | runtime? |
|---|---|---|---|---|---|---|
| 1 | `dsc_version_major/minor` | 1/2 (58-59) | → PPS (`drm_dsc_helper.c:111-113`) | `minor<<28` (135) | 1.2 | `sky-dsc12` log |
| 2 | `bits_per_component` | 8 (63) | PPS pps_3 (`helper:118-120`) | MAIN_CONF[3:0] (159), CFG BIT11 (235) | 8 | yes (log) |
| 3 | `bits_per_pixel` (4.4 fx) | `8<<4` = 128 (64) | `&0xf` check (1897), `get_bpp_int()`=8 (`helper:1447`) | raw `<<10` (143,150) → 128 | 128 / 8 bpp | yes (log `bpp=128`) |
| 4 | `pic_width/height` | 1080/2460 (71-72) — **overwritten by DSI** | `mode->hdisplay/vdisplay` (1052-1053) | PICTURE_SIZE (163-166) | 1080x2460 | yes |
| 5 | `slice_width/height` | 540/20 (60-61) | check_dsc `%` only (2641-2651) | SLICE_SIZE (168-171) | 540x20 | yes |
| 6 | `slice_count` | 2 (62) | `slice_per_intf` (960), `num_active_slice_per_enc` | CMN_MAIN_CNF[8:7] (122-124) | 2 | yes |
| 7 | **`slice_chunk_size`** | **0 at probe** (435 only fills RC) | computed at 1949 → 540 (`helper:1338`) | **DSC_MISC_SIZE (173)**, PPS `chunk_size` (`helper:155`) | 540 | yes (`chunk=540`) |
| 8 | **`dsc_slice_per_pkt`** | flag BIT13 in mode_flags (449) | `dsi_host.c:1742-1745` | — | **1 at runtime** (log) vs vendor **2** | yes → **mismatch, see bug A** |
| 9 | `pkt_per_line` / `WC` | n/a | 985 / 1002 → 2 / 540 | — | 2 pkts of 540 B per line | yes (log) |
| 10 | `eol_byte_num` | n/a | 965 → 1080%3 = 0 | — | 0 | yes (log) |
| 11 | `initial_lines` | n/a | from `bpc`,`initial_xmit_delay` | ENC_DF_CTRL[7:0] | 2 | yes (log) |
| 12 | `initial_xmit_delay` | 512 (81) — **dead** | 1278 → 512 | HRD_DELAYS (176) | 512 | `sky-dscparams` log |
| 13 | `initial_offset` | 6144 (82) — **dead** | 1279 → 6144 | OFFSETS_3 (199) | 6144 | `sky-dscparams` |
| 14 | **`first_line_bpg_offset`** | 12 (80) then **13** (439) | 1277 **rewrites it to 12** | OFFSETS_1 (189) | **12** (13 is discarded) | `sky-dscparams` log → **bug B** |
| 15 | **`rc_range_params[15]`** | vendor table (85-101) — **dead** | 1285-1297 rewrites from `DRM_DSC_1_2_444` | RANGE_BPG_OFFSETS_0..2 (306-349), MIN/MAX_QP | 1.2 table | **differs from vendor → bug B** |
| 16 | `rc_buf_thresh[14]` | 14..126 (73) — dead | 1924 → spec table | BUF_THRESH_0..3 | spec | indirect |
| 17 | `initial_scale_value` | 0 | 1946 → 32 | RC_SCALE (181) | 32 | `sky-dscparams`-adjacent |
| 18 | `line_buf_depth` | 9 (79) | 1947 → 9 | MAIN_CONF[9:6] (158) | 9 | — |
| 19 | `convert_rgb` | 0 (69) | 1920 forces **1** | MAIN_CONF BIT4 (155) | 1 | — |
| 20 | `mux_word_size` | 48 (78) | 1923 const params | used in compute only (1352-1359) | 48 | — |
| 21 | **`block_pred_enable`** | **true** (65) | PPS pps_4 (`helper:123-130`) | MAIN_CONF BIT20 (152) | 1 | → **conflict with vendor DT, bug D** |
| 22 | `second_line_bpg_offset` / `nsl_bpg_offset` / `second_line_offset_adj` | unset | unset | OFFSETS_1/4 → 0 | 0 | correct for 444 |
| 23 | `wide_bus` | (implicit via dsc) | `is_wide_bus_enabled()` (818) | `dpu_encoder_is_widebus_enabled()` | 1 both sides | yes (`widebus=1`) |
| 24 | traffic mode | VIDEO_BURST (447) | BURST via `dsi_get_traffic_mode()` (769-776) | `INTF_MODE_VIDEO` → `DSC_MODE_VIDEO` | burst / video | yes (`mode=0x4`) |
| 25 | EOT packets | `NO_EOT_PACKET` (448) | BIT not set → no EOT append (911-913) | — | no EOT | → **bug C** |
| 26 | clock lanes | — | continuous HS clock since `CLOCK_NON_CONTINUOUS` unset (930-938) | — | continuous | — |

**Dead code warning:** rows 12-16 mean the whole static table in `panel-xiaomi-m19.c:57-102`
(with the exception of geometry rows 1-6 and 21) has *no effect*. Whatever was transcribed from the
vendor DT is replaced at every power-on by `dsi_populate_dsc_params()`.

---

## 3. Ranked suspected bugs

### Bug A — per-line packetisation (`slice_per_pkt`) does not match the vendor config  [confidence: high]

**Vendor:** `docs/panel-m19-init.json` → `qcom,mdss-dsc-slice-per-pkt: [2]`, `slice-count = 1080/540 = 2`
⇒ one compressed packet of 1080 B per line.

**Mainline expectation:** `dsi_host.c:1740-1746` — `dsc_slice_per_pkt = slice_count` **only** if
`MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT` (`BIT(13)`) is present in `dsi->mode_flags` *at attach time*,
otherwise 1. `dsi_update_dsc_timing()` then programs
`WC = chunk*slice_per_pkt`, `PKT_PER_LINE = slice_count/slice_per_pkt` (963, 966, 985, 1002).

**What is actually happening — provable from the log arithmetic:**
`bytes_per_pkt=540` and `pkt_per_line=2` ⟹ `dsc_slice_per_pkt == 1`.
Independently, `sky-tsetup: flags=0xa03` = `BIT(0)|BIT(1)|BIT(9)|BIT(11)` — **`BIT(13)` is not set**,
although `panel-xiaomi-m19.c:449` sets it. So the DSI is emitting **two 540-byte packets per line**
where the DDIC is configured for **one 1080-byte packet per line**.

**Why it produces the observed artifact:** the DDIC's slice de-framer is configured by the vendor
init sequence (the 163 vendor DCS writes in `docs/panel-m19-init.json` program the panel's own DSC
registers, page 0x23 regs 0x00-0x2b, 0x30-0x54, 0x58-0x7x). When the host's packet boundaries stop
matching the decoder's slice boundaries, every line is re-assembled from the wrong byte offsets —
per-line, periodic corruption (horizontal bands), no DSI protocol error, no underrun. Changing
`slice_width`/`slice_count`/`slice_per_pkt` changes the offsets → artifact structure changes, exactly
as reported.

**Minimal patch to test** (diagnostic, `dsi_host.c`):
```c
 /* dsi_host.c:1740-1746 */
-		if (dsi->mode_flags & MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT)
-			msm_host->dsc_slice_per_pkt = dsi->dsc->slice_count;
-		else
-			msm_host->dsc_slice_per_pkt = 1;
+		msm_host->dsc_slice_per_pkt = dsi->dsc->slice_count;   /* m19: slice-per-pkt = 2 */
```
Expected log after the change: `bytes_per_pkt=1080 pkt_per_line=1`; `PKT_PER_LINE` field becomes 0,
`WC` becomes 1080. If the picture changes at all, this is on the critical path. (Do the reverse test
too — force 1 — to confirm which of the two the DDIC wants.)

*Secondary action:* first confirm the running kernel really contains
`MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT` — see §5 (the runtime logs do not match the current sources).

---

### Bug B — encoder rate-control model ≠ the vendor's (RC table + `first_line_bpg_offset`)  [confidence: medium]

**Facts:**
* The table the driver author transcribed into the panel (`panel-xiaomi-m19.c:85-101`) is
  **byte-identical** to mainline's `rc_parameters_pre_scr` (DSC **1.1**), `drm_dsc_helper.c:369-377`.
* The local "sky:" patch at `dsi_host.c:1926-1939` deliberately selects `DRM_DSC_1_2_444`
  (`drm_dsc_helper.c:561-571`), which differs from that table in **5 of the 15 ranges**
  (`{5,11,-10}/{5,12,-12}/{5,13,-12}/{7,13,-12}/{13,15,-12}` vs
  `{5,10,-10}/{5,11,-12}/{5,11,-12}/{9,12,-12}/{12,13,-12}`), and it resets
  `first_line_bpg_offset` to 12 (`:1277`), silently discarding the `= 13` set at
  `panel-xiaomi-m19.c:439`.
* Both the PPS and the DPU block are programmed from the post-`populate` struct, so they agree with
  each other; the divergence is with the **vendor's** table.
* `first_line_bpg_offset` feeds `nfl_bpg_offset = first_line_bpg_offset<<11 / (slice_height-1)`
  (`helper:1389`) → 1294 instead of 1402 → `DSC_RC_OFFSETS_2[31:16]`.

**Why it could produce banding:** if the DDIC's decoder uses its *own* RC table (the DSC registers the
vendor init sequence writes) rather than the PPS, then encoder and decoder rate-control models
differ; the decoder's buffer model drifts inside every slice and is re-synchronised at the slice
boundary → periodic banding with a 20-line period. If instead the DDIC fully honours the PPS RC
fields, this bug is harmless. This is the single most important thing to settle (§4).

**Minimal patch to test** (`dsi_host.c`, at the end of `dsi_populate_dsc_params()`, after 1935):
```c
+	/* vendor DDIC table (m19) == mainline's DSC 1.1 pre-SCR table */
+	drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
+	dsc->first_line_bpg_offset = 13;
 	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
```
i.e. effectively *revert the "sky:" 1.2-table patch* and reinstate `fl_bpg = 13`. This changes
`nfl_bpg_offset` (1294 → 1402), `slice_bpg_offset`, `scale_increment_interval` and 5 range entries,
in both the PPS and the DPU registers.

---

### Bug C — EOT packets suppressed on the compressed stream  [confidence: medium-low, cheap to test]

`panel-xiaomi-m19.c:448` sets `MIPI_DSI_MODE_NO_EOT_PACKET`; `dsi_host.c:910-913` then leaves
`DSI_EOT_PACKET_CTRL_TX_EOT_APPEND` clear, so the DSI appends no EOT to any HS packet, including the
compressed-pixel-stream packets and the PPS (`mipi_dsi_picture_parameter_set_multi`, type 0x0a).
For a **video-mode DSC** panel whose line is split into N packets, the packet tail is the only
delimiter the DDIC has; a missing EOT lets the DDIC mis-time the end of a compressed packet
→ per-line corruption, sensitive to `pkt_per_line`.
Cheap A/B: **drop `MIPI_DSI_MODE_NO_EOT_PACKET`** from `dsi->mode_flags`.

---

### Bug D — `block_pred_enable` contradicts the extracted vendor DT  [confidence: unknown, must be resolved first]

`panel-xiaomi-m19.c:65` sets `block_pred_enable = true`; `docs/panel-m19-init.json` (extracted from
the vendor DTS) records `qcom,mdss-dsc-block-prediction-enable: false`. The task brief says block
prediction **is** enabled. **These two statements cannot both be true.** A block-prediction mismatch
between encoder and decoder is one of the classic "DSC enabled, image is noise" failures.
Resolve by reading the vendor DTS node directly (not the extraction) before testing.
Test patch if `false` turns out to be correct: `.block_pred_enable = false,`.

---

### Also checked and *rejected* (with evidence, so they are not re-investigated)

| Hypothesis | Why it is not the bug |
|---|---|
| Panel sends a PPS with zeroed `chunk_size`/RC fields (because `drm_dsc_compute_rc_parameters()` was never called at probe) | Ordering: DSI pre_enable → `dsi_populate_dsc_params()` (`dsi_host.c:2559`→`1059`) runs before panel `enable()` → PPS (`panel:293`), see §1.2. Same memory (`dsi->dsc == &ctx->dsc == msm_host->dsc`, `dsi_host.c:1741`). |
| `drm_dsc_setup_rc_params()` leaves `slice_chunk_size` = 0 and the DPU programs 0 | Recomputed at 1949 before the DPU runs (`dpu_encoder.c:2110`); log shows `chunk=540`. |
| `bits_per_pixel` encoded wrongly (e.g. 8 instead of 128) | 4.4 fixed point is what everyone expects: `dsi_host.c:1897`, `drm_dsc_get_bpp_int()` (`helper:1447-1451`), DPU `bpp<<10` (143-150), PPS split into `pps_4` high bits + `bits_per_pixel_low` (`helper:132-133`). Panel value is already 128. |
| `first_line_bpg_offset`/`initial_offset`/`initial_xmit_delay` mismatch *between PPS and DPU* | Both read the same struct after `populate`; PPS is built later (`panel:293`), DPU later still. |
| widebus disagreement between DPU and DSI | Single source of truth, `msm_dsi_host_is_wide_bus_enabled()`; both call it (§1.7). |
| `MIPI_DSI_MODE_VIDEO_HSE` / `NO_HFP` / `NO_HBP` / `CLOCK_NON_CONTINUOUS` missing | These only set `DSI_VID_CFG0_PULSE_MODE_HSA_HE` / `*_POWER_STOP` LP-escape bits and the HS-clock force (`dsi_host.c:835-848, 930-938`). They change power states in the porch/BLLP regions, not the content or framing of the active payload. Burst mode is correct per the vendor DT (`traffic_mode: burst_mode`, `panel_type: dsi_video_mode`). Not a content-corruption candidate. |
| DSC geometry invalid (would be rejected by `msm_dsi_host_check_dsc()`) | 1080 % 540 == 0, 2460 % 20 == 0 (`dsi_host.c:2641-2651`) → OK. |
| `dce_bytes_per_line` read before the DSC config is populated | Encoder enable runs after bridge pre_enable (`drm_atomic_helper.c:1736-1745`), so `slice_chunk_size` is already 540. |

---

## 4. Could this be a DRM/DPU-side bug rather than DSC?

**No — with the evidence below.** The user's question deserves a per-item answer.

### 4.1 UBWC / format modifier on a linear fbdev buffer — impossible by construction

* fbdev: `msm_fbdev.c:109` — `drm_get_format_info(dev, fourcc, DRM_FORMAT_MOD_LINEAR)`; the buffer
  itself comes from `msm_gem_new()` (`msm_fbdev.c:115`) and the fb is created from a legacy
  `drm_mode_fb_cmd2` with `modifier[0] == 0`.
* `msm_framebuffer_init()` (`msm_fb.c:137-160`) resolves the format with
  `mdp_get_format(kms, pixel_format, mode_cmd->modifier[0])` (`:154`).
* `mdp_get_format()` (`disp/mdp_format.c:619-651`) is a **hard switch on the modifier**:
  `case 0:` → `mdp_formats[]` (linear); `case DRM_FORMAT_MOD_QCOM_COMPRESSED:` →
  `mdp_formats_ubwc[]`; anything else → return NULL → `msm_framebuffer_init()` fails loudly with
  "unsupported pixel format". There is no code path that silently picks a UBWC variant for a linear
  buffer.
* `dpu_plane_format_mod_supported()` (`dpu_plane.c:1781-1793`) unconditionally accepts
  `DRM_FORMAT_MOD_LINEAR`, and accepts `QCOM_COMPRESSED` only if `ubwc_enc_version != 0` **and** the
  fourcc is in the compressed list; the plane's modifier list is
  `{QCOM_COMPRESSED, LINEAR, INVALID}` (`dpu_plane.c:91-95`).
* Consequence for the hypothetical artifact: if UBWC *were* programmed over a linear buffer, the
  SSPP would interpret the buffer as tile+meta formatted — the result is a uniform, tile-block-shaped
  garble of the *whole* image that is **invariant under DSC slice geometry**. The reported symptom
  changes with `slice_width`/`slice_count`/`slice_per_pkt`, which no plane-level addressing/tiling
  bug can do.
* Also: `XRGB8888` linear is `mdp_format.c:374-376` (`BPC8A/BPC8/BPC8/BPC8`,
  `C1_B_Cb/C0_G_Y/C2_R_Cr/C3_ALPHA`) → `MSM_FORMAT_IS_UBWC(fmt)` is false, the align/UBWC handling in
  `dpu_formats.c:61,94-139,241,265-308` is never reached, and `intf->panel_format` is derived from
  `bpc_g_y|bpc_b_cb|bpc_r_cr` (`dpu_hw_intf.c:231-241`) → 8/8/8.

### 4.2 Wrong CSC / 32-bit XRGB (alpha ignored) — colour errors, not banding

A wrong format or CSC would show as channel swaps, wrong primary order, gamma or reduced depth.
`dpu_hw_intf.c:231-241` treats everything as RGB888 at the interface, and `dsi->format` is
`MIPI_DSI_FMT_RGB888` (`panel:446`) → `DSI_VID_CFG0_DST_FORMAT(RGB888)` (`dsi_host.c:850`) which is
irrelevant for a compressed stream anyway (the video datatype is `COMPRESSED_PIXEL_STREAM`). A format
problem would also be **static** w.r.t. DSC parameters.

### 4.3 The only DPU-side object that matters is shared, not duplicated

The DPU does not have its own DSC config: `dpu_encoder_get_dsc_config()` (`dpu_encoder.c:644-652`)
returns `msm_dsi_get_dsc_config()` i.e. `msm_host->dsc`, the very same struct the DSI populates and the
panel serialises into the PPS. So "DPU and DSI disagree" is structurally impossible here; the only
disagreement that can exist is host-vs-DDIC, which is what bugs A/B/C/D are about.

### 4.4 What *would* look like a DRM bug but is not

The I-cache/underrun/fence class of DPU bugs produce tearing, a shifted image, or a solid colour
(timer/underflow colour `0xff`, `dpu_encoder_phys_vid.c:89`), and would log `... underrun`. None of
that matches, and dmesg is clean.

**Verdict: the DRM/KMS (DPU plane) side is not implicated. The defect lives in the DSC/DSI-packing
configuration, specifically in how the compressed line is parameterised and packetised.**

---

## 5. Blocking process issue — the runtime evidence does not match these sources

The dmesg strings quoted in the task cannot have been produced by the current tree:

| Task dmesg | Current source |
|---|---|
| `sky-dsc12: slice=540x20 count=2 chunk=540 bpp=128 bpc=8 mode=0x4 initial_lines=2` | `dpu_hw_dsc_1_2.c:116-120` also prints `pic=%ux%u … active_slice/enc=%u` |
| (absent) | `sky-dscparams: …` (`dsi_host.c:1940-1944`) |
| (absent) | `sky-panel: PPS sent, err=%d` (`panel-xiaomi-m19.c:295`) |
| (absent) | `sky-enc: prep_dsc i=…` (`dpu_encoder.c:2038-2040`), `sky-enc: kickoff …` (`:2107`) |
| `sky-dsi-dsc: … pkt_per_line=2` | `dsi_host.c:967-970` also prints `slice_per_pkt=%u` |

Either the log was abridged, or **the running kernel predates the current patches**. This matters a
lot for bug A: `flags=0xa03` (no `BIT(13)`) is only meaningful if the log is complete. **Rebuild from
this tree and re-capture dmesg before drawing further conclusions.** Things to capture:

1. `sky-dsi-dsc:` must show `slice_per_pkt=2 bytes_per_pkt=1080 pkt_per_line=1` after bug A is
   addressed.
2. `sky-dscparams:` must show `fl_bpg=12` today (proving the `= 13` override is discarded) — or 13
   after the bug B patch.
3. `sky-enc: topology num_intf=1 dsc=set cat_dsc_count=1` and `mode=0x4` confirm the single-DSC
   topology assumed above.
4. Optional register-level confirmation: `DSC_CTL`/`DSC_CFG`/registers are not exposed via debugfs
   (`dpu_kms.c:296-316` only creates `debug/{hwmask,danger,vbif,core_irq,sspp,perf}`), so verify
   `REG_DSI_VIDEO_COMPRESSION_MODE_CTRL` (0x29c) semantics through the `sky-dsi-dsc` print instead.
5. `cat /sys/kernel/debug/dri/<N>/kms` shows the atomic state (confirms the fb is linear, plane
   format `XR24`).

---

## 6. Suggested test order (cheapest → most informative)

1. Rebuild from this tree, re-capture the four `sky-*` lines (§5). Everything else depends on it.
2. Bug A: force `dsc_slice_per_pkt = slice_count`; expect `bytes_per_pkt=1080 pkt_per_line=1`.
3. Bug C: drop `MIPI_DSI_MODE_NO_EOT_PACKET`.
4. Bug B: reinstate the 1.1 RC table + `first_line_bpg_offset = 13`.
5. Bug D: resolve `block-prediction-enable` against the real vendor DTS, then test `false`.
6. Only if all of the above fail: instrument `dsi_host.c` to print the final
   `REG_DSI_VIDEO_COMPRESSION_MODE_CTRL` word and the full PPS byte array, and compare against the
   DDIC's vendor-programmed DSC registers (`docs/panel-m19-init.json`, page 0x23 regs 0x00-0x2b).
