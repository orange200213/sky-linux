# M7 — touch (Novatek on SPI) — in progress

Hardware, from the vendor DT (`spi@a84000`, QUPv3 wrapper 1, SE1):

| Item | Vendor value |
|---|---|
| controller | `qcom,spi-geni` at `0xa84000`, IRQ `<0 0x162 4>` (SPI 354, level high) |
| bus clock | `spi-max-frequency = <0x2faf080>` = 50 MHz |
| touch chip | `novatek,NVT-ts-spi` (`novatek@0`), `spi-max-frequency = <0x927c00>` = 9.6 MHz |
| reset | `novatek,reset-gpio = <&tlmm 12 ...>` |
| irq | `novatek,irq-gpio = <&tlmm 13 ...>`, falling edge |
| vendor extras | `novatek,swrst-n8-addr = <0x3f0fe>`, `novatek,spi-rd-fast-addr = <0x3f310>`, MP criteria tables |
| pins | `pinctrl-0` = 4 phandles (default), `pinctrl-1` = 2 phandles (sleep) |

## Attempt 1 (reverted): panic

Enabling the SE node standalone (`status = "okay"` with
`compatible = "novatek,nt36672a-ts"`) panics at boot:

```
sm4450-tlmm f100000.pinctrl: invalid group "gpio8" for function "qup1_se1"   (gpio8..gpio11)
Unable to handle kernel NULL pointer dereference at 0x30
pc : geni_se_clks_on+0x24/0x140
     geni_se_resources_activate
     spi_geni_runtime_resume
     spi_geni_probe
```

Two independent mistakes:

1. the SE node is not a child of a `qcom,geni-se-qup` wrapper, so `core` clock and
   the interconnect path are missing and `geni_se_clks_on()` dereferences NULL;
2. the pin groups are wrong for this SoC — the vendor's `qup1_se1` is not
   gpio8..11 on SM4450, and the node also needs the correct GIC line (354, not 602)
   and the 50 MHz bus clock.

The node is back to `status = "disabled"; /* needs a qupv3 wrapper node */`.

## Next

1. Add the QUPv3 wrapper 1 node (`qcom,geni-se-qup`, `m-ahb`/`s-ahb` clocks) and
   move `spi@a84000` inside it with `interrupts = <GIC_SPI 354 ...>`,
   `clocks = <&gcc GCC_QUPV3_WRAP1_S1_CLK>`, `clock-names = "se"`.
2. Add the SM4450 touch pinctrl groups (bus + irq/reset) from the vendor DT.
3. Bind the mainline `novatek-nvt-ts` driver and read the 12-byte parameter block it
   prints; it validates `chip_id`, so a mismatch tells us the real chip
   (NT36672C/E are likely) and we add a chip variant, or write a small driver of
   our own using the vendor `swrst-n8-addr` / `spi-rd-fast-addr` hints.
4. Add `vcc`/`iovcc` supplies (the driver does `regulator_bulk_get` on both).
