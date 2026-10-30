# Vendor DSC + DSI-mode configuration for panel `m19_36_02_0a` (Redmi 12 5G / SM4450 "sky")

Analysis of `/tmp/msm_drm.ko` (ELF64 ARM64 relocatable, not stripped, BuildID
`13abad0d9d4517ce6a44e90d62deceb24c7d2fc3`) and `/tmp/river.dts`.

Sections/symbols used (all offsets are **section** offsets, because the module is
relocatable and every section has `sh_addr = 0`):

| section | file offset | size | note |
|---|---|---|---|
| `.text` | `0x1000` | `0x184fd8` | code; all `T`/`t` symbols below are `.text` offsets |
| `.data` | `0x195ee0` | `0x9a20` | `sde_dsc_rc_init_param_lut_v1` lives here |
| `.rodata` | `0x19f900` | `0x6d948` | strings + DSC LUTs/tables |
| `.bss` | (NOBITS) | `0x1eaae` | `is_panel` (u8) at `.bss+0x1d43c` |

`rodata_file_off = 0x19f900 + rodata_vaddr`, e.g. `m19_36_02_0a` at
`.rodata+0x1622e` = file `0x1b5b2e`.

Tools: `aarch64-linux-gnu-objdump -d/-r`, `aarch64-linux-gnu-nm -n`,
`aarch64-linux-gnu-readelf -SW`, `strings -t x`, `python3` (raw section dumps and a
re-implementation of the vendor arithmetic that is fed *the real table bytes*).

---

## 1. Summary table

Configuration analysed: **1080x2460, 90 Hz, 4-lane DSI, video mode, DSC,
bpp = 8, bpc = 8, slice 540x20 (2 slices per packet), 4:4:4,
block prediction enabled**.

