# Vendor DSI packetization follow-up

Read-only ARM64 reverse engineering of `/tmp/msm_drm.ko`, ELF relocatable with symbols intact, Build ID `13abad0d9d4517ce6a44e90d62deceb24c7d2fc3`. All instruction addresses below are `.text` offsets in this module, not runtime virtual addresses. Only this report was written; no code or device changes were made for this analysis.

## Verified result

For Xiaomi sky m19, width 1080, slice width 540, slice height 20, bpc 8, bpp 8, and DT `qcom,mdss-dsc-slice-per-pkt = <2>`:

- Slices per line: **2**.
- Slices per packet: **2**.
- Actual packets per line: **1**.
- Chunk bytes per slice: **540**.
- Packet word count / payload bytes: **1080 = 0x438**.
- End-of-line bytes: **0**; compressed pixel clocks per line: **360**.
- Actual vendor DSI video compression register value: **`0x04380b01`**.
- Register location: raw DSI controller base **`+0x2a0`**, equivalent to mainline shifted DSI logical register **`0x29c`**. With the repository's DSI0 resource base `0x0ae94000`, this is **`0x0ae942a0`**.

The prior interpretation of `dsc+3` as packet count, and resulting `pkt_per_line=2` / `WC=540`, is incorrect for this DT configuration. The actual DSI producer consumes the private fields at `dsc+144/+152/+156`, not the byte at `dsc+3`.

## DT initialization and pointer identity

The parser is inlined into `dsi_panel_get_mode` (symbol starts `0x16a564`). Here `x19` is the mode-private allocation; DSC starts at `x19+832` (`0x340`), established explicitly by `add x26,x19,#0x340` at **`0x16b1d4`**.

1. **`0x16afc8/0x16afd0`** load `.rodata+0x2c44`, verified by reading the ELF string as **`qcom,mdss-dsc-slice-per-pkt`**. `x2=fp-40` is the output buffer, and **`0x16afd4`** calls the property reader.
2. **`0x16afdc`** loads the result into `w3`. **`0x16afe0–0x16afe8`** accept values 1 or 2 (`unsigned(value-1)<2`); other values enter the error path.
3. **`0x16b004: str w3,[x19,#1000]`** stores the DT value. Since `1000-832=168`, this is exactly **`dsc+168=2`**.
4. **`0x16b1ec–0x16b1fc`** compute `(width+slice_width-1)/slice_width` and store its low byte at `[x19,#835]`, i.e. **`dsc+3` = slice count = 2**. There is no division by slices per packet in this computation.
5. **`0x16b208–0x16b210`** pass `x0=x26` (DSC), `w1=w28` (interface width) to `sde_dsc_populate_dsc_private_params`.
6. **`0x16b23c`** sets mode-info byte `+73` (DSC enable), and **`0x16b240`** stores `x26` at mode-info **`+80`**, precisely the pointer later consumed by the hardware function.

## Private packet-field computation

`sde_dsc_populate_dsc_private_params` starts at **`0x1236ac`**. In the following formulas, integer arithmetic follows the positive panel inputs; bpp is stored in fixed point with four fractional bits.

```text
N = (interface_width + slice_width - 1) / slice_width
B = (((dsc.bits_per_pixel >> 4) * slice_width) + 7) >> 3
S = dsc[168]; if S > N, use S = 1
packets_per_line = N / S
bytes_per_packet = B * S
total_bytes_per_line = B * N
eol_bytes = total_bytes_per_line % 3
pclk_per_line = (total_bytes_per_line + 2) / 3
```

The assembly uses `sdiv` for the divisions. For this panel `N=2`, `B=540`, `S=2`, so division is exact.

