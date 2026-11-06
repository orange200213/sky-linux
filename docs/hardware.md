# Hardware notes — Xiaomi Redmi 12 5G (`sky`), SM4450

> **Scope: one variant.** All values below were measured on the **Japanese-market
> Redmi 12 5G** (`sky`, SM4450), stock **V14.0.7.0.TMWINXM** (Android 13). The
> Redmi 12 5G was sold in several regions and variants of the same name can differ
> in panel, touch controller, PMIC rails, modem/WLAN firmware and partition
> layout — verify before trusting a value on another unit.

Facts collected from `fastboot getvar all`, the vendor DTB/DTBO, the runtime DT
and from running Linux on the device.

## SoC / boot

| Item | Value |
|------|-------|
| SoC | Qualcomm SM4450, "ravelin" (Snapdragon 4 Gen 2), `qcom,sm4450` |
| ABL product string | `river` |
| Firmware | UEFI (EDK2) in `uefi_a/b`, secure app in `uefisecapp_a/b` |
| Boot chain | PBL → XBL → ABL → EDK2 → Android boot image |
| Slots | single active slot (`a`) during bring-up |
| ABL loads `boot.img` at | `0xA8000000` |
| ABL DTB | `0xB7403000` (runtime), `0xB553B000` seen earlier |
| Temperature/fuse info | `qcom,msm-id` not needed by the shim |

## Memory

| Item | Value |
|------|-------|
| RAM declared in the DTS | five segments, matching a **4 GB** unit (see below) |
| Usable RAM measured | `free -m` → **3259 MB** total (`/proc/meminfo` agrees) |
| Low RAM bank | `0x80000000` + `0x68e00000` (1.6 GiB) |
| High segments | `0xF1300000 + 0x0ED00000`, `0x8_00000000 + 0x3FE00000`, `0x8_40000000 + 0x3C700000`, `0x8_7CD00000 + 0x01E00000` |
| Kernel load address (shim) | `0xC0000000` (inside the declared bank) |
| DTB load address | right after the kernel `Image` + its `.bss`, 2 MiB aligned |
| `ramoops` | `0x97700000`, 2 MiB |
| `mpss` region | `0x8a000000` + 0xd700000 |
| ABL splash / framebuffer | `0xb8000000`, 43 MiB reserved region |

## Display

| Item | Value |
|------|-------|
| Panel | 1080×2460 IPS LCD, Xiaomi `m19` (DSI) |
| Framebuffer left by ABL | base `0xb8000000`, stride `0x10E0` (4320), `x8r8g8b8` |
| DPU | SM4450 display subsystem `@0xae00000`, DPU 8.6, IRQ 83 |
| DSI | controller v2.6, PHY v4.3 |
| Backlight | KTD3136 on I²C (no mainline driver yet) |
| fbcon size at 8×16 | 135×153 characters |

## Interrupts / timers

| Item | Value |
|------|-------|
| GIC | distributor `0x17200000`, redistributors `0x17260000` |
| Arch timer | `19.2 MHz` (`cntfrq=0x124F800`), MMIO timer `@0x17420000` |
| PSCI | `arm,psci-1.0`, `smc` |

## Storage (UFS)

| Item | Value |
|------|-------|
| Controller | `ufshc@0x1d84000`, 2 lanes, IRQ 265 |
| PHY | `phy@0x1d87000`, QMP UFS, SM4450 tables |
| Logical block size | 4096 |
| LUN0 | `/dev/sda`, 117 GiB, holds GPT with 20 partitions |
| `userdata` | `sda20`, ~108 GiB, **ext4** |

## USB

| Item | Value |
|------|-------|
| Controller | DWC3 `@0xa600000`, IRQ 133 (plus PDC IRQs 14/15/17) |
| HS PHY | SNPS femto `@0x88e3000` (`qcom,sm4450-usb-hs-phy`) |
| SS/DP PHY | QMP combo `@0x88e8000` — `disabled` in the vendor DT as well |
| Roles | peripheral only for now (`dr_mode = "peripheral"`, no role switch) |
| Gadget | CDC ACM `/dev/ttyGS0` + CDC NCM `usb0` (192.168.7.1/24) |
| IOMMU / DMA | `iommus = <&apps_smmu 0x540 0x0>;` + `dma-coherent`. Without it, enabling the high memory segments makes the gadget fail with `dwc3-qcom a600000.usb: unknown endpoint event 0` |

## PMIC / power

| Item | Value |
|------|-------|
| Primary PMIC | PM6450 (SPMI), 102 regulators in the vendor DT |
| RTC/ADC | PMK8350 |
| Charger | PM7250B |
| Fuel gauge | `qcom_battmgr` (PMIC glink) |
| Vendor → mainline regulator naming | `pm6450_l4` = `ldob5` = `vreg_l5b_0p88`, etc. |

## Wireless

| Item | Value |
|------|-------|
| WCN6750 on WPSS | ath11k (WLAN) + hci_qca (BT), `qcom,sm4450` remoteproc |
| Status | nodes present in the DTS, `status = "disabled"` — not started yet |

## Known-protected hardware

* A few TLMM GPIO register pages are XPU/TZ-protected on this SoC. Reading
  their direction register raises a synchronous external abort; see
  `docs/m1-mainline-kernel-boot.md` §3.
* Fingerprint (Goodix/FPC) sits on SPI with TZ involvement; not touched.
* GPU SMMU (`3da00000.iommu`) times out after 10 s on probe; the GPU node is
  disabled until the display/GPU milestone.
