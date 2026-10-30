# Vendor line timing follow-up — hypothesis 2 only

Analysis of `/tmp/msm_drm.ko` (ARM64 relocatable ELF, symbols intact; BuildID
`13abad0d9d4517ce6a44e90d62deceb24c7d2fc3`). Addresses below are `.text`
section offsets, not runtime addresses. Read alongside `HANDOFF.md` §3/4/8 and
`vendor-dsc-analysis.md` §7. No source changes or device operations performed.

**Finding: the vendor computes the same 491-cycle horizontal total as mainline.
The current 276180 kHz module setting already reproduces the vendor line time
to about 2.4 ppm. No module-only timing correction is evidenced.**

## Inputs and compressed horizontal active

Default vendor timing node: 1080×2460, 90 Hz, HFP/HSA/HBP = 59/12/60,
VFP/VSA/VBP = 54/10/10, four D-PHY lanes, RGB888 transport, DSC bpp8/bpc8,
slices 540×20. No explicit panel clockrate in that node.

`sde_dsc_populate_dsc_private_params` (`0x1236ac`), inspected only for line-size
bookkeeping (no PPS/rate-control reanalysis):

| Address | Operation / consequence |
|---|---|
| `0x1236c0`, `0x1236e4`, `0x123714` | Load slice width 540 and integer DSC bpp `128 >> 4 = 8`. |
| `0x123704`, `0x12371c`, `0x123724` | Slice count = `(width + slice_width - 1) / slice_width = 2`. |
| `0x123720`, `0x12372c` | Chunk bytes = `(8*540 + 7) >> 3 = 540`. |
| `0x123738`, `0x123744`–`0x123778`, `0x12378c` | Store `dsc+160 = ceil(chunk_bytes * slice_count / 3) = 360`. This is the compressed horizontal active in 24-bit pixel-clock units. |

## Vendor DSI timing registers

`dsi_ctrl_hw_cmn_set_video_timing` (`0x144888`):

- `0x1448a0`–`0x1448c4`: enabled DSC selects the descriptor at `mode+80`;
  `0x1448b4` selects **`dsc+160`** for horizontal active, loaded at `0x144908`.
- `0x144984` loads mode+4/+8 (HBP/HSA); `0x144988` loads mode+12 (HFP).
  `0x144994`–`0x1449a0` computes `HSA + HBP + compressed_active + HFP`.
  No division/scaling of any porch occurs.
- `0x1449a4` and `0x1449bc` write horizontal active start/end to base+0x24:
  start = `60+12 = 72`, end = `72+360 = 432`.
- `0x14498c`/`0x144990`, `0x1449a8`–`0x1449b0` sum vertical active and porches.
- `0x1449c4`/`0x1449c8` subtract one from the horizontal/vertical totals;
  `0x1449e4`/`0x1449f0` pack/write them to base+0x2c.

Thus vendor `Htotal = 360+60+12+59 = 491`, `Vtotal = 2460+54+10+10 = 2534`;
DSI TOTAL low/high halves are **490 / 2533**, exactly the reported mainline
register values. The additional in-place width calculation at
`0x144964`–`0x144978` uses mode+96/+100; the register active-width operand was
already loaded from DSC+160, so it does not change this total calculation.

## Vendor clock calculation

`dsi_panel_get_host_cfg_for_mode` copies the mode into hostcfg+136 at
`0x16cbac`–`0x16cbbc`; `0x16cbd0`–`0x16cbe0` supplies the DSC enable and pointer
at hostcfg+209/+216. This pins the fields consumed below.

`dsi_ctrl_update_host_config` (`0x14ce78`), default video / no explicit clock path:

- `0x14d034`–`0x14d078` counts active lanes (4) and saves transport bpp (24).
  `0x14d06c`/`0x14d07c` checks the explicit bitclock override; absent here.
