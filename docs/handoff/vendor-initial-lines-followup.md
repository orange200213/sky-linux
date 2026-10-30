# Vendor initial lines and DPU DSC 1.2 comparison

For the full-frame m19 configuration (1080x2460, one encoder, two 540x20 soft slices, DSC 1.1 RGB, 8bpc/8bpp, initial_xmit_delay=170), **the vendor computes initial_lines=3; current mainline computes 2**. This is a static reconstruction of `/tmp/msm_drm.ko`, not a vendor runtime register read. No code or device changes were made; PPS and planes were not re-audited.

## Caller evidence

All vendor addresses below are `.text` section offsets in the relocatable ARM64 module. The calculation is in `sde_encoder_dce_setup` (start `0xa584c`, size `0x1460`), specifically `0xa62dc–0xa63a4`.

The DSC-info base is encoder+3976 (`0xa62b4`: `add x13,x19,#0xf88`). Thus:

| Encoder offset | DSC-info offset | Input/value |
|---|---|---|
| 3977 | +1 | bpc=8 |
| 3980 | +4 | slice_width=540 |
| 3992 | +16 | bpp=128, shifted right four to 8 |
| 3998 | +22 | initial_xmit_delay=170 |
| 4098 | +122 | chunk_size=540 |
| 4116 | +140 | computed initial_lines |
| 4148 | +172 | soft slices per encoder=2 |

At `0xa6280`, the slice-count byte at encoder+3979 (DSC+3) is loaded; `0xa62a0` stores it to encoder+4148. The following optional right shift is skipped for this topology: full-frame width makes the half-width flag zero; `0xa6afc–0xa6b08` sees mode bit1 clear and branches straight to `0xa62d0`. Consequently +172 remains 2.

For RGB (native flags zero), the arithmetic selects 64 at `0xa6324`, and retains width=540 and bpp=8. At `0xa632c`, the product of mode bits0/1 is zero for video mode4, so execution continues through the delay adjustment. At `0xa6340–0xa6350`, `cmp soft_slices,1; ccmp 64,8,#0,gt; csel w15,w15,w17,hi` **preserves w15=chunk*8=4320** because 2>1 and 64>8. It does not select the alternative 3780.

The remaining arithmetic is:

```text
mux_pixels = floor((8 + 48 + 9*64 - 1) / 8) = 78
rate_pixels = floor((8 + 4320 - 1) / 8) = 540
numerator = slice_width + 279*soft_slices + initial_xmit_delay
            + mux_pixels + rate_pixels + 28
          = 540 + 558 + 170 + 78 + 540 + 28 = 1914
initial_lines = floor(1914 / 540) = 3
```

`0xa637c` contains the `279*soft_slices + width` multiply-add; `0xa6388` and `0xa6390` produce 78 and 540; `0xa6394–0xa63a0` add the remaining terms and divide by width. `0xa63a4` stores the result to encoder+4116 = DSC+140.

The DPU DSC 1.2 config at `0x120554` (size `0x3ec`, symbol suffix `$b46e25d6a9148e14877c4a661eeda8be`) consumes this byte at `0x1205c0` and writes ENC_DF_CTRL at `0x120654`. The older config at `0x11fcf4` reads the same field as a word at `0x11fd0c`, packing it at bit20 in its different register layout. The fourth config argument is a boolean override, **not initial_lines**: the caller loads it from stack+92 at `0xa6590` before its indirect config call at `0xa6598`.

Mainline `kernel/min-tree/drivers/gpu/drm/msm/disp/dpu1/dpu_encoder.c:1969` instead computes ceil((84*3+170+47+84*3)/540)=2. Therefore 2 does not match the vendor calculation; the earlier handoff's initial-lines hypothesis now has a concrete vendor value to compare.

## Register comparison, same full-frame inputs

Compared directly against `dpu_hw_dsc_1_2.c:87–244`:

| Register | Vendor vs current mainline |
|---|---|
| DSC_CMN_MAIN_CNF (+0x00) | Both mode low bits plus active slices at bits8:7: mode4 and two slices gives 0x100. |
| ENC_DF_CTRL (enc+0x00) | **Vendor 0x12bc0203 vs mainline 0x12bc0202**, assuming ordinary output-buffer capability flags. Difference is initial_lines low byte. Both use video bit9 and max address1199 at bit18. |
| DSC_MAIN_CONF (enc+0x30) | Same DSC1.1 version, fixed-point bpp shift10, block prediction bit20, RGB bit4, line depth shift6 and bpc fields. |
| enc+0x34 through +0x64 | Same field packing for picture/slice/chunk sizes, HRD delays, RC scale/intervals/offsets, flatness and RC config, when supplied the same parameters. Vendor flatness threshold comes from DSC+184; private-param code `0x123710–0x123728` sets `2 << (bpc-8)`, matching mainline for bpc8. |
| DSC_CFG (ctl+0x04) | Same normal RGB/8bpc/video/full-frame value0x801. Vendor has an additional conditional padding facility; see below. |

Other visible vendor facilities are conditional, not established full-frame differences:

* `0x1205dc–0x1205e4` ORs ENC_DF_CTRL bits10/11 (0xc00) when fourth argument bit0 is set. Caller `0xa63a8–0xa63c8` sets that boolean only when its half-width flag is set, slice_count>=2, and slice_width==picture_width. Full-frame m19 has 540!=1080, so it is zero. Mainline lacks this override, but it does not explain the full-frame mismatch.
* `0x1205e8–0x12063c` selects output-buffer sizes using vendor capability bits4/5. For two slices the possible max addresses are299,599,1199; with neither capability set it selects1199, matching mainline's `2400/2-1`. Mainline SM4450 catalog has no DSC feature flags; the vendor capability value was not recovered here, so another output-buffer difference is not proven.
* `0x1208e0–0x120908` optionally adds DSC_CFG bit16 and fields at shifts18/22 if DSC-info+196 is set. This appears to be a padding facility (private-parameter code derives +200/+204), absent in mainline. Its enable is not established for this panel, so no active register mismatch is claimed.

The supported concrete finding is **initial_lines 3 vs 2**. The remaining ordinary DSC 1.2 config packing inspected matches for the stated inputs; conditional facilities should not be presented as demonstrated active differences.

## Experiment deployment

Built Image/modules and repacked via mklinuxpayload + build-sky.sh. Artifact `build/dsc-initial-lines3/sky-initial-lines3.img`, SHA256 `6040dcea6ddf98a038ed762063721580216ba382fb86263cb5e80d0e67302126`. Diagnostic source patch `build/dsc-initial-lines3/initial-lines3.patch` applies3 only to matching panel DSC geometry/parameters. This is a local A/B override, not an upstream-ready general algorithm.

Flashed only boot_a; fastboot write succeeded, set_active a and reboot succeeded. Restored panel module SHA25661ba59c0... (long writes, NO_EOT_PACKET), clock276180. Runtime confirms initial_lines=3 at all three DSC configuration calls, PPSixd170/final7072/scale_inc113 unchanged, flags0xa03, mode90Hz, PPSerr0, IDc01100/power9c. Wrote all10627200 bytes of quadrant pattern to fb0. Log `build/dsc-initial-lines3/runtime.log`. Awaiting user visual result. Fallback kernel image `shim/out/sky-m6n-dsi-regs.img` remains available.
