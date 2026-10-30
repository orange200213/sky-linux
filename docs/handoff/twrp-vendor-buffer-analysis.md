# TWRP vendor DSC output-buffer analysis

## Scope

This is a bounded comparison of `build/twrp-msm_drm.ko` against
`kernel/min-tree/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_dsc_1_2.c`. No source code
was changed. The module is ELF64 AArch64 relocatable; the relevant newer DSC-1.2
config entry is symbol `sde_hw_dsc_config$b46e25d6a9148e14877c4a661eeda8be` at
`.text+0x120510` (size 0x3ec). The bounded command used was:

```sh
aarch64-linux-gnu-objdump -d --start-address=0x120510 \
  --stop-address=0x120650 build/twrp-msm_drm.ko
```

## Mainline calculation

The min-tree helper is explicit:

```c
int max_addr = 2400 / num_softslice;
if (hw_dsc->caps->features & BIT(DPU_DSC_NATIVE_42x_EN))
        max_addr /= 2;
return max_addr - 1;
```

`dpu_hw_dsc_config_1_2()` derives `num_active_slice_per_enc` from
`dsc->slice_count`, halves it only in multiplex mode, and writes
`_dsc_calc_output_buf_max_addr(...) << 18` into `ENC_DF_CTRL`.

## Vendor disassembly: capability branches and bit width

At `0x1205a4` the vendor compares the active-slice value (`dsc+172`) with 2 and
1. At `0x1205a8`/`0x1205cc` it tests capability bit 4; at `0x1205b8`,
`0x1205d4`, and `0x1205ec` it tests capability bit 5 (`tst x9,#0x20`). The
selected constants are already shifted for the register field:

| branch family | constants seen in disassembly | `constant >> 18` |
|---|---:|---:|
| 1-slice, capability bit 5 set/clear | `0x12bc0000` / `0x257c0000` | 1199 / 2399 |
| 2-slice, capability bit 5 set/clear | `0x095c0000` / `0x12bc0000` | 599 / 1199 |
| capability bit 4 path, 2-slice | `0x04ac0000` / `0x095c0000` | 299 / 599 |

(The exact selection order is the `tbnz`/`tst` and `csel` sequence at
`0x1205a8..0x1205fc`; do not infer it from symbol names.) The constants prove
that the output-buffer maximum is a **14-bit register field at bits [31:18]**:
`0x12bc0000 >> 18 = 1199`, `0x04ac0000 >> 18 = 299`; the low register bits
remain available for initial-lines/video/control flags. This is not a 10-bit
field and is not a byte-sized value.

The vendor then ORs this selected value with the control word at `0x120600` and
writes it through `sde_reg_write` at `0x12060c..0x120610`. The live readings
reported for `ENC_DF_CTRL` are:

* vendor: `ae81100 = 0x04ac0203` => `(value >> 18) & 0x3fff = 299`;
* mainline: `ae81100 = 0x12bc0203` => `(value >> 18) & 0x3fff = 1199`.

Both have the same low control value `0x0203`; only the output-buffer capacity
field differs. Thus the mismatch is not initial-lines, video mode, or a write
address issue.

## Panel and topology facts

The active panel is 1080x2460 DSC video, 8 bpc/8 bpp, two 540x20 slices per
packet. The vendor DT metadata records `qcom,display-topology = <1 1 1>` and
DSC block metadata: `qcom,sde-dsc-linewidth = <0xa00>` (2560),
`qcom,sde-dsc-native422-supp = <1>`, encoder base `0x100`, control base
`0xf00`, DSC revision `dsc_1_2`. The panel's DSC source is nevertheless the
DSC-1.1/4:4:4 path (no `qcom,mdss-dsc-version` or native chroma property in the
panel node); `native422-supp` is a hardware capability, not proof that this
frame is native 4:2:2.

## Smallest grounded fix recommendation (analysis only)

Do **not** change panel timing, slice geometry, topology, or the DT
`native422-supp` capability merely to make the register match. The live vendor
value demonstrates that this vendor DPU path deliberately selects native
output-buffer capacity (`299`) for the active capability branch, while the
current mainline path selects `1199`. The smallest grounded mainline fix is a
DPU DSC capability/branch correction so the active SM4450 DSC instance uses the
vendor's native buffer-capacity branch (the branch that produces 299), while
retaining the existing `<< 18` field placement and the 0x0203 control bits.

