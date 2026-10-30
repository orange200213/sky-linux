# M5 — Display bring-up (DPU 8.6 + DSI 2.6 + Xiaomi "m19" panel)

The panel works: the framebuffer console prints normally and a test pattern renders
correctly at 1080x2460, 90 Hz, with DSC enabled.

This page is the short version. The full record, including the vendor reverse
engineering and every failed experiment, is in [`docs/handoff/`](handoff/HANDOFF.md).

## Hardware

| Item | Value |
|---|---|
| Panel | Xiaomi "m19", 6.79" IPS LCD, 1080x2460, 90 Hz, 24 bpp, 4 DSI lanes |
| Service part | B16790013 (also used by Redmi 12 4G/5G, Redmi 13 4G/5G, Note 12R, POCO M6 Pro 5G) |
| Backlight | Kinetic KTD3136 on i2c1 `0x36`, HWEN on TLMM gpio6, range 0..2047 |
| Supplies | `vddio`, `extvdd`, `avdd`, `ibb` (kept always-on during bring-up) |
| Reset | TLMM gpio92, active low |
| DSI | `dsi@ae94000`, `qcom,sm4450-dsi-phy-5nm` |

## What the panel needs

DSC is mandatory for this panel. The configuration below is what the vendor driver
programs and what mainline now matches:

```
DSC 1.1, 444, 8 bpp, 8 bpc, block prediction on
slice 540x20, 2 slices, one 1080-byte packet per line
initial_xmit_delay 170   <- vendor override (Xiaomi lut_v1), standard value is 512
  => derived: initial_dec_delay 526, final_offset 7072,
              scale_increment_interval 113, nfl_bpg_offset 1294
first_line_bpg_offset 12, rc_model_size 8192, rc_edge_factor 6,
rc_tgt_offset 3/3, initial_offset 6144, slice_chunk_size 540
```

Those last values are not guesses: they come from decompiling the vendor
`msm_drm.ko` and are corroborated by the panel's own init command `0xC1`, which
carries the same numbers.

## The bug that kept the image corrupted

`drivers/gpu/drm/msm/disp/dpu1/dpu_hw_dsc_1_2.c` computes the DSC rate-buffer
capacity as `2400 / num_softslice`. The SM4450 DSC block only has **600 bytes**, so
with two soft slices the hardware field must be `600 / 2 - 1 = 299`, not
`2400 / 2 - 1 = 1199`.

With 1199 the encoder believed it had four times the real buffer, kept emitting data
the block could not hold, and every line reached the DSI — and the panel — malformed.
Nothing was reported on the DSI link, because the corruption happened before the PHY.
The visible result was banded noise whose layout tracked the DSC slice geometry: the
split landed exactly on the slice boundary at x = 540, and the pattern changed
whenever `slice_width`, `slice_count` or `slice_per_pkt` changed.

The field only matters at start-up: writing it live over MMIO after the encoder had
started changed nothing, while programming it before the first modeset fixes the
image.

Patch: [`kernel/patches/0003-dpu-dsc-sm4450-output-buffer-size.patch`](../kernel/patches/0003-dpu-dsc-sm4450-output-buffer-size.patch)

```c
	/* SM4450 exposes a 600-byte DSC rate buffer, not 2400 */
	if (of_machine_is_compatible("qcom,sm4450"))
		max_addr = 600 / num_softslice;
```

## Working log

```
sky-m19:   mode 1080x2460@90 clock=276180 kHz
sky-phy:   vendor m19 timings, bitclk=671863152
sky-dscparams: v1.1 bpp=128 bpc=8 fl_bpg=12 ixd=170 dec=526 init_off=6144
               final_off=7072 scale_inc=113 scale_dec=7 nfl=1294 chunk=540
sky-tsetup:   flags=0x2a03 dsc=set widebus=1
sky-dsi-dsc:  slice_per_intf=2 chunk=540 total_bytes=1080 bytes_per_pkt=1080
              eol=0 pkt_per_line=1 slice_per_pkt=2
sky-dsiregs:  VID_COMP=04380b01 TOTAL=09e501ea ACT_H=01b00048 ACT_V=09b00014
sky-intf:     cfg2 wrote 00001011 read 00001011 (widebus=1 compression=1)
sky-panel:    vendor-long-write init; PPS sent, err=0
sky-panel:    dcs read id=3 [c0 11 00] pwr=1 [9c]
sky-dsc-buffer: max_addr=299 slices=2
Console:      switching to colour frame buffer device 135x153
```

`VID_COMP = 0x04380b01` means: datatype 0x0b, one packet per line, no EOL padding,
compression enabled, and a 1080-byte packet — i.e. both 540-byte slices in one
packet, exactly what the vendor programs.

## Files

| Path | Purpose |
|---|---|
| `kernel/drivers/panel/panel-xiaomi-m19.c` | panel driver (DSC config, init sequence, DCS backlight) |
| `kernel/drivers/backlight/ktd3136.c` | KTD3136 backlight driver |
| `kernel/patches/0003-dpu-dsc-sm4450-output-buffer-size.patch` | the display fix |
| `docs/handoff/` | full bring-up record, vendor analysis, handoff material |

## Still to do on the display

* Turn the working recipe into upstreamable patches (catalog field instead of the
  machine-compatibility check, PHY timings through DT, proper DT binding).
* Single backlight device and a brightness curve; suspend/resume with panel sleep
  in/out.
* Optional 60 Hz mode.
