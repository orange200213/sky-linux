# DSC slice-boundary and runtime status audit

User clarified the visible seam is approximately50%, not45%; SVG conversion caused the earlier estimate. This aligns with two540px slices in a1080px picture.

Decoded active DPU DSC registers:

```
PIC       1080x2460
SLICE     540x20
CHUNK     540
BPP       128/16 = 8
BPC       8
XMIT      170
DEC       526
INITIAL_LINES 3
```

A read-only runtime status snapshot was taken with `build/dsc-register-audit/read-dsc-status`. At encoder offsets104/108/10c it read `0x00000042`, `0x005f000d`, `0x005d0011`; offsets114-12c and19c-1bc were zero. These registers are status/interrupt-related, but no bitfield interpretation has been established from the local source; do not label them as errors. The configured geometry and all established PPS values match expected vendor values. No display-affecting change was made.

## Immediate-source conclusion (bounded)

`kernel/min-tree/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_dsc_1_2.c` only declares these offsets (`+0x04` general status, `+0x08` horizontal-slice status, `+0x0c` output status, `+0x10` interrupt status) and never reads, decodes, masks, or compares them. Its configuration path writes encoder control/configuration registers and does not define any status bit names or error predicates. The vendor `/tmp/msm_drm.ko` contains the DSC v1.2 setup symbols (`sde_dsc1_2_setup_ops`, `sde_hw_dsc_config`) but no locally recoverable status-bit definitions or status interpretation.

Therefore the snapshot is an observation only: `+0x104 = 0x00000042`, `+0x108 = 0x005f000d`, `+0x10c = 0x005d0011`, and `+0x114..0x12c = 0`. From this immediate code, none can be classified as an error or as normal progress. In particular, the zeroes at `+0x114..0x12c` are not evidence of a fault, because those addresses are beyond the four status offsets requested and the source gives no meaning to them. Treat this as “no source-backed status verdict”; a definitive verdict requires the matching hardware register specification or a vendor status consumer.