| DSC offset | Proven operation / instruction offsets | Result |
|---|---|---:|
| `+168` | `0x123730` loads slices per packet; compare at `0x123764`; `csinc ... le` at `0x123770` retains it when `S<=N`, otherwise selects 1 | 2 |
| `+144` | `0x12377c` divides `N/S`; `0x123794` stores first word | 1 packet/line |
| `+148` | `0x123720/0x12372c` calculate chunk bytes; `0x123794` stores second word | 540 |
| `+152` | `0x123780` multiplies chunk bytes by effective S; `0x123790` stores first word | 1080 bytes/packet |
| `+156` | `0x123738`, `0x12374c–0x123784` calculate `(B*N)%3`; `0x123790` stores second word | 0 |
| `+160` | `0x123744/0x123748/0x123768–0x123778` calculate ceil(total/3); `0x12378c` stores first word | 360 |

`dsc+3` and private `dsc+144` therefore have different meanings even though both can equal 2 with slices-per-packet 1.

## Actual video compression register producer

The function is **`dsi_ctrl_hw_cmn_set_video_timing`**, start **`0x144888`**, not a separately named `setup_dsc` function.

The callback's first argument is the DSI controller hardware object. **`0x1448a0`** checks mode-info `+73`; **`0x1448ac`** loads its DSC pointer from `+80`. For enabled, non-null DSC:

```asm
1448b4: add x8,  x12, #0xa0    // dsc+160: compressed pclk/line
1448b8: add x9,  x12, #0x98    // dsc+152: bytes/packet
1448bc: add x10, x12, #0x90    // dsc+144: actual packets/line
1448c0: add x11, x12, #0x9c    // dsc+156: EOL bytes
1448f4: ldr w10, [x10]
1448f8: ldr w11, [x11]
1448fc: ldr w9,  [x9]
144900: ldr x13, [x0]         // raw mapped DSI controller base
144904: lsl w10, w10, #5
144908: ldr w8, [x8]
14490c: lsl w11, w11, #4
144910: and x10, x10, #0xffffffc0
144914: lsl w9, w9, #16
144918: orr x10, x11, x10
14491c: mov w12, #0xb01
144920: orr x9, x10, x9
144924: orr x20, x9, x12
144928: add x9, x13, #0x2a0
144930: str w20, [x9]         // actual MMIO write
```

Exact packing, for these 32-bit input fields:

```text
reg = (bytes_per_packet << 16)
    | ((packets_per_line << 5) & 0xffffffc0)
    | (eol_bytes << 4)
    | 0x00000b01
```

For supported packet counts 1, 2, 4, the packet term is `(packets_per_line >> 1)<<6`, giving encodings 0, 1, 2. Thus one packet produces zero in bits `[7:6]`; it does not produce a literal 1 in that field. `0xb01` supplies compressed pixel stream datatype `0x0b` and compression-enable bit 0.

For the panel:

```text
(1080 << 16) | ((1 << 5) & 0xffffffc0) | (0 << 4) | 0xb01
= 0x04380b01
```

The video path of `dsi_ctrl_timing_setup` selects the video timing callback at **`0x14b71c`**, passes hardware object `ctrl+96` and mode-info `ctrl+0x630` at **`0x14b730–0x14b738`**, then calls it. This verifies the register writer belongs to DSI video timing setup.

## Address spaces and the four-byte shift

The vendor probe calls `msm_ioremap` for resource string **`dsi_ctrl`** at **`0x1515f8–0x151608`** and stores the returned pointer directly at controller-object `+96` at **`0x15164c`**. That is the hardware object's first member, loaded by **`0x144900`** above. There is no four-byte subtraction in this assignment.

Mainline intentionally shifts its software base:

- `kernel/linux-7.2.11/drivers/gpu/drm/msm/dsi/dsi_cfg.h:39`: `DSI_6G_REG_SHIFT = 4`.
- `dsi_cfg.c`: 6G configuration entries use `.io_offset = DSI_6G_REG_SHIFT`.
- `dsi_host.c:2049`: `msm_host->ctrl_base += cfg->io_offset`.
- `dsi_host.c:198`: `dsi_write` uses `ctrl_base + reg`.
- `registers/display/dsi.xml:369`: logical `VIDEO_COMPRESSION_MODE_CTRL = 0x29c`, with WC `[31:16]`, datatype `[13:8]`, packet encoding `[7:6]`, EOL `[5:4]`, enable bit 0.