- The command-mode branch at `0x14d0c4` is bypassed for video.
- `0x14d12c`–`0x14d140` selects **`dsc+160 = 360`**, replacing only horizontal
  active. `0x14d160`–`0x14d17c` adds hostcfg+140/+144/+148 (HBP/HSA/HFP).
- `0x14d168`, `0x14d174`, `0x14d184`, `0x14d190` sum the four vertical fields.
  `0x14d064` loads refresh at hostcfg+180. `0x14d18c`–`0x14d1a0` therefore gives
  aggregate bit rate `fps * 24 * Htotal * Vtotal` and pixel rate `aggregate / 24`.
- D-PHY branch `0x14d1ec` divides aggregate by lanes; `0x14d1f4`–`0x14d204`
  rounds the byte rate to an even integer:
  `byteclk = (floor((lane_bps+4)/8)+1) & ~1`; byte-interface clock divides this
  by hostcfg+76. `0x14d2a4`–`0x14d2c8` passes the byte/interface/pixel rates to
  `dsi_clk_set_link_frequencies` (`0x16fde4`). These are requested rates, not a
  measurement of PLL rounding.

```
vendor pixel clock = 491 * 2534 * 90 = 111,977,460 Hz
vendor lane rate   = pixel_clock * 24 / 4 = 671,864,760 bit/s/lane
ideal byte clock   = lane_rate / 8 = 83,983,095 Hz
vendor rounded byte-clock request = 83,983,096 Hz
line time          = 491 / 111,977,460 = 4.3848110146 us
logical DRM clock  = 1211 * 2534 * 90 / 1000 = 276180.66 kHz
```

## Vendor DPU and mainline comparison

Vendor `sde_encoder_phys_vid_enable` (`0xa7b64`) computes HBP = htotal−hsync_end
at `0xa7e48`; the DSC capability branch at `0xa7ea0`/`0xa7eac` divides only active
width by 3 using multiply/shift at `0xa7eb0`–`0xa7ec8` (1080→360).
`0xa7ef4`–`0xa7f1c` stores unchanged porch/pulse differences. For the single-INTF
DSI path there is no DP widebus horizontal-porch halving (separate type-10 branch
at `0xa7fb8`–`0xa8018`). `sde_hw_intf_setup_timing_engine` (`0x10ac24`) sums
HBP/HSA/width at `0x10ac6c`–`0x10ac90`, then HFP at `0x10ad68`, again producing
491. Compressed/widebus active-data register handling is separate from that sum.

Mainline `kernel/min-tree/drivers/gpu/drm/msm/dsi/dsi_host.c`:

- `dsi_adjust_pclk_for_compression()` at line 604 substitutes
  `ceil(1080*8/(8*3)) = 360` into htotal, preserving porches.
- `dsi_timing_setup()` at line 1034 onward likewise sets compressed active to
  `ceil(1080 bytes*8/24) = 360`; lines 1088 onward program totals minus one.
- `dsi_byte_clk_get_rate()` at line 653 onward requests `pclk*24/(8*4)`.

Mainline `disp/dpu1/dpu_encoder_phys_vid.c:131`–`140` scales width by
`DSC_bpp/(bpc*3)` and leaves DSI porches intact, matching the vendor.

With `clock_khz=276180`, mainline requests:

```
pclk = floor(276180000 * 491 / 1211) = 111,977,192 Hz
byteclk = floor(pclk * 24 / 32) = 83,982,894 Hz
nominal lane rate = 671,863,152 bit/s
refresh = pclk / (491 * 2534) = 89.9997846 Hz
```

The 268 Hz pixel-clock difference is normal integer-kHz truncation (~2.4 ppm).
Setting 276181 kHz merely moves to the other side of the rounding error; it is
not an evidenced corruption fix. Do not divide all porches by three or multiply
the module clock by three: neither follows the vendor path. Hypothesis 2's
proposed horizontal-total scaling error is unsupported by this binary. Actual
PLL output/readback remains unmeasured; the conclusion concerns the driver
arithmetic and register geometry, not a claim of measured electrical timing.