Because the observed values establish a 4x capacity difference (299 versus
1199, both encoded in the same [31:18] field), a generic constant change to
`2400` would be unjustified. First make the capability predicate/instance
selection match the vendor; only if a runtime readback then remains 1199 should
an explicit native-capacity divisor be considered. This preserves YAGNI and
avoids editing code in this handoff.

Validation target after any future implementation: with the same panel and
`<1 1 1>` topology, read `ae81100`; expected vendor-compatible value is
`0x04ac0203`, and `(reg >> 18) & 0x3fff` must equal 299.

## Runtime test outcome

Vendor baseline extracted from TWRP: `build/twrp-live.fdt` (SHA256 772cd154f8e007914c762e589d2e222676708d1ae2b8449e5d3b348fecb02199), module `build/twrp-msm_drm.ko` (d050e7f63d721798f2f3d31cee85a03a9e33cd58667786e03038c7f5cef881d8). Vendor encoder/RC snapshot: `build/twrp-vendor-dsc-active.txt`. Vendor awake DSI WC=1080 packetization `04380b01`; compressed interface CONFIG2=1011 DATA_HCTL=00fb0048, matching mainline.

Boot_a was backed up to `build/twrp-dsc-buffer/boot_a-before-tests.img` (128MiB, SHA256 ae839dcadc2e6f3f0834da66a4f0800467261caca2e6661ceac5f4f634ce627b). Known mainline baseline `build/phy-vendor-timing/sky-vendor-phy.img` was written ONLY to boot_a, resolved /dev/block/sde13. Boot verified with kernel7.3, two active slices/encoder, initial_lines3, PPS success and WC1080.

Guarded runtime tool `build/twrp-dsc-buffer/test-buffer` changed ae81100 from12bc0203 to04ac0203 and verified readback. User reported essentially no visual change. Restored12bc0203 with confirmed readback. This rules out an immediate improvement from the live buffer-only change; a cold initialization experiment remains distinct and untested.

Subsequent attempted read of DSC common base ae80000 (rather than previous ae81000 alias) was followed by loss of SSH and USB enumeration. Causality is unconfirmed: helper transfer/run returned255 with no output and serial was unavailable. Do NOT repeat this base read without resolving access safety. No persistent kernel source changes made in this test. Device currently requires connection/recovery check before further tests.

## Cold startup299 outcome — clear improvement

Built a diagnostic SM4450-only override `max_addr=600/num_softslice-1`; same baseline DTB, Image load0xc0000000 and DTB0xc4a00000. Image `build/twrp-dsc-buffer/sky-buffer299-startup.img` SHA25650846c5b4503e78302ed39e8ca388351e42243c4027cbe470ad31b95e6e0a840 deployed ONLY boot_a. Boot logs confirm max_addr299 slices2 from first modeset, PPS success, and ENC_DF_CTRL readback04ac0203.

User reports clear improvement: no flicker; uniform diagonal red/green alternating stripes in upper half, blue/white diagonal stripes in lower half; kernel text was visible before test pattern. Full display still incorrect. This distinguishes initialization-time test from the earlier no-effect live write; exact reason not established.

User requested backup and waiting for SVG. Complete reproducible artifacts at `build/twrp-dsc-buffer/improved-state/` with README and SHA256SUMS. Panel module retrieved from device SHA256b26ea6bbb0eddafa96b0654046697b44029f841470d59c09ab56fcee6bb2ee1f. Preserve running state and do not proceed with more tests until user's visual evidence/steering.

## Pattern stride correction

User reports pure white and pure green display normally. Runtime `/sys/class/graphics/fb0/stride`=4352, virtual_size1080,2460, bits_per_pixel32. Original pattern.rgb uses tightly packed4320-byte rows; direct dd causes progressive row misalignment (32 bytes/row), explaining diagonal red/green and blue/white patterns. Do not attribute these diagonal bands to DSC corruption. Future pattern writes must pad each row to4352 and write10705920 bytes. Solid white/green tests used tightly packed input too, so they alone are not complete spatial verification. Switched toVT1, cleared console, and printed actual last60 dmesg lines to/dev/tty1 at user's request. No display register changes.