Therefore vendor raw-base `+0x2a0` and mainline logical `+0x29c` target the **same physical DSI register**. The repository DTS `dts/sm4450-xiaomi-sky.dts:1310` gives the controller resource base `0x0ae94000`, yielding physical `0x0ae942a0`. This physical address follows that DTS mapping; it is not a live device register readback.

The old report's **`sde_hw_intf_setup_te_config`** evidence is unrelated to DSI compression. At **`0x10babc/0x10bac0`**, the register-name string `.rodata+0x3566b` is **`INTF_TEAR_SYNC_THRESH`**. **`0x10bac4`** supplies offset `0x29c` to `sde_reg_write`, using the SDE INTF register-map object at hardware-interface-object `+0x30` (`0x10ba04`). Its packed low/high thresholds must not be interpreted as DSI packet fields. The numeric offset coincidence does not identify a shared register or address space.

## Scope and corrections to previous analysis

This closes the original vendor report's explicit caveat that the DSI packet/register producer had not been located. Its numerical `0x04380b01` conclusion is now independently supported by the actual vendor producer, while its identification of the SDE TE writer as DSI compression is disproved. The `dsc+3 = pkt_per_line` label in its parser table and subsequent HANDOFF claims must be read as **slice count**, not actual packets per line.

The current mainline `dsi_update_dsc_timing` at `dsi_host.c:940` assumes one slice per packet: `bytes_per_pkt = slice_chunk_size` and `pkt_per_line = slice_count`. With two slices that produces `WC=540`, packets-per-line=2, and register `0x021c0b41`. That differs from the verified vendor setting above. This report documents that discrepancy without changing code or running a device experiment.

Verification method: bounded `nm`, `readelf`, `aarch64-linux-gnu-objdump -dr --start-address/--stop-address` on immediate parser/private/helper/callback functions, direct ELF string reads at relocation targets, and inspection of the immediate mainline register/base definitions. No hardware behavior or successful panel bring-up is claimed from static disassembly alone.

## Parent-session runtime corroboration

After this read-only analysis, the parent session separately built/deployed a packetization experiment. The existing artifact and captured log were inspected here read-only; this analysis did not perform that deployment.

- Artifact: `build/panel-vendor-packet/panel-xiaomi-m19.ko`, SHA-256 **`fd883fa9342f22ef712877851f2aa316dae389cddf512d1dee1f768aacf5a717`** (locally verified).
- `build/panel-vendor-packet/runtime.log:4`: vendor m19 PHY timings active, bit clock `671863152`.
- Log lines 5–7: `flags=0x2a03`; `slice_per_intf=2 chunk=540 total_bytes=1080 bytes_per_pkt=1080 eol=0 pkt_per_line=1 slice_per_pkt=2`; register readback **`VID_COMP=04380b01`**.
- Log lines 8 and 10: PPS sent with `err=0`; DSC `initial_lines=3`, slice `540x20`, bpp fixed-point `128`, bpc 8, picture `1080x2460`.
- Log lines 1–3 record **10,627,200 bytes** written for the framebuffer test pattern.

This corroborates programming of the predicted packet fields on that experimental mainline boot. Visual panel output remains pending; the log alone does not establish a working image.

User reports no visible change after verified vendor packet grouping. This is a negative visual result for the current vendor-PHY/initial_lines3/long-write/90Hz baseline.

Following no-change result, diagnostic panel module adds reads of page10 C0/C1/C2 after PPS, without altering existing DSC writes. Build artifact build/panel-ddic-read/panel-xiaomi-m19.ko SHA2561671fb2fe7e9a29870146c7b123e5c394501988779a3e204b34c43b8bf4e28bd. Readback may be unsupported; no conclusion until results.