| field | vendor value | how obtained (addresses / offsets) | confidence |
|---|---|---|---|
| **DSC version major / minor** | **1 / 1** (PPS0 = `0x11`, DSC 1.1) | `dsi_panel_get_mode` `0x16ae88`: parse `qcom,mdss-dsc-version`; on failure stores `0x0101` at `dsc+126/127` (= `panel+958/959`). m19 DT node has **no** such property (§2.4, §A.4). | verified twice (code default + DT absence; the `qcom,mdss_dsi_sim_cmd` test panel in the same dts *does* declare `qcom,mdss-dsc-version = <0x12>` at `river.dts:620`) |
| `scr_ver` (2nd arg of `sde_dsc_populate_dsc_config`) | **0** | `0x16af00`: `qcom,mdss-dsc-scr-version` absent → `strb wzr,[x19,#968]`; passed as `w1` at `0x16b1e4`/`0x16b200` | verified twice (code default + DT absence) |
| `bits_per_pixel` (1/16 units) | **128** (8 bpp) | `dsc+16` = `panel+848`; index-map match uses `>>4` = 8 (`0x123284`, `0x1232f0`); PPS5/PPS4 pack at `0x123868`-`0x123894` | verified twice (index map vs PPS packing) |
| `bits_per_component` | **8** | `dsc+1` = `panel+833`; PPS3 pack `0x123858` (`bfi w8,w9,#4,#28`) | verified twice (index map `+12` vs PPS3) |
| `line_buf_depth` | **9** (= bpc+1) | `0x1234d0`: `w8 = w20+1; strb w8,[x19,#0]`; PPS3 low nibble | verified twice (code + PPS3 byte) |
| `convert_rgb` | **1** | `0x123264`-`0x123278`: `((nat422\|nat420)^1)` → `dsc+2`; PPS4 bit4 | verified twice (code + PPS4 packing + mainline `dsi_host.c:1920`) |
| `simple_422` / `vbr_enable` | **0 / 0** | `0x123270` `strb wzr,[x19,#8]`, `0x12327c` `strb wzr,[x19,#120]` | single source (code immediates) |
| `block_pred_enable` | **1** | `dsc+26` = `panel+858`, set from `of_property_read_bool(...)` at `0x16b1d8`-`0x16b1e8`; DT has `qcom,mdss-dsc-block-prediction-enable;`; PPS4 bit5 (`dsc[26]<<5`, `0x12388c`) | verified twice (DT property + code/PPS) |
| native 4:2:2 / 4:2:0 flags (`dsc+128`/`dsc+129`) | **0 / 0** (4:4:4) | `qcom,src-chroma-format` parse at `0x16b0e0` (string `.rodata+0x19293`); absent in m19 node → nothing stored | verified twice (DT absence + `w6 = 0` selector, §3.1) |
| RC table index (`sde_dsc_index_map`) | **entry 0 → `table_index = 0`** | `0x1232c0`-`0x123314` loop over `sde_dsc_index_map` (`.rodata+0x6803c`, 16x24 B) matching (mode, ver, bpc, bpp) + `+4 == scr_ver` | verified twice (disassembly loop + decoded table bytes) |
| RC init LUT actually used | **`sde_dsc_rc_init_param_lut_v1` entry 0** (`.data+0x77b0`), *not* the `.rodata` table | `0x1233e0`-`0x123404`: `if (is_panel == 10) lut = &..._lut_v1`; `is_panel` = 10 is set for `m19_36_02_0a` (§2) | verified twice (relocation + `.data` byte dump) |
| `initial_xmit_delay` | **170 (0x00AA)** | LUT entry0 dword[3] = `0x000000aa` at `.data+0x77c2`; loaded `0x123478`, stored `strh w13,[x19,#22]` `0x123484`; PPS16/17 = `00 aa` | **verified three ways** (LUT bytes, disassembly, panel register 0xC1 = `00 aa`) |
| `initial_offset` | **6144 (0x1800)** | LUT dword[2] = `0x1800`; `strh w13,[x19,#28]` `0x123444`; PPS32/33 = `18 00` | verified twice (LUT + PPS) |
| `rc_model_size` | **8192 (0x2000)** | `0x123170` `mov w8,#0x2000; strh w8,[x19,#104]`; PPS38/39 | verified twice |
| `rc_edge_factor` | **6** | `0x123258` `mov w8,#6; strb w8,[x19,#18]`; PPS40 | verified twice |
| `rc_quant_incr_limit0` / `limit1` | **11 / 11 (0x0b/0x0b)** | LUT dword[0]/[1]; `strb w10,[x19,#20]` `0x123470`, `strb w8,[x19,#19]` `0x12341c`; PPS41/42 | verified twice |
| `rc_tgt_offset_hi` / `lo` | **3 / 3** | `0x123260`/`0x123274` `mov w11,#0x303; strh w11,[x19,#14]`; PPS43 = `0x33` | verified twice |
| `flatness_min_qp` / `max_qp` | **3 / 12** | LUT dword[6]/[7]; `strb` to `dsc+106/107` `0x123498`/`0x1234ac`; PPS36/37 | verified twice |
| `first_line_bpg_offset` | **12 (0x0C)** | `0x123174`-`0x1231c0`: `major==1 && minor==1 && scr_ver==0` → `w8 = 0x0c`, `strb w8,[x19,#27]`; PPS27 | **verified three ways** (code path, PPS28/29 = nfl = 1294 ⇒ fl = 12, panel 0xC1) |
| `mux_word_size` (vendor `dsc+121`) | **48 (0x30)** | `0x1234dc`/`0x1234e4` `csel w8,#0x40,#0x30,hi` on `bpc>10`; stored `strb w8,[x19,#121]` `0x1234ec` | single source (code) |
| `num_extra_mux_bits` (derived) | **246 → 240** | `0x123628`-`0x123630` `w9 = 3*(mux+4*bpc)+6` = 246; `0x123638`-`0x123644` mux-alignment ⇒ 240 | verified twice (two code sites + mainline formula gives same 240) |
| `slice_chunk_size` | **540 (0x021C)** | `0x123550`/`0x12355c`-`0x123568`: `(bpp*slice_width+7)>>3`; `strh w11,[x19,#122]` `0x123570`; PPS14/15 = `02 1c` | verified twice (also `sde_dsc_populate_dsc_private_params` `0x123638`-`0x1236a4`) |
| `initial_scale_value` | **32 (0x20)** | `0x123580`-`0x123590`: `(rc_model_size<<3)/(rc_model_size-initial_offset)` = 65536/2048; `strb w16,[x19,#108]` `0x123598`; PPS21 | verified twice |
| `initial_dec_delay` | **526 (0x020E)** | `0x123540`-`0x12359c`: `((bpp + bpp*xmit) + (rcm-init_off) + gpl*fl - 1)/bpp - xmit = 696-170`; `strh w0,[x19,#24]`; PPS18/19 | **verified three ways** (code, PPS, panel 0xC1 = `02 0e`) |
| `nfl_bpg_offset` | **1294 (0x050E)** | `0x12352c`-`0x1235a0`: `(sh-1 + (fl<<11) - 1)/(sh-1)` = 24594/19; `strh w12,[x19,#114]`; PPS28/29 | **verified three ways** (code, PPS, panel 0xC1 = `05 0e`) |
| `slice_bpg_offset` | **1302 (0x0516)** | `0x1235a8`-`0x123668`: `(gt + ((rcm-init_off+extra)<<11) - 1)/gt`, `gt = gpl*sh = 3600`; `strh w9,[x19,#116]`; PPS30/31 | **verified three ways** (code, PPS, panel 0xC1 = `05 16`) |
| `final_offset` | **7072 (0x1BA0)** | `0x12364c`-`0x123684`: `rcm - xmit*bpp + extra` = 8192-1360+240; `strh w8,[x19,#118]`; PPS34/35 | **verified three ways** (code, PPS, consistency: `(rcm-final_offset)=1120` drives `final_scale=58` which reproduces the panel's `scale_incr=113`) |
| `scale_increment_interval` | **113 (0x0071)** | `0x123688`-`0x12369c`: `(final_offset<<11)/((fs-9)*(nfl+slice_bpg))` = 14483456/127204, `fs = 65536/1120 = 58`; `strh w8,[x19,#112]`; PPS22/23 | **verified three ways** (code, PPS, panel 0xC1 = `00 71`) |
| `scale_decrement_interval` | **7** | `0x12367c`/`0x123694`/`0x1236a0`: `gpl/(initial_scale_value-8)` = 180/24; `strh w15,[x19,#110]`; PPS24/25 | **verified three ways** (code, PPS, panel 0xC1 = `00 07`) |
| `rc_buf_thresh[14]` | `0e 1c 2a 38 46 54 62 69 70 77 79 7b 7d 7e` | immediates built at `0x123318`-`0x123370` and stored to `dsc+30..+56` (stride 2); PPS44..57 | verified twice (immediates + `drm_dsc_rc_buf_thresh[]>>6` in mainline) |
| `rc_range_params[15]` (`sde_dsc_rc_range_{min,max}_qp`, `_bpg` group 0) | min `0,0,1,1,3,3,3,3,3,3,5,5,5,7,13`; max `4,4,5,6,7,7,7,8,9,10,10,11,11,12,13`; bpg `2,0,0,-2,-4,-6,-8,-8,-8,-10,-10,-12,-12,-12,-12` | tables `.rodata+0x67be4` / `+0x67cb6` / `+0x67d88`, group `table_index*15 + i`; interleaved into `dsc+58..102` at `0x1233a0`-`0x1233c8` | verified twice (table bytes + mainline `rc_parameters_pre_scr` bpp8/bpc8 entry — byte-identical) |
| `rc_buf_thresh`/range params → PPS bytes 44..87 | `0e…7e` then 2 bytes per range: `min<<3 \| max>>2`, `(max&3)<<6 \| bpg` | `0x1239fc`-`0x123a68` (thresh) and `0x123a70`-`0x123aa8` (range loop) | verified twice (code + spec packing) |
| PPS0..PPS87 layout | identical to mainline `drm_dsc_pps_payload_pack()` | see §4 field-by-field mapping | verified twice (per-field byte-offset derivation vs mainline source) |
| PPS88..PPS93 (native/second-line fields) | **all 0** (1.1 path skips them; the offsets 88..93 do match mainline, but bytes 89-93 carry vendor-specific values instead of `second_line_bpg_offset`/`nsl_bpg_offset`/`second_line_offset_adj`) | `0x123aac` `cmp w8,#2; b.ne` return | single source (code) |
| `qcom,display-topology` / intf mode | `1 1 1` (single LM, single INTF, single encoder), `dsi_video_mode` | DT panel node (`river.dts:2237-2260`) | single source (DT) |
| widebus | **no DT property for DSI** (`qcom,widebus-enable` is parsed only by `dp_parser_parse` `0x35670`); `dsi_display_update_byte_intf_div()` unconditionally programs `byte_intf_div = 2` | see §6 | partially verified — see §7 |
| `DSI_VIDEO_COMPRESSION_MODE_CTRL` (0x29c) | written by `sde_hw_intf_setup_te_config` (`0x10b9cc`) as `cfg[12] \| cfg[16]<<16` | see §5 | packing verified; **the producer of `cfg[12]/cfg[16]` was not located** — see §7 |

**Full PPS payload the vendor will emit (computed by re-running the vendor algorithm
on the real table bytes, `sim.py` in the appendix):**

```
  0: 11 00 00 89 30 80 09 9c 04 38 00 14 02 1c 02 1c
 16: 00 aa 02 0e 00 20 00 71 00 07 00 0c 05 0e 05 16
 32: 18 00 1b a0 03 0c 20 00 06 0b 0b 33 0e 1c 2a 38
 48: 46 54 62 69 70 77 79 7b 7d 7e 01 02 01 00 09 40
 64: 09 be 19 fc 19 fa 19 f8 1a 38 1a 78 1a b6 2a f6
 80: 2b 34 2b 74 3b 74 6b f4 00 00 00 00 00 00 00 00
 96: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
112: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

---

## 2. Question 1 — where `m19_36_02_0a` is referenced, and what the "panel descriptor" really is

### 2.1 The string

`strings -t x` → `1b5b2e m19_36_02_0a` (13 bytes + NUL, standalone in the `.rodata`
string blob, surrounded by unrelated DSI error strings).
As a section offset: `.rodata + 0x1622e`.

### 2.2 Every relocation that points at it

`aarch64-linux-gnu-objdump -r /tmp/msm_drm.ko` (96,982 relocation lines) contains
exactly four references:

```
0000000000159cf4 R_AARCH64_ADR_PREL_PG_HI21  .rodata+0x000000000001622e
0000000000159cf8 R_AARCH64_ADD_ABS_LO12_NC   .rodata+0x000000000001622e
00000000001644f8 R_AARCH64_ADR_PREL_PG_HI21  .rodata+0x000000000001622e
00000000001644fc R_AARCH64_ADD_ABS_LO12_NC   .rodata+0x000000000001622e
```

Both are **code** references (`adrp`/`add` pairs), **not** data pointers:

* `0x159cf4` is inside `dsi_display_dev_probe` (starts `0x159a74`),
* `0x1644f8` is inside `panel_display_show` (debugfs `panel_display`, `0x1644dc`).

There is **no** relocation from `.data`, `.ref.data` or `.rodata` to
`.rodata+0x1622e`. A data-driven panel descriptor keyed by this name therefore
**does not exist in this module**; `struct msm_display_dsc_info` is not even
referenced by any structure initialiser for this panel (checked with the same
relocation scan; the only `.data`-target relocation section in the module is
`.rela.data.msm_hdcp_driver.llvm.*`, and none of its 1046 entries points at the
string).

### 2.3 What the reference actually does — `is_panel`, the real panel-specific hook

```
  159cc0:  mov  w9, #0x228                     ; 552
  159ce0:  bl   set_panel_name_gki
  159cf4:  adrp x1, .rodata+0x1622e            ; "m19_36_02_0a"
  159cf8:  add  x1, x1, #0
  159cfc:  mov  x0, x23                        ; panel name from DT
  159d00:  bl   strstr
  159d04:  cbz  x0, 159d14
  159d08:  adrp x8, .bss+0x1d43c               ; symbol `is_panel` (u8)
  159d0c:  mov  w9, #0xa
  159d10:  strb w9, [x8]                       ; is_panel = 10
  159d14:  ... strstr(panel_name, "m19_42_03_0b") -> is_panel = 11
  159d34:  ... strstr(panel_name, "m19_42_03_0c") -> is_panel = 12
```

`nm` confirms `is_panel` is `d`/`.bss+0x1d43c`, size 1. So the *only* thing the
binary knows about `m19_36_02_0a` is the integer **10**, and the only consumer of
that value is `sde_dsc_populate_dsc_config` — see §3.3. Even the index-map "what
table do I use" question is *not* answered by a per-panel struct.

### 2.4 Where the panel configuration really lives (runtime structure)

All panel parameters come from the DT node and are copied into the driver's
`struct dsi_panel` in `dsi_panel_get_mode` (`0x16a564`). Two facts pin the DSC
sub-structure's location and layout:

* `0x16b1d4: add x26, x19, #0x340` and `0x16b1f8: mov x0, x26` →
  `bl sde_dsc_populate_dsc_config` ⇒ **`struct msm_display_dsc_info` = `dsi_panel + 0x340`**.
* `sde_dsc_populate_dsc_private_params(x26, w28)` immediately after (`0x16b208`-`0x16b210`).

Decoder-ring of the DT→struct stores (all `x19` = `dsi_panel*`, so `dsc = panel+0x340`):

| DT property | store | dsc offset |
|---|---|---|
| `qcom,mdss-dsc-bit-per-component` | `0x16af…` (u32 read) → `panel+833` | `dsc+1` |
| `qcom,mdss-dsc-slice-width` | `0x16b0…` → `panel+836` | `dsc+4` |
| `qcom,mdss-dsc-slice-height` | `0x16af4c: strh w8,[x19,#838]` | `dsc+6` |
| `qcom,mdss-dsc-bit-per-pixel` | `0x16af7c`-`0x16b0xx` → `panel+848` (`bpp<<4`, see `0x16b218: lsr #4`) | `dsc+16` |
| `qcom,mdss-dsc-block-prediction-enable` | `0x16b1d8` `of_property_read_bool` → `0x16b1e8: strb w9,[x19,#858]` | `dsc+26` |
| `qcom,mdss-dsc-version` (absent) | `0x16ae9c: mov w8,#0x101; strh w8,[x19,#958]` | `dsc+126/127` |
| `qcom,mdss-dsc-scr-version` (absent) | `0x16af00: strb wzr,[x19,#968]` | passed as arg, see §3.1 |
| `qcom,src-chroma-format` (absent) | `0x16b18c: cmp w8,#2 / #1` → `panel+960` / `+961` | `dsc+128` / `dsc+129` |
| mode `hdisplay` | `0x16af9c: strh w28,[x19,#842]` | `dsc+10` (pic_width) |
| mode `vdisplay` | `0x16afa4: strh w8,[x19,#844]` | `dsc+12` (pic_height) |
| derived | `0x16b1ec`-`0x16b1fc`: `(pic_width + slice_width - 1) / slice_width` → `strb w8,[x19,#835]` | `dsc+3` = **pkt_per_line = 2** |

Mode parameters (lanes, fps, porches) are **not** in any C descriptor either — they
are read from the DT node directly in the same function:
`qcom,mdss-dsi-panel-width = <0x438>` (1080), `-height = <0x99c>` (2460),
`-panel-framerate = <0x5a>` (90), `qcom,mdss-dsi-lane-0…3-state` (⇒ 4 lanes),
`h-front-porch 0x3b, h-back-porch 0x3c, h-pulse-width 0x0c, v-front-porch 0x36,
v-back-porch 0x0a, v-pulse-width 0x0a` (`river.dts:2255-2261`).
The init sequence pointer is likewise DT-only (`qcom,mdss-dsi-on-command`, 1486 B,
already extracted to `docs/panel-m19-init.json`).

**Conclusion (Q1):** there is no vendor "panel descriptor struct" for this panel
inside `msm_drm.ko`. The module is fully DT-driven; the string reference exists only
to set the global byte `is_panel = 10`, which is used for exactly one purpose — the
DSC RC-init-parameter LUT selection described next.

---

## 3. Question 2 — RFC/DSC parameters from `sde_dsc_populate_dsc_config`

Function layout: `sde_dsc_populate_dsc_config` `0x123144`–`0x1236ac` (size 0x568),
`sde_dsc_populate_dsc_private_params` `0x1236ac`–`0x1237fc`,
`sde_dsc_create_pps_buf_cmd` `0x1237fc`–`0x123b20`, `_get_dsc_v1_2_bpg_offset`
`0x1230a0`–`0x123144`.

Relocation map of the function (from `objdump -r`), which is what makes the table
attribution unambiguous:

```
0x12329c/0x1232ac  .rodata+0x6803c   -> sde_dsc_index_map   (16 x 24 B, size 0x180)
0x12335c           .rodata+0x67d88   -> sde_dsc_rc_range_bpg
0x123364           .rodata+0x67be4   -> sde_dsc_rc_range_min_qp
0x12336c           .rodata+0x67cb6   -> sde_dsc_rc_range_max_qp
0x1233cc/0x1233d0  is_panel (bss)    + .rodata+0x7ede  "sde_dsc_populate_dsc_config panel[%d]"
0x1233e4/0x1233f0  sde_dsc_rc_init_param_lut_v1 (.data+0x77b0)
0x1233e8/0x1233f4  .rodata+0x67e5c   -> sde_dsc_rc_init_param_lut
0x1236f0/0x1236f8  .rodata+0x6d74c   (3-entry u32 table used by private_params)
```

### 3.1 Inputs and the version dispatch (`0x123160`-`0x1231c8`)

```
  123160:  mov  w3, w1              ; arg2 = scr_ver
  123164:  ldrb w1, [x0, #127]      ; dsc_version_major
  123168:  ldrb w2, [x0, #126]      ; dsc_version_minor
  123170:  mov  w8, #0x2000
  123178:  strh w8, [x0, #104]      ; rc_model_size = 8192
  123174:  cmp  w1, #0x1 ; b.ne 123198
  12317c:  cmp  w2, #0x1 ; b.ne 123198
  123188:  cmp  w3, #0x1
  12318c:  b.ne 1231bc
  123190:  mov  w8, #0xf            ; scr_ver == 1 -> 15
  1231bc:  mov  w8, #0xc            ; scr_ver == 0 -> 12
  1231c0:  strb w8, [x19, #27]      ; first_line_bpg_offset
  1231c4:  mov  w2, #0x1            ; -> index-map "version" key = 1
  123250:  mov  w8, #0x6 ; strb w8,[x19,#18]     ; rc_edge_factor = 6
  123260:  mov  w11,#0x303 ; strh w11,[x19,#14]  ; rc_tgt_offset hi/lo = 3/3
  12326c:  eor  w8, w8, #0x1 ; strb w8,[x19,#2]  ; convert_rgb = !(422|420) = 1
  123280:  b.ne 1235c0                            ; version_major != 1 -> error out
```

For **our** panel (`major=1, minor=1, scr_ver=0`) the `0x123198` (minor==2) branch and
`_get_dsc_v1_2_bpg_offset()` are **not executed**; `first_line_bpg_offset = 12`.

### 3.2 Index-map match (`0x123284`-`0x123314`)

```
  123284:  ldrh w11, [x19, #16]     ; bits_per_pixel (1/16 units) = 128
  123288:  ldrb w20, [x19, #1]      ; bits_per_component = 8
  12328c:  cmp  w10, #0             ; w10 = dsc[129] (src-chroma-format == 2)
  123290:  cset w10, ne
  123298:  lsl  w9, w10, #1
  1232a4:  csinc w6, w9, wzr, eq    ; w6 = 0 (444) / 1 (dsc[128]) / 2 (dsc[129])
  1232a8:  lsr  w21, w11, #4        ; w21 = 8   (integer bpp)
  loop over 16 entries x 24 B:
  1232c4:  ldr w9,[x9,#8]  ; cmp w9, w2   -> +8  == version (1)
  1232dc:  ldr w11,[x9,#12]; cmp w11,w20  -> +12 == bpc (8)
  1232ec:  ldr w9,[x9,#16] ; cmp w9, w21  -> +16 == bpp (8)
  1232d0:  ldr w9,[x10,x8] ; cmp w6, w9   -> +0  == mode selector
  123304:  ldr w9,[x9,#4]  ; cmp w9, w3   -> +4  == scr_ver (only when version==1)
  123314:  ldrsw x22, [x8, #20]           -> +20 = table_index
```

Decoded `sde_dsc_index_map` (`.rodata+0x6803c`, 16 entries x 24 B, fields
`{mode, +4, version, bpc, bpp(integer), table_index}`):

```
 [0] mode=0 +4=0          ver=1 bpc=8  bpp=8  idx=0   <-- m19_36_02_0a matches HERE
 [1] mode=0 +4=0          ver=1 bpc=10 bpp=8  idx=1
 [2] mode=0 +4=0          ver=1 bpc=10 bpp=10 idx=2
 [3] mode=0 +4=1          ver=1 bpc=8  bpp=8  idx=3
 [4] mode=0 +4=1          ver=1 bpc=10 bpp=8  idx=4
 [5] mode=0 +4=1          ver=1 bpc=10 bpp=10 idx=5
 [6] mode=0 +4=0xffffffff ver=2 bpc=8  bpp=8  idx=3
 [7] mode=0 +4=0xffffffff ver=2 bpc=10 bpp=8  idx=4
 [8] mode=0 +4=0xffffffff ver=2 bpc=10 bpp=10 idx=5
 [9] mode=1 ...           ver=2 bpc=8  bpp=7  idx=6
[10] mode=1 ...           ver=2 bpc=8  bpp=8  idx=7
[11] mode=1 ...           ver=2 bpc=10 bpp=7  idx=8
[12] mode=1 ...           ver=2 bpc=10 bpp=8  idx=9
[13] mode=1 ...           ver=2 bpc=10 bpp=10 idx=10
[14] mode=2 ...           ver=2 bpc=8  bpp=6  idx=11
[15] mode=2 ...           ver=2 bpc=10 bpp=6  idx=12
```

⇒ **`table_index = 0`** (raw bytes: `00 00 00 00 | 00 00 00 00 | 01 00 00 00 |
08 00 00 00 | 08 00 00 00 | 00 00 00 00`).

Note how this differs from a DSC-1.2 panel: entry `[6]` (which is what a
`qcom,mdss-dsc-version = <0x12>` panel with the same bpp/bpc would select) maps to
table_index **3**, not 0.

### 3.3 LUT selection — the Xiaomi override (`0x1233cc`-`0x123420`)

```
  1233cc:  adrp x23, is_panel
  1233d0:  ldrb w1, [x23] ; bl printk ("sde_dsc_populate_dsc_config panel[%d]")
  1233e0:  ldrb w8, [x23]                       ; is_panel
  1233e4:  adrp x10, sde_dsc_rc_init_param_lut_v1     ; .data+0x77b0
  1233e8:  adrp x11, .rodata+0x67e5c                  ; sde_dsc_rc_init_param_lut
  1233ec:  lsl  x9, x22, #5                     ; table_index * 32
  123400:  cmp  w8, #0xa
  123404:  csel x11, x10, x11, eq                ; is_panel==10 -> lut_v1 !!
  123410:  ldr  w8, [x11]      ; -> dsc+20  (rc_quant_incr_limit0)
  123424:  ldr  w8, [x11, #4]  ; -> dsc+19  (rc_quant_incr_limit1)
  123438:  ldr  w8, [x11, #8]  ; -> dsc+28  (initial_offset, u16)
  12344c:  ldr  w12,[x11, #12] ; -> dsc+22  (initial_xmit_delay, u16)
  123460:  ldr  w10,[x11, #16] ; -> dsc+130
  123478:  ldr  w13,[x11, #20] ; -> dsc+134 (u16)
  12348c:  ldr  w13,[x11, #24] ; -> dsc+106 (flatness_min_qp)
  1234a0:  ldr  w13,[x11, #28] ; -> dsc+107 (flatness_max_qp)
```

Byte dumps (`python3` on the extracted sections):

```
=== sde_dsc_rc_init_param_lut  .rodata+0x67e5c (leader entry 0) ===
  [ 0] 0x0000000b 0x0000000b 0x00001800 0x00000200 0x0 0x0 0x00000003 0x0000000c
=== sde_dsc_rc_init_param_lut_v1  .data+0x77b0 (entry 0) ===
  [ 0] 0x0000000b 0x0000000b 0x00001800 0x000000aa 0x0 0x0 0x00000003 0x0000000c
```

`is_panel == 10` for this panel (m19_36_02_0a), so the **`_v1` table from `.data`
is used**, and the only difference at entry 0 is
`initial_xmit_delay = 0xaa = 170` instead of the standard `0x200 = 512`.
(Entries 1..14 of both tables are byte-identical, including entry 3 which a DSC-1.2
panel would use — so this override is specific to the 1.1/8bpc/8bpp 4:4:4 case.)

### 3.4 RC tables (group 0 = `table_index 0`)

```
  123350:  lsl  x8, x22, #4        ; idx*16
  123374:  sub  x13, x8, x22       ; idx*15  <- 15 ranges per table index
  12338c:  add  x8,  x10, x13      ; bpg      (0x67d88)
  123390:  add  x10, x11, x13      ; min_qp   (0x67be4)
  123394:  add  x11, x12, x13      ; max_qp   (0x67cb6)
  12339c:  add  x13, x19, #0x3c    ; &dsc[60]
  loop (15 iterations, 3 bytes each):
  1233a8:  ldrb w14,[x10,x9]  ; min_qp
  1233ac:  ldrb w15,[x11,x9]  ; max_qp
  1233b0:  ldrb w16,[x8,x9]   ; bpg
  1233bc:  sturb w14,[x13,#-2] ; dsc+58 = min_qp
  1233c0:  sturb w15,[x13,#-1] ; dsc+59 = max_qp
  1233c4:  strb  w16,[x13],#3  ; dsc+60 = bpg   (stride 3)
```

Group 0 (`.rodata+0x67be4 + 0*15` etc.):

```
min_qp:  0  0  1  1  3  3  3  3  3  3  5  5  5  7 13
max_qp:  4  4  5  6  7  7  7  8  9 10 10 11 11 12 13
bpg:     2  0  0 -2 -4 -6 -8 -8 -8 -10 -10 -12 -12 -12 -12
```

This is **byte-identical** to mainline `rc_parameters_pre_scr[]` for
`bpp = DSC_BPP(8), bpc = 8` (`drm_dsc_helper.c:373-382`), i.e. the vendor stores
the standard DSC 1.1 table.

### 3.5 `rc_buf_thresh` immediates (`0x123318`-`0x123370`)

```
  123318:  mov x10,#0xe ; movk #0x1c,lsl16 ; movk #0x2a,lsl32 ; movk #0x38,lsl48
  12331c:  mov x11,#0x46; movk #0x54 ... ; movk #0x62 ...; movk #0x69 ...
  123320:  mov x12,#0x70; movk #0x77 ... ; movk #0x79 ...; movk #0x7b ...
  12333c:  mov w13,#0x7d; movk w13,#0x7e,lsl16
  123358:  stur x10,[x19,#30]   ; dsc+30,32,34,36   (u16 stride 2)
  123360:  stur x11,[x19,#38]   ; dsc+38,40,42,44
  123368:  stur x12,[x19,#46]   ; dsc+46,48,50,52
  123370:  stur w13,[x19,#54]   ; dsc+54,56
```

⇒ `0x0e 0x1c 0x2a 0x38 0x46 0x54 0x62 0x69 0x70 0x77 0x79 0x7b 0x7d 0x7e`,
identical to mainline `drm_dsc_rc_buf_thresh[] >> 6`
(`drm_dsc_helper.c:300`, values 896…8064).

### 3.6 The arithmetic that produces the remaining numbers

Relevant register state and the exact operations (`0x1234a8`-`0x1236a4`, only the
path taken for our configuration — 4:4:4, 1.1):

```
  1234a8:  ldrh w11,[x19,#4]        ; slice_width = 540
  1234c4:  and  w16,w12,#0xffff     ; w12 = dsc+22 = initial_xmit_delay = 170
  1234cc:  and  w13,w8,#0xffff      ; w8  = dsc+28 = initial_offset   = 6144
  1234d0:  add  w8,w20,#0x1 ; strb w8,[x19]      ; line_buf_depth = bpc+1 = 9
  1234dc:  mov  w8,#0x30 ; mov w14,#0x40
  1234e4:  csel w8,w14,w8,hi                     ; bpc > 10 ? 0x40 : 0x30
  1234ec:  strb w8,[x19,#121]                    ; mux_word_size = 48
  12351c:  ldrh w10,[x19,#104]      ; rc_model_size = 8192
  123520:  ldrh w0, [x19,#6]        ; slice_height = 20
  123528:  ldrb w17,[x19,#27]       ; first_line_bpg_offset = 12
  12352c:  add  w12, w11, #0x2      ; 542
  123534:  mul  w14, w21, w16       ; bpp*xmit           = 1360
  123538:  umull x12, w12, #0xAAAAAAAB ; >>33 -> (540+2)/3 = 180   groups_per_line
  12353c:  add  w1, w21, w14        ; 8 + 1360 = 1368
  123540:  sub  w13, w10, w13       ; rcm - init_off = 2048
  123548:  sub  w12, w0, #0x1       ; 19
  12354c:  add  w1, w1, w13         ; 3416
  123550:  mul  w11, w21, w11       ; bpp*slice_width = 4320
  123554:  madd w1, w15, w17, w1    ; 180*12 + 3416 = 5576
  123558:  add  w17, w12, w17, lsl #11 ; 19 + 12*2048 = 24595
  12355c:  tst w11,#7 ; lsr w11,#3 ; cinc        ; chunk = (4320/8) = 540 (round-up)
  123570:  strh w11,[x19,#122]                   ; slice_chunk_size = 540
  123574:  and  w17, w11, #0xffff
  12357c:  sdiv w1, (w1-1), w21     ; 5575/8 = 696
  123580:  lsl  w11, w10, #3        ; 65536
  123584:  mul  w2, w17, w0         ; 540*20 = 10800
  123588:  mul  w17, w15, w0        ; 180*20 = 3600   groups_total
  12358c:  sub  w0, w1, w16         ; 696-170 = 526   initial_dec_delay
  123590:  sdiv w16, w11, w13       ; 65536/2048 = 32 initial_scale_value
  123594:  lsl  w1, w2, #3          ; slice_bits = 86400
  123598:  strb w16,[x19,#108]      ; initial_scale_value  = 32
  12359c:  strh w0, [x19,#24]       ; initial_dec_delay    = 526
  1235a0:  strh w12,[x19,#114]      ; nfl_bpg_offset       = 1294   (see below)
  123628:  add  w9, w8, w20, lsl #2 ; 48 + 4*8 = 80      (convert_rgb path)
  12362c:  add  w9, w9, w9, lsl #1  ; 240
  123630:  add  w9, w9, #0x6        ; 246  num_extra_mux_bits
  123634:  sub  w1, w1, w9          ; 86400-246 = 86154
  123638:  sdiv w2, w1, w8 ; msub    ; remainder vs mux_word_size = 42
  123640:  sub  w8, w1, w8          ; 42-48 = -6
  123644:  add  w8, w8, w9          ; 246-6 = 240   (mux-aligned extra bits)
  123648:  add  w9, w8, w13         ; 240+2048 = 2288
  12364c:  sub  w14, w10, w14       ; 8192-1360 = 6832
  123650:  add  w9, w17, w9, lsl #11
  123654:  add  w8, w8, w14         ; 240+6832 = 7072  final_offset
  12365c:  sub  w10, w10, w8        ; rcm - final_offset = 1120
  123660:  sdiv w9, w9-1, w17       ; slice_bpg_offset = 1302
  123664:  sdiv w10, w11, w10       ; final_scale = 65536/1120 = 58
  123668:  strh w9, [x19,#116]      ; slice_bpg_offset = 1302
  123684:  strh w8, [x19,#118]      ; final_offset     = 7072
  123688:  ubfiz w8, w8, #11, #16   ; 7072<<11 = 14483456
  12368c:  mul  w9, (w10-9), (nfl+slice_bpg)  ; 49 * 2596 = 127204
  123694:  sdiv w15, w15, (isv-8)   ; 180/24 = 7   scale_decrement_interval
  12369c:  strh w8, [x19,#112]      ; scale_increment_interval = 113
  1236a0:  strh w15,[x19,#110]      ; scale_decrement_interval = 7
```

`nfl_bpg_offset` is computed earlier in the same block
(`0x12352c`/`0x12355c`/`0x123568`/`0x12356c`):
`(slice_height-1 + (first_line_bpg_offset<<11) - 1) / (slice_height-1)` = `24594/19`
= **1294**.

Resulting numbers for our configuration:

```
rc_model_size                8192      rc_edge_factor            6
initial_xmit_delay            170      rc_quant_incr_limit0/1   11 / 11
initial_dec_delay             526      rc_tgt_offset_hi/lo      3 / 3
initial_offset               6144      flatness_min/max_qp      3 / 12
final_offset                 7072      first_line_bpg_offset   12
initial_scale_value            32      nfl_bpg_offset        1294
scale_increment_interval      113      slice_bpg_offset      1302
scale_decrement_interval        7      slice_chunk_size       540
num_extra_mux_bits      246 -> 240     groups_per_line        180
```

### 3.7 `_get_dsc_v1_2_bpg_offset` (0x1230a0) — present but **not used for this panel**

```
  1230a4:  ldrh w8, [x0, #6]        ; dsc+6  = slice_height  (NOT bits_per_pixel!)
  1230ac:  cmp  w8, #0x7 ; b.hi 1230c0
  1230b8:  sub  w8, (2*w8), #0x2    ; <=7   -> 2*h-2
  1230c0:  cmp  w8, #0x14 ; b.hs 1230d0  -> 12
  1230d0:  cmp  w8, #0x1f ; b.hs 1230e0  -> 13
  1230e0:  cmp  w8, #0x2a ; cinc   -> 14/15
  1230ec:  ldrb w11,[x0, #128] ; ldrb w11,[x0,#129]
  1230f8:  ubfx w9, w9(#16), #4, #8 ; add w10, w9, w10, uxtb #1 ...
  12312c:  sub  w9, w10, w9*3
  123134:  cmp  w9, w8, uxtb ; csel  -> min(delta, tmp)
```

It is reached only from the `dsc_version_minor == 2` branch (`0x123198`) and, if it
were executed for our geometry, `dsc+6 = slice_height = 20` would land in the
`[0x14,0x1f)` bucket and return **13**. Because the panel is DSC 1.1 the driver
never calls it here, and the payload really carries 12 — see the nfl cross-check
below.

### 3.8 Independent cross-check: the panel's own DSC register in the init sequence

The panel's init sequence (`river.dts:2250`, also in `panel-m19-init.json`)
contains a DCS long write to page-0 register `0xC1`:

```
39 01 00 00 00 00 11 c1 | 89 28 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16
```

Decoding it against the numbers we computed:

| bytes | value | matches |
|---|---|---|
| `89` | `0x89` = `line_buf_depth(9) \| bpc(8)<<4` | **PPS3 / `dsc+0`,`dsc+1`** |
| `28` | `0x28` | **not identified** (see §7 item 3) |
| `00 14` | 20 | `slice_height` |
| `00 aa` | **170** | `initial_xmit_delay` ✓ (proves the `_v1` LUT override is live) |
| `02 0e` | **526** | `initial_dec_delay` ✓ |
| `00 71` | **113** | `scale_increment_interval` ✓ |
| `00 07` | **7** | `scale_decrement_interval` ✓ |
| `05 0e` | **1294** | `nfl_bpg_offset` ✓ (⇒ `first_line_bpg_offset = 12`, not 13) |
| `05 16` | **1302** | `slice_bpg_offset` ✓ |

Seven of the eight decodable fields match the driver's computation exactly. This is
the strongest single piece of evidence in this report: it proves the `_v1` LUT is
used (170), that the 1.1 `first_line_bpg_offset = 12` path is taken (1294), and that
`final_offset`/`scale_increment_interval` follow from it (113).

---

## 4. Question 3 — PPS payload layout (`sde_dsc_create_pps_buf_cmd`, 0x1237fc)

Signature by use: `int sde_dsc_create_pps_buf_cmd(struct msm_display_dsc_info *dsc,
u8 *buf, u32 pps_identifier, u32 buf_len)`; called from `dsi_panel_update_pps`
(`0x16d148`) with `buf = panel+0x737`, `pps_identifier = 0`, `buf_len = 0x80`, and a
`dsi_cmd_desc` at `panel+0x730` holding DTYPE `0x0A` (MIPI DCS `set_pps`), last=1,
128-byte payload.

```
  123814:  cmp  w3, #0x80 ; b.hs ok ; mov w0,#-22    ; buf_len >= 128
  12383c:  bl   memset                               ; zero all 128 bytes
  123840:  ldrb w8,[x19,#126] ; ldrb w9,[x19,#127]
  123848:  strb w21,[x20,#1]                         ; PPS1 = pps_identifier (0)
  12384c:  orr  w8,w8,w9,lsl #4 ; strb w8,[x20]      ; PPS0 = minor | major<<4
  123854:  ldrb w8,[x19,#0] ; ldrb w9,[x19,#1]
  12385c:  bfi  w8,w9,#4,#28 ; strb w8,[x20,#3]      ; PPS3 = linebuf | bpc<<4
  123864:  ...  strb w9,[x20,#5]                     ; PPS5 = bpp[7:0]   (dsc+16)
  12388c:  lsl  w10,w10,#5                           ; block_pred<<5 (dsc+26)
  123898:  orr  w9,w10,w11,lsl #4                    ; | convert_rgb<<4 (dsc+2)
  1238a0:  orr  w9,w9,w13,lsl #2                     ; | vbr_enable<<2  (dsc+120)
  1238a4:  strb w9,[x20,#4]                          ; PPS4
```

Full field→byte mapping recovered from the store immediates (all stores are
`strb wN,[x20,#k]`, so byte offsets are exact):

| PPS byte | source | PPS byte | source |
|---|---|---|---|
| 0 | `dsc+126 \| dsc+127<<4` (minor, major) | 44..57 | `dsc[30],dsc[32],…,dsc[56]` (rc_buf_thresh[0..13], stride 2) |
| 1 | `pps_identifier` arg (=0) | 58..87 | `dsc[58+3i..60+3i]` → 2 bytes/range: `min<<3\|max>>2`, `(max&3)<<6\|bpg` |
| 2 | 0 (memset, never written) | 88 | `version_minor==2` only: 1 if `dsc+128` (4:2:2), 2 if `dsc+129` (4:2:0) |
| 3 | `dsc[0] & 0xf \| dsc[1]<<4` | 89 | `dsc+130` |
| 4 | `bpp[9:8] \| vbr<<2 \| simple422<<3 \| conv_rgb<<4 \| block_pred<<5` | 90 | `dsc+133` |
| 5 | `dsc+16 & 0xff` (bpp low) | 91 | `dsc+132` |
| 6,7 | `dsc+12` (pic_height, LE read, BE emit) | 92 | `dsc+135` |
| 8,9 | `dsc+10` (pic_width) | 93 | `dsc+134` |
| 10,11 | `dsc+6` (slice_height) | 94..127 | 0 |
| 12,13 | `dsc+4` (slice_width) | | |
| 14,15 | `dsc+122` (chunk_size) | | |
| 16,17 | `dsc+22` (initial_xmit_delay, high byte masked `&0x3`) | | |
| 18,19 | `dsc+24` (initial_dec_delay) | | |
| 20 | 0 (never written) | | |
| 21 | `dsc+108 & 0x3f` (initial_scale_value) | | |
| 22,23 | `dsc+112` (scale_increment_interval) | | |
| 24,25 | `dsc+110` (scale_decrement_interval, high `&0xf`) | | |
| 26 | 0 | | |
| 27 | `dsc+27 & 0x1f` (first_line_bpg_offset) | | |
| 28,29 | `dsc+114` (nfl_bpg_offset) | | |
| 30,31 | `dsc+116` (slice_bpg_offset) | | |
| 32,33 | `dsc+28` (initial_offset) | | |
| 34,35 | `dsc+118` (final_offset) | | |
| 36 | `dsc+106 & 0x1f` (flatness_min_qp) | | |
| 37 | `dsc+107 & 0x1f` (flatness_max_qp) | | |
| 38,39 | `dsc+104` (rc_model_size) | | |
| 40 | `dsc+18 & 0xf` (rc_edge_factor) | | |
| 41 | `dsc+20 & 0x1f` (rc_quant_incr_limit0) | | |
| 42 | `dsc+19 & 0x1f` (rc_quant_incr_limit1) | | |
| 43 | `dsc[14]<<4 \| dsc[15]&0xf` (tgt_offset_hi/lo) | | |

### 4.1 Against mainline `drm_dsc_pps_payload_pack()`

`drivers/gpu/drm/display/drm_dsc_helper.c:99-273` writes the same fields to the same
byte offsets for **PPS0 … PPS87**:

* PPS0 `minor | major<<4` — same.
* PPS1/2 zero — same.
* PPS3 `line_buf_depth | bits_per_component<<4` — same.
* PPS4 `(bpp&0x300)>>8 | vbr<<2 | simple422<<3 | convert_rgb<<4 | block_pred<<5` — same.
* PPS5 `bpp & 0xff` — same.
* PPS6..15 = `cpu_to_be16(pic_height)`, `pic_width`, `slice_height`, `slice_width`,
  `slice_chunk_size` — the vendor reads little-endian u16 and emits **high byte
  first**, i.e. big-endian on the wire: same bytes as `cpu_to_be16()` on a LE host.
* PPS16/17 `initial_xmit_delay` hi(2 bits)/lo — same.
* PPS18/19 `initial_dec_delay` BE — same.
* PPS20 zero — same.
* PPS21 `initial_scale_value` — same.
* PPS22/23, 24/25 `scale_increment_interval`, `scale_decrement_interval` — same.
* PPS26 zero, PPS27 `first_line_bpg_offset` — same.
* PPS28..35 `nfl_bpg_offset`, `slice_bpg_offset`, `initial_offset`, `final_offset` — same.
* PPS36..43 flatness min/max, `rc_model_size`, `rc_edge_factor`,
  `rc_quant_incr_limit0/1`, `rc_tgt_offset_hi<<4|lo` — same
  (mainline uses the constants `DSC_RC_EDGE_FACTOR_CONST 6`,
  `DSC_RC_TGT_OFFSET_{HI,LO}_CONST 3`, `drm_dsc.h:22-25`).
* PPS44..57 `rc_buf_thresh[14]` — same.
* PPS58..87 `rc_range_parameters[15]` packed as
  `min_qp<<11 | max_qp<<6 | bpg` over `__be16` — the vendor produces
  `pps[2i]=min<<3|max>>2`, `pps[2i+1]=(max&3)<<6|bpg`, which is the same 16-bit
  big-endian value.

**The only structural difference** is in the DSC-1.2-only tail:

* mainline writes PPS88 = `native_422 | native_420<<1`, PPS89 =
  `second_line_bpg_offset`, PPS90/91 = `nsl_bpg_offset`, PPS92/93 =
  `second_line_offset_adj`, PPS94..127 = 0.
* the vendor writes those six bytes at **PPS88..PPS93** as well (the post-loop
  pointer is `pps+86`, and it stores at `[x9,#2]` … `[x9,#7]`), but only when
  `dsc_version_minor == 2`; the first of them,
  `88 = 1 if dsc+128 (4:2:2) else 2 if dsc+129 (4:2:0)`, matches mainline's
  `native_422 | native_420<<1` encoding. If neither flag is set the vendor stores a
  stale register value there (visible in `0x123adc: csel w8, w11, w8, eq`), i.e. a
  vendor quirk that only affects the DSC-1.2 4:4:4 path.
* For our panel (`minor = 1`) the whole block is skipped (`0x123ab4: cmp w8,#2;
  b.ne 123ac8` → return), so **PPS88..127 are all zero** — identical to what
  mainline produces for this configuration.

Byte-for-byte, the vendor PPS and a stock-mainline PPS differ only in the three
fields affected by the `initial_xmit_delay` override — see §8.

---

## 5. Question 4 — DSI compression-mode registers

### 5.1 The register-write site (verified)

There is no vendor `dsi_update_dsc_timing()`; in this build the DSI compression
registers are programmed by the **SDE interface block**
`sde_hw_intf_setup_te_config()` (`0x10b9cc`), reached through
`sde_hw_intf_ops.setup_te_config` (ops slot `+216`, installed at `0x10ab0c`
in `sde_hw_intf_init`, CFI jump table entry `0x1823c8`). It is the **only** code in
the whole module that writes offset `0x29c`/`0x2a4`/`0x2a8` (grep over the full
`objdump -d`):

```
  123…  (base = hw + 0x30)
  10ba28:  mov w1,#0x288 ; w2 = cfg[0] | (cfg[32]!=0)<<20     ; sde_reg_write
  10ba40:  ldr w2,[x19,#4]   -> reg 0x28c
  10ba58:  ldr w2,[x19,#8]   -> reg 0x294
  10ba70:  ldr w2,[x19,#24]  -> reg 0x2a4   COMMAND_COMPRESSION_MODE_CTRL
  10ba88:  ldr w2,[x19,#28]  -> reg 0x2a8   COMMAND_COMPRESSION_MODE_CTRL2
  10baa0:  ldr w2,[x19,#20]  -> reg 0x2a0
  10bab8:  ldp w9,w8,[x19,#12]
  10bac8:  orr w2, w9, w8, lsl #16
  10bac4:  mov w1, #0x29c ; sde_reg_write   ; VIDEO_COMPRESSION_MODE_CTRL
  10baec:  ldr w8,[x19,#20] ; ldr w9,[x19,#12] ; add w2,(w8+w9)+1 -> reg 0x290
```

and `sde_hw_intf_update_te()` (`0x10bbe8`) later does a read-modify-write of `0x29c`
that only replaces the **low 16 bits**, i.e. WC is programmed once at setup:

```
  10bc0c:  mov w1,#0x29c ; bl sde_reg_read
  10bc18:  ldr w8,[x19,#12]
  10bc1c:  and w9,w0,#0xffff0000
  10bc28:  orr w2,w9,w8 ; mov w1,#0x29c ; bl sde_reg_write
```

### 5.2 Field layout of `0x29c` (from the identical mainline register description)

`drivers/gpu/drm/msm/registers/display/dsi.xml:369`:

```
VIDEO_COMPRESSION_MODE_CTRL (0x29c): WC[31:16], DATATYPE[13:8],
                                    PKT_PER_LINE[7:6], EOL_BYTE_NUM[5:4], EN[0]
COMMAND_COMPRESSION_MODE_CTRL (0x2a4): STREAM0/1 {DATATYPE[13:8]/[29:24],
                                    PKT_PER_LINE[7:6]/[23:22], EOL_BYTE_NUM[5:4]/[21:20], EN[0]/[16]}
COMMAND_COMPRESSION_MODE_CTRL2 (0x2a8): STREAM0_SLICE_WIDTH[15:0], STREAM1_SLICE_WIDTH[31:16]
```

so the vendor's `cfg[12]` = `{DATATYPE, PKT_PER_LINE, EOL_BYTE_NUM, EN}` and
`cfg[16]` = `WC` (bytes per packet). The vendor's split into two 32-bit fields
(`cfg[12]`/`cfg[16]`) with `<<16` re-joining therefore reproduces the mainline
register layout exactly.

### 5.3 What the values must be for our configuration (mainline formula)

Following mainline `dsi_update_dsc_timing()` (`dsi_host.c:948-1005`) with
`slice_count = pic_width/slice_width = 2` — the vendor derives the same number and
stores it at `dsc+3` (`0x16b1ec`-`0x16b1fc`,
`(pic_width + slice_width - 1) / slice_width`), and recomputes it in
`sde_dsc_populate_dsc_private_params` (`0x1236d8`-`0x123724`) — plus
`dsc_slice_per_pkt = 2` (`qcom,mdss-dsc-slice-per-pkt = <2>`) and
`slice_chunk_size = 540`, bpp = 8:

```
total_bytes_per_intf = 540 * 2            = 1080
bytes_per_pkt (WC)   = 540 * 2            = 1080  = 0x438
eol_byte_num         = 1080 % 3           = 0
pkt_per_line         = 2 / 2              = 1     -> log2 field = 1>>1 = 0
datatype             = MIPI_DSI_COMPRESSED_PIXEL_STREAM = 0x0b
=> 0x29c = (0x438<<16) | (0x0b<<8) | (0<<6) | (0<<4) | 1 = 0x0438_0B01
```

Caveat: this is derived from the mainline formula, **not** read out of the vendor
binary — see §7 item (4).

---

## 6. Question 5 — widebus / lanes / byte divider

Verified facts:

1. **No DSI-side widebus DT property exists.** `qcom,widebus-enable`
   (`.rodata+0x48374`) is referenced only at `0x35670`, inside
   `dp_parser_parse` (`0x34674`: `of_find_property` → `strb w8,[x25,#722]`). No DSI
   code path reads it, and `grep -i widebus /tmp/river.dts` returns nothing at all.
2. **4 lanes** for this panel: the DT node carries
   `qcom,mdss-dsi-lane-0-state … lane-3-state` (4 properties).
3. The interface block exposes
   `sde_hw_intf_enable_wide_bus()` (`0x10b708`) = read/modify/write of **register
   `0x60` bit 0**:
   ```
   10b72c:  mov w1,#0x60 ; bl sde_reg_read
   10b73c:  bfxil w2, w19, #0, #1     ; replace bit0 with `en`
   10b748:  mov w1,#0x60 ; bl sde_reg_write
   ```
   and `sde_hw_intf_enable_compressed_input()` (`0x10b668`) which ORs **bit 12
   (`0x1000`) of register `0x60`** (with a HW-version gate on bits `[31:28] >= 7`
   vs `== 6`).
4. `dsi_display_update_byte_intf_div()` (`0x159744`) unconditionally programs
   `byte_intf_div = 2`:
   ```
   159744:  ldr  w8, [x0, #800]      ; index sanity check
   15974c:  cmp  w8, #3 ; b.hs -> brk #0x5512
   159754:  ldr  x8, [x0, #288]
   159758:  mov  w9, #0x2
   15975c:  str  w9, [x8, #1184]     ; byte_intf_div = 2
   ```
   i.e. the byte-interface clock is byte_clk/2 — the configuration widebus needs.
   This function has **no relocation referencing it** inside the module (it is an
   exported helper), so the call site/condition cannot be confirmed from the binary.

Not verified (see §7): whether the runtime path actually asserts
`enable_wide_bus`/`enable_compressed_input` for this panel, and any explicit
`bits_per_pclk` setting (no such symbol or DT property exists in this build).

---

## 7. Question 6 — verification matrix / things I could **not** verify

Verified twice or three times (see table in §1):

* DSC version 1.1 + `scr_ver = 0` — driver default *and* DT property absence
  (and the contrast with a sibling node in the same dts that sets `<0x12>`).
* `table_index = 0` — disassembly compare sequence *and* the decoded
  `sde_dsc_index_map` bytes.
* `initial_xmit_delay = 170` — `.data` LUT bytes, disassembly (`csel` on
  `is_panel == 10` + `strh`), **and** the panel's own `0xC1` register write.
* `initial_dec_delay = 526`, `nfl_bpg_offset = 1294`, `slice_bpg_offset = 1302`,
  `scale_increment_interval = 113`, `scale_decrement_interval = 7` — disassembly
  **and** the panel's `0xC1` payload.
* `first_line_bpg_offset = 12` — code branch, the nfl cross-check (1294 ⇒ 12), and
  the fact that `_get_dsc_v1_2_bpg_offset()` is unreachable for minor = 1.
* `rc_buf_thresh` and `rc_range_params` — immediates/table bytes **and** byte-equal
  to mainline's DSC 1.1 tables.
* PPS0..87 layout — derived per-field from store immediates **and** compared against
  mainline's `drm_dsc_pps_payload_pack()` field by field.

Single-source / open items, explicitly flagged:

1. `simple_422 = 0` and `vbr_enable = 0` come only from the `strb wzr` immediates
   (`0x123270`, `0x12327c`); no second source.
2. The vendor field at `dsc+121` (`mux_word_size`, 48) is single-source (instruction
   immediates only). Its value is corroborated indirectly because the derived
   `num_extra_mux_bits = 240` is what reproduces the panel's 1294/1302/113 numbers.
3. Byte 1 (`0x28`) of the panel's `0xC1` register write is **unidentified**; it does
   not match any value I computed (PPS4 would be `0x30`). It may be a panel-vendor
   enable/version field rather than an RC parameter.
4. **The producer of `cfg[12]`/`cfg[16]` for `DSI_VIDEO_COMPRESSION_MODE_CTRL` was
   not located.** `sde_hw_intf_setup_te_config()` receives an already-packed
   configuration; the encoder-side code that fills it
   (`sde_encoder_phys_vid_enable`, `0xa7b64`, builds the config on the stack and
   branches on a compression flag at `[connector+…+2376] bit 8` around `0xa7e9c`)
   was not decoded to the point where `datatype`/`pkt_per_line`/`eol_byte_num`/`WC`
   can be read off. Therefore the `0x29c` *value* (0x0438_0B01) in §5.3 is computed
   from the mainline formula, **not extracted** from the vendor code.
5. Whether widebus (`INTF 0x60` bit 0) and compressed input (bit 12) are actually
   asserted for this panel is **not verified** — the runtime call site is behind an
   ops pointer and I did not decode the caller.
6. `qcom,mdss-dsi-lane-map`, `qcom,mdss-dsi-phy-timings` etc. are DT-only; no
   additional vendor defaults were found in the binary.
7. `sde_dsc_populate_dsc_private_params()` (`0x1236ac`) computes DSI/encoder-private
   quantities that I decoded only partially: `pkt_per_line = ceil(pic_width/slice_width) = 2`
   (`0x1236d8`-`0x123724`), `slice_chunk_size = (bpp*slice_width+7)>>3 = 540`
   (`0x123720`-`0x12372c`, stored at `dsc+148`), `(chunk*pkt_per_line) % 3 = 0`
   (`dsc+156`) and `(chunk*pkt_per_line+2)/3 = 360` (`dsc+160`),
   `2 << (bpc-8) = 2` (`dsc+184`, same as mainline `drm_dsc_flatness_det_thresh()`),
   plus `dsc+144/152` which involve an input field at `dsc+168` that is written by
   neither function. I could not identify a consumer for these fields inside the
   module, so their exact meaning (eol_byte_num vs. bytes-per-packet bookkeeping)
   is unconfirmed.

---

## 8. Differences vs Linux mainline `drm_dsc_setup_rc_params()`

Reference: `kernel/min-tree/drivers/gpu/drm/display/drm_dsc_helper.c`,
`rc_parameters_pre_scr[]` (lines 357-395), `drm_dsc_setup_rc_params()` (1244-1301),
`drm_dsc_compute_rc_parameters()` (1313-1438), `drm_dsc_set_rc_buf_thresh()`
(303-330), `drm_dsc_pps_payload_pack()` (99-273), constants in
`include/drm/display/drm_dsc.h:22-25`, and the msm glue in
`drivers/gpu/drm/msm/dsi/dsi_host.c:1894-1948`
(`dsc->simple_422 = 0; dsc->convert_rgb = 1; dsc->vbr_enable = 0;` at 1919-1921,
`drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR)` at 1933,
`initial_scale_value = drm_dsc_initial_scale_value(dsc)` at 1945,
`line_buf_depth = bpc + 1` at 1946, `drm_dsc_compute_rc_parameters()` at 1948).

For the same configuration (bpp 8, bpc 8, 540x20 slices, 4:4:4, `convert_rgb = 1`,
mux_word_size 48), mainline computes:

| quantity | stock mainline | vendor | source of the mainline number |
|---|---|---|---|
| `initial_xmit_delay` | **512** | **170** | `rc_parameters_pre_scr` bpp8/bpc8 entry, `drm_dsc_helper.c:373` (`{ 512, 12, 6144, 3, 12, 11, 11, …`) |
| `first_line_bpg_offset` | 12 | 12 | same table |
| `initial_offset` | 6144 | 6144 | same table |
| `flatness_min_qp`/`max_qp` | 3 / 12 | 3 / 12 | same table |
| `rc_quant_incr_limit0/1` | 11 / 11 | 11 / 11 | same table |
| `rc_range_params[15]` | table | identical | `drm_dsc_helper.c:374-381` |
| `rc_buf_thresh[14]` | `0e…7e` | identical | `drm_dsc_rc_buf_thresh[] >> 6`, line 320 |
| `rc_model_size` / `rc_edge_factor` / `rc_tgt_offset` | 8192 / 6 / 3,3 | same | `drm_dsc.h:22-25`, `drm_dsc_set_const_params()` |
| `num_extra_mux_bits` | 246 → 240 | 246 → 240 | `convert_rgb` branch, lines 1343-1360 |
| `slice_chunk_size` | 540 | 540 | line 1338-1340 |
| `initial_scale_value` | 32 | 32 | `drm_dsc_initial_scale_value()`, line 1470 |
| `scale_decrement_interval` | 7 | 7 | line 1366-1368 (180/(32-8)) |
| `nfl_bpg_offset` | 1294 | 1294 | line 1389 |
| `slice_bpg_offset` | 1302 | 1302 | line 1398 |
| **`final_offset`** | **4336** | **7072** | line 1372 (`8192 - (512·128+8)/16 + 240`) |
| **`final_scale`** | 16 | 58 | line 1381 (`65536/(8192-final_offset)`) |
| **`scale_increment_interval`** | **488** | **113** | line 1410 (`final_offset·2048 / ((nfl+slice_bpg)·(final_scale-9))`) |
| `initial_dec_delay` | 526 | 526 | lines 1428-1437 (`hrd_delay - initial_xmit_delay`; 1038-512 and 696-170 coincide) |

The resulting 128-byte PPS differs in exactly three 16-bit fields:

```
byte 16,17 : vendor 00 aa   mainline 02 00    (initial_xmit_delay 170 vs 512)
byte 22,23 : vendor 00 71   mainline 01 e8    (scale_increment_interval 113 vs 488)
byte 34,35 : vendor 1b a0   mainline 10 f0    (final_offset 7072 vs 4336)
```

Everything else (PPS0-15, 18-21, 24-33, 36-127) is byte-identical.

**Concrete action items for a mainline driver that must match this panel byte for
byte** (analysis only — no source was modified):

1. `dsc->first_line_bpg_offset = 12;` — i.e. *drop* the `= 13` override currently
   present in `dsi_populate_dsc_params()`; 13 is the `_get_dsc_v1_2_bpg_offset()`
   answer, which the vendor only uses for `dsc_version_minor == 2`.
2. `dsc->initial_xmit_delay = 170;` after `drm_dsc_setup_rc_params()`, i.e. emulate
   the `sde_dsc_rc_init_param_lut_v1[0]` override; then re-run
   `drm_dsc_compute_rc_parameters()`, which will then produce
   `final_offset = 7072`, `scale_increment_interval = 113`,
   `initial_dec_delay = 526` and the exact vendor PPS.

---

## Appendix A — raw disassembly used

### A.1 `sde_dsc_populate_dsc_config` version dispatch, LUT select, table interleave

```
  123174:	cmp	w1, #0x1
  123178:	strh	w8, [x0, #104]        ; rc_model_size = 0x2000
  12317c:	b.ne	123198
  123180:	cmp	w2, #0x1
  123184:	b.ne	123198
  123188:	cmp	w3, #0x1
  12318c:	b.ne	1231bc
  123190:	mov	w8, #0xf
  1231bc:	mov	w8, #0xc
  1231c0:	strb	w8, [x19, #27]        ; first_line_bpg_offset
  1231c4:	mov	w2, #0x1
  123250:	ldrb	w9, [x19, #128]
  123258:	mov	w8, #0x6
  12325c:	strb	w8, [x19, #18]        ; rc_edge_factor = 6
  123260:	mov	w11, #0x303
  123264:	orr	w8, w10, w9
  12326c:	eor	w8, w8, #0x1
  123270:	strb	wzr, [x19, #8]        ; simple_422 = 0
  123274:	strh	w11, [x19, #14]       ; rc_tgt_offset = 0x0303
  123278:	strb	w8, [x19, #2]         ; convert_rgb = 1
  12327c:	strb	wzr, [x19, #120]      ; vbr_enable = 0
  123284:	ldrh	w11, [x19, #16]       ; bits_per_pixel (1/16) = 128
  123288:	ldrb	w20, [x19, #1]        ; bpc = 8
  1232a8:	lsr	w21, w11, #4          ; bpp = 8
  1232a4:	csinc	w6, w9, wzr, eq       ; mode selector (0 for 4:4:4)
  1232b4:	add	x8, x8, #0x18         ; 24-byte index_map entries
  1232b8:	cmp	x8, #0x180            ; 16 entries
  1232c4:	ldr	w9, [x9, #8]          ; +8  version
  1232dc:	ldr	w11, [x9, #12]        ; +12 bpc
  1232ec:	ldr	w9, [x9, #16]         ; +16 bpp
  1232d0:	ldr	w9, [x10, x8]         ; +0  mode
  123304:	ldr	w9, [x9, #4]          ; +4  scr_ver (version==1 only)
  123314:	ldrsw	x22, [x8, #20]        ; +20 table_index  -> 0
  1233e0:	ldrb	w8, [x23]             ; is_panel
  1233e4:	adrp	x10, sde_dsc_rc_init_param_lut_v1
  1233e8:	adrp	x11, .rodata+0x67e5c   ; sde_dsc_rc_init_param_lut
  123400:	cmp	w8, #0xa
  123404:	csel	x11, x10, x11, eq
  123410:	ldr	w8, [x11]             ; -> dsc+20
  123424:	ldr	w8, [x11, #4]         ; -> dsc+19
  123438:	ldr	w8, [x11, #8]         ; -> dsc+28 (u16)
  12344c:	ldr	w12, [x11, #12]       ; -> dsc+22 (u16) initial_xmit_delay = 0xaa
  123460:	ldr	w10, [x11, #16]       ; -> dsc+130
  123478:	ldr	w13, [x11, #20]       ; -> dsc+134
  12348c:	ldr	w13, [x11, #24]       ; -> dsc+106
  1234a0:	ldr	w13, [x11, #28]       ; -> dsc+107
```

### A.2 PPS packing landmarks (`sde_dsc_create_pps_buf_cmd`)

```
  123840:	ldrb	w8, [x19, #126]   ; version_minor
  123844:	ldrb	w9, [x19, #127]   ; version_major
  123848:	strb	w21, [x20, #1]    ; PPS1 = pps_identifier
  12384c:	orr	w8, w8, w9, lsl #4
  123850:	strb	w8, [x20]         ; PPS0
  12385c:	bfi	w8, w9, #4, #28
  123860:	strb	w8, [x20, #3]     ; PPS3
  123894:	strb	w9, [x20, #5]     ; PPS5 (bpp low)   [w9 = dsc+16]
  1238a4:	strb	w9, [x20, #4]     ; PPS4
  1238a8:	ldrb	w9, [x19, #13] ; strb w9,[x20,#6]    ; pic_height hi
  1238c0:	ldrb	w9, [x19, #11] ; strb w9,[x20,#8]    ; pic_width hi
  1238d0:	ldrb	w9, [x19, #7]  ; strb w9,[x20,#10]   ; slice_height hi
  1238e0:	ldrb	w9, [x19, #5]  ; strb w9,[x20,#12]   ; slice_width hi
  1238f0:	ldrb	w9, [x19, #123]; strb w9,[x20,#14]   ; chunk_size hi
  123900:	ldrb	w9, [x19, #23] ; and #0x3 ; strb w9,[x20,#16]
  12390c:	ldrb	w9, [x19, #22] ; strb w9,[x20,#17]   ; initial_xmit_delay
  123924:	ldrb	w9, [x19, #108]; and #0x3f; strb w9,[x20,#21]
  123950:	ldrb	w9, [x19, #110]; strb w9,[x20,#25]  ; scale_decrement_interval lo
  12399c:	ldrb	w9, [x19, #118]; strb w9,[x20,#35]  ; final_offset lo
  1239d0:	ldrb	w9, [x19, #20] ; and #0x1f; strb w9,[x20,#41]
  1239e0:	ldrb	w9, [x19, #19] ; and #0x1f; strb w9,[x20,#42]
  1239f4:	bfi	w10, w9, #4, #28 ; strb w10,[x20,#43]  ; tgt_offset hi|lo
  1239fc:	ldrb	w9, [x19, #30] ; strb w9,[x20,#44]   ; rc_buf_thresh[0] (stride 2 src)
  123aa0:	add	x12, x9, #0x2   ; 2 bytes per rc_range_params entry
```

### A.3 `is_panel` (panel identification)

```
  159cf4:	adrp	x1, .rodata+0x1622e        ; "m19_36_02_0a"
  159d00:	bl	strstr
  159d08:	adrp	x8, .bss+0x1d43c           ; is_panel
  159d0c:	mov	w9, #0xa
  159d10:	strb	w9, [x8]
```

### A.4 DSC version / scr-version / chroma-format defaults (`dsi_panel_get_mode`)

```
  16ae88:	adrp	x1, .rodata+0x53492         ; "qcom,mdss-dsc-version"
  16ae94:	blr	x22                          ; of_property_read_u32
  16ae98:	cbz	w0, 16aea8
  16ae9c:	mov	w8, #0x101
  16aea0:	strh	w8, [x19, #958]            ; dsc+126/127 = 1/1  <-- default
  16aeac:	ubfx	w4, w8, #4, #4               ; value>>4 = major
  16aeb0:	and	w5, w8, #0xf                ; value&0xf = minor
  16aebc:	strb	w5, [x19, #958]
  16aec0:	b.ne	16c950                      ; major must be 1
  16aec4:	sub	w8, w5, #0x1 ; cmp #0x2       ; minor must be 1 or 2
  16aeec:	adrp	x1, .rodata+0x1c4ce         ; "qcom,mdss-dsc-scr-version"
  16af00:	strb	wzr, [x19, #968]            ; scr_ver = 0 default
  16b100:	adrp	x1, .rodata+0x19293         ; "qcom,src-chroma-format"
  16b18c:	cmp	w8, #0x2 ; b.eq 16b870
  16b19c:	strb	w8, [x19, #960]             ; dsc+128 = 1  (value 1)
  16b870:	mov	w8, #0x1
  16b874:	strb	w8, [x19, #961]             ; dsc+129 = 1  (value 2)
```

### A.5 `sde_hw_intf_setup_te_config` compression registers

```
  10ba70:	ldr	w2, [x19, #24]
  10ba7c:	mov	w1, #0x2a4 ; bl sde_reg_write     ; CMD_COMPRESSION_MODE_CTRL
  10ba88:	ldr	w2, [x19, #28]
  10ba94:	mov	w1, #0x2a8 ; bl sde_reg_write     ; CMD_COMPRESSION_MODE_CTRL2
  10baa0:	ldr	w2, [x19, #20]
  10baac:	mov	w1, #0x2a0 ; bl sde_reg_write
  10bab8:	ldp	w9, w8, [x19, #12]
  10bac8:	orr	w2, w9, w8, lsl #16
  10bac4:	mov	w1, #0x29c ; bl sde_reg_write     ; VIDEO_COMPRESSION_MODE_CTRL
```

### A.6 widebus / compressed input / byte_intf

```
  10b708:  sde_hw_intf_enable_wide_bus:
  10b72c:	mov	w1, #0x60 ; bl sde_reg_read
  10b73c:	bfxil	w2, w19, #0, #1                     ; bit0 = en
  10b748:	mov	w1, #0x60 ; bl sde_reg_write

  10b668:  sde_hw_intf_enable_compressed_input:
  10b694:	mov	w1, #0x60 ; bl sde_reg_read
  10b6a4:	ldr	x8, [x22, #96] ; lsr w8,w8,#28 ; cmp w8,#7   ; HW version gate
  10b6c8:	orr	w2, w0, #0x1000                      ; bit12 = compressed input
  10b6e4:	mov	w1, #0x60 ; bl sde_reg_write

  159744:  dsi_display_update_byte_intf_div:
  15974c:	cmp	w8, #0x3 ; b.hs -> brk #0x5512
  159758:	mov	w9, #0x2
  15975c:	str	w9, [x8, #1184]                      ; byte_intf_div = 2
```

---

## Appendix B — byte dumps

```
=== sde_dsc_index_map  .rodata+0x6803c  (16 x 24 B, size 0x180 per nm) ===
mode,+4,version,bpc,bpp16(=bpp<<4 in the panel struct),table_index
 [0] 0 0         1 8  8   0        <-- selected for m19_36_02_0a
 [1] 0 0         1 10 8   1
 [2] 0 0         1 10 10  2
 [3] 0 1         1 8  8   3
 [4] 0 1         1 10 8   4
 [5] 0 1         1 10 10  5
 [6] 0 0xffffffff 2 8  8   3
 [7] 0 0xffffffff 2 10 8   4
 [8] 0 0xffffffff 2 10 10  5
 [9] 1 0xffffffff 2 8  7   6
[10] 1 0xffffffff 2 8  8   7
[11] 1 0xffffffff 2 10 7   8
[12] 1 0xffffffff 2 10 8   9
[13] 1 0xffffffff 2 10 10  10
[14] 2 0xffffffff 2 8  6   11
[15] 2 0xffffffff 2 10 6   12
(column 5 of the table stores the *integer* bpp; the driver compares dsc+16>>4)

=== sde_dsc_rc_init_param_lut  .rodata+0x67e5c (15 x 32 B, size 0x1e0) ===
entry   [0]=lim0 [1]=lim1 [2]=init_off [3]=xmit [4] [5] [6]=flat_min [7]=flat_max
 [ 0]   0x0000000b 0x0000000b 0x00001800 0x00000200 0 0 0x00000003 0x0000000c
 [ 1]   0x0000000f 0x0000000f 0x00001800 0x00000200 0 0 0x00000007 0x00000010
 [ 2]   0x0000000f 0x0000000f 0x00001600 0x0000019a 0 0 0x00000007 0x00000010
 [ 3]   0x0000000b 0x0000000b 0x00001800 0x00000200 0 0 0x00000003 0x0000000c
 [ 4]   0x0000000f 0x0000000f 0x00001800 0x00000200 0 0 0x00000007 0x00000010
 [ 5]   0x0000000f 0x0000000f 0x00001600 0x0000019a 0 0 0x00000007 0x00000010
 [ 6]   0x0000000b 0x0000000b 0x00001600 0x0000019a 0 0 0x00000003 0x0000000c
 [ 7]   0x0000000b 0x0000000b 0x00000800 0x00000155 0 0 0x00000003 0x0000000c
 [ 8]   0x0000000f 0x0000000f 0x00001600 0x0000019a 0 0 0x00000007 0x00000010
 [ 9]   0x0000000f 0x0000000f 0x00000800 0x00000155 0 0 0x00000007 0x00000010
 [10]   0x0000000f 0x0000000f 0x00000800 0x00000111 0 0 0x00000007 0x00000010
 [11]   0x0000000b 0x0000000b 0x00001600 0x0000019a 0 0 0x00000003 0x0000000c
 [12]   0x0000000b 0x0000000b 0x00000800 0x00000155 0x0c 0x00000200 0x00000003 0x0000000c
 [13]   0x0000000f 0x0000000f 0x00000800 0x00000155 0x0c 0x00000200 0x00000007 0x00000010
 [14]   0x0000000f 0x0000000f 0x00000800 0x00000100 0x0c 0x00000200 0x00000007 0x00000010

=== sde_dsc_rc_init_param_lut_v1  .data+0x77b0 (15 x 32 B) ===
identical to the above except entry 0 dword[3]:
 [ 0]   0x0000000b 0x0000000b 0x00001800 0x000000aa 0 0 0x00000003 0x0000000c

=== sde_dsc_rc_range_min_qp .rodata+0x67be4 (group 0, 15 entries) ===
  0  0  1  1  3  3  3  3  3  3  5  5  5  7 13
=== sde_dsc_rc_range_max_qp .rodata+0x67cb6 (group 0) ===
  4  4  5  6  7  7  7  8  9 10 10 11 11 12 13
=== sde_dsc_rc_range_bpg   .rodata+0x67d88 (group 0, as int8) ===
  2  0  0 -2 -4 -6 -8 -8 -8 -10 -10 -12 -12 -12 -12
```

`nm` sizes confirm the geometry: `sde_dsc_rc_range_{min,max}_qp` 210 B = 14 groups x
15, `_bpg` 210 B, `sde_dsc_rc_init_param_lut` 0x1e0 = 15 x 32 B,
`sde_dsc_index_map` 0x180 = 16 x 24 B.

### B.1 Panel init-sequence cross-check bytes (`river.dts:2250`)

```
39 01 00 00 00 00 11 c1 89 28 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16
^^ dtype=0x39 (DCS long write), len=0x11, wait=0, = 0xc1 + 16 payload bytes
```

---

## Appendix C — reproduction script (`/tmp/dscwork/sim.py`, abridged)

```python
ro = open('rodata.bin','rb').read(); dt = open('data.bin','rb').read()   # extracted sections
u32 = lambda b,o: struct.unpack_from('<I',b,o)[0]
idx_map = [tuple(u32(ro,0x6803c+e*24+k*4) for k in range(6)) for e in range(16)]
lut_v1  = [tuple(u32(dt,0x77b0+e*32+k*4) for k in range(8)) for e in range(15)]
lut_ro  = [tuple(u32(ro,0x67e5c+e*32+k*4) for k in range(8)) for e in range(15)]
minqp = ro[0x67be4:0x67be4+210]; maxqp = ro[0x67cb6:0x67cb6+210]; bpg = ro[0x67d88:0x67d88+210]
# inputs straight from the DT node
bpc, bpp, sw, sh, pw, ph = 8, 8, 540, 20, 1080, 2460
ver_major, ver_minor, scr_ver, is_panel = 1, 1, 0, 10      # is_panel from strstr()
# index-map match -> entry 0, table_index 0  (see sim.py for the loop)
lim0,lim1,init_off,xmit,_,_,fmin,fmax = lut_v1[0]           # 11,11,6144,170,..,3,12
fl = 12 if scr_ver == 0 else 15                             # 1.1 branch
mux = 0x30; gpl = (sw+2)//3; chunk = (bpp*sw+7)>>3
extra = 3*(mux+4*bpc)+6                                     # 246
extra -= mux - (chunk*8*sh-extra) % mux                     # -> 240
isv = (0x2000*8)//(0x2000-init_off)                         # 32
dec = ((bpp + bpp*xmit + (0x2000-init_off) + gpl*fl - 1)//bpp) - xmit   # 526
nfl = (sh-1 + (fl<<11) - 1)//(sh-1)                         # 1294
final_off = 0x2000 - (xmit*128+8)//16 + extra               # 7072
fs = (0x2000*8)//(0x2000-final_off)                         # 58
slice_bpg = (gpl*sh + ((0x2000-init_off+extra)<<11) - 1)//(gpl*sh)      # 1302
sinc = (final_off<<11)//((fs-9)*(nfl+slice_bpg))            # 113
sdec = gpl//(isv-8)                                         # 7
# then pack the 128-byte PPS exactly as in the table in section 4
```

Output of that script for this configuration:

```
index_map match entry=0 table_idx=0
LUT used: lut_v1(.data)[0] = (11, 11, 6144, 170, 0, 0, 3, 12)   (rodata variant = (11, 11, 6144, 512, 0, 0, 3, 12))
first_line_bpg_offset = 12
mux(+121)=48  groups_per_line=180  num_extra_mux_bits=246 -> 240  chunk=540 slice_bits=86400
initial_scale_value=32 initial_dec_delay=526 nfl=1294 slice_bpg=1302 final_offset=7072
(rc_model_size-final_offset=1120) final_scale=58 scale_incr=113 scale_decr=7
```

and, for the comparison table in §8, the stock-mainline run gives
`xmit=512 final_offset=4336 final_scale=16 scale_incr=488 dec_delay=526 nfl=1294
slice_bpg=1302 isv=32 sdec=7`, with PPS differences only at bytes 16/17, 22/23,
34/35.
