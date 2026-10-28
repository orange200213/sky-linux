# sm4450-xiaomi-sky.dts

Board device tree for the Xiaomi Redmi 12 5G, derived from ROCKNIX's
`sm4450-anbernic-rg55g1.dts` (same SoC) with sky-specific values taken from the
device's own runtime DT.

It is meant to be dropped into `arch/arm64/boot/dts/qcom/` of a kernel tree that
already has the ROCKNIX SM4450 series applied (it `#include`s mainline
`sm4450.dtsi`, `pmk8350.dtsi`, `pm7250b.dtsi`).

## What was changed compared to the Anbernic board file

| Change | Reason |
|--------|--------|
| `/memory@80000000` added (1.6 GiB) | arm64 board files do not describe RAM; without it the kernel has none |
| `/chosen/framebuffer@b8000000` added, `reg = <0x2b00000>` | `simpledrm` + `fbcon` on ABL's splash buffer; `reg` must cover the whole splash region |
| `bootargs` set | `console=tty0 fbcon=nodefer nokaslr panic=15 clk_ignore_unused pd_ignore_unused` |
| USB node reduced | `dr_mode = "peripheral"`, `phys = <&usb_1_hsphy>`, `qcom,select-utmi-as-pipe-clk`, no role switch |
| panel rails without GPIOs | the PM6450 GPIO phandles do not resolve yet (no panel driver) |
| `gpu@3d00000` disabled | its SMMU probe times out for 10 s |
| Anbernic-only peripherals removed | fan, hall sensor, joypad, RGB LEDs |

## Regenerating and building

```bash
python3 tools/mksky_dts.py                      # regenerate the .dts
KDIR=/path/to/linux-7.3 tools/dtsbuild.sh dts/sm4450-xiaomi-sky.dts build
```

`KDIR` must point at a kernel tree with the ROCKNIX SM4450 series applied — the
board file includes `sm4450.dtsi` and the qcom `dt-bindings` headers.

The generator is the source of truth: it applies each of the changes above
programmatically so they are easy to review and to re-apply against a newer
ROCKNIX board file.
