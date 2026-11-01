# sky-linux — mainline Linux on the Xiaomi Redmi 12 5G (SM4450 "sky")

> **Tested on one variant only.** Everything in this repository was developed and
> verified on the **Japanese-market Xiaomi Redmi 12 5G** (codename `sky`, SoC
> SM4450 / "ravelin"), stock firmware **V14.0.7.0.TMWINXM** (Android 13). Other
> regional variants of the same model share the name but can differ in panel,
> touch controller, PMIC rails, modem/WLAN firmware and partition layout, so treat
> every value here as "measured on that unit" until someone confirms it on
> another variant.
>
> **AI assistance.** This project was built with the help of
> **DeepSeek V4.1 (1017)** and **GPT-5.6 sol** — used for code, DTS work,
> reverse-engineering and the documentation in this repository.

Bring-up notes, patches and tools for running a **mainline Linux kernel** on the
Xiaomi Redmi 12 5G (codename `sky`), a Qualcomm **SM4450 / "ravelin"**
(Snapdragon 4 Gen 2) phone.

The device ships Android 13 on a downstream 5.10 kernel. This project replaces
the Android boot image with a small freestanding shim that loads a stock
mainline `Image` plus a hand-written device tree, and then grows a usable
Linux system on top of it.

Everything here was developed and verified on real hardware.

---

## Status

| Milestone | Result | Notes |
|-----------|--------|-------|
| **M0** boot shim | ✅ works | `boot_a` holds our own boot image; UEFI/ABL no longer decides which DTB to use |
| **M1** mainline kernel boots | ✅ works | Linux 7.3 + ROCKNIX SM4450 series, own `sm4450-xiaomi-sky.dts`, fbcon console |
| **M2** USB gadget (CDC ACM) | ✅ works | dwc3 + SNPS femto HS PHY, `qcom,select-utmi-as-pipe-clk` was the missing bit |
| **M3** USB networking + SSH | ✅ works | NCM + built-in DHCP, OpenSSH 9.x (needs `sshd-auth`/`sshd-session`) |
| **M4** UFS storage | ✅ works | `userdata` (ext4, 108 GiB) mounted read/write, persistent logs |
| **M5** display (DPU/DSI/panel) | ✅ works | DPU 8.6 + DSI 2.6 + `m19` panel at 1080x2460@90 with DSC, KTD3136 backlight, fbcon on `msmdrmfb` — [details](docs/m5-display.md) |
| **M6** power / charging | ✅ works | ADSP firmware + QRTR + pd-mapper + pmic-glink, `battery`/`qcom-battmgr-*` power supplies — [details](docs/m6-power.md) |
| **M7** touch (Novatek SPI) | 🚧 **partial** | NT36672E on QUPv3 wrapper 1 SE1 via GPI DMA, mainline regmap driver, runtime recovery. Verified **only on the `m19` panel** and **only from raw `/dev/input/event3` (`dd`) output** — never yet through a desktop/window manager, and the controller can still drop out (recovery exists, root cause of the IC death is not fully explained) — [details](docs/m7-touch.md) |
| **M8** Wi-Fi / BT (WCN6750) | 🚧 in progress | WPSS boot, WLFW QMI, MSA handshake, regdb/BDF and the CE rings all work; blocked on **where the WCSS UMAC/WBM/REO/TCL register block lives on this SNOC variant** (every documented offset aborts) — [details](docs/m8-wifi.md), [working handoff](docs/wifi-handoff/HANDOFF-snoc-mainline.md) |

Current boot chain:

```
PBL → XBL → ABL → EDK2 (uefi_a) → our boot.img in boot_a
                                        │
                                        └─ shim (fake "kernel") 
                                             ├─ copies Image to 0xC0000000
                                             ├─ copies DTB   to 0xC2A00000 (or wherever)
                                             └─ jumps at EL1, MMU off, x0 = DTB
```

---

## Repository layout

```
docs/          milestone write-ups, hardware notes, how debugging was done
shim/          the freestanding ARM64 shim that is flashed as boot_a
kernel/        our kernel patches, debug-only patches, and the build config
 dts/          sm4450-xiaomi-sky.dts + the generator that produced it
docs/         milestone write-ups; docs/handoff/ has the full display bring-up record
initramfs/     the /init script and the initramfs build helper
kernel/       config, patch series, and the out-of-tree panel + backlight drivers
tools/         payload/boot-image packers and the DTS build helper
```

---

## Quick start (what you need)

1. **Kernel**: stock Linux 7.3 (`linux-7.3.tar.xz` from kernel.org), the
   ROCKNIX SM4450 patch series applied, plus the patches in
   `kernel/patches/`. See `kernel/README.md`.
2. **Device tree**: drop `dts/sm4450-xiaomi-sky.dts` into
   `arch/arm64/boot/dts/qcom/`, add it to the Makefile, build `dtbs`.
3. **Payload**: `tools/mklinuxpayload.py` appends the DTB behind the `Image`
   and writes a small header the shim understands.
4. **Boot image**: `shim/build-sky.sh` wraps the payload into an Android v4
   boot image with the shim as the "kernel".
5. Flash **`boot_a` only**:

```bash
fastboot flash boot_a out/sky-shim-boot-v4.img
fastboot reboot
```

Recovery is always:

```bash
fastboot flash boot_a /path/to/stock/boot.img
```

---

## Hardware summary

| Item | Value |
|------|-------|
| SoC | Qualcomm SM4450 (ravelin), 4×A55 + 4×A78 |
| RAM | 4/6/8 GB LPDDR4X, low bank at `0x80000000` |
| Display | 1080×2460 LCD, `m19` panel, DSI, KTD3136 backlight |
| Framebuffer left by ABL | `0xb8000000`, stride 4320, 43 MiB "splash region" |
| Storage | UFS 4.0, 2 lanes, `/dev/sda` = 117 GiB LUN with `userdata` (108 GiB) |
| USB | dwc3 + SNPS femto HS PHY (no USB3 here) |
| PMIC | PM6450 (+ PMK8350 RTC/ADC, PM7250B charger) |
| Variant tested | Japanese-market Redmi 12 5G (`sky`), stock **V14.0.7.0.TMWINXM**; panel/touch/firmware values are from that unit only |
| Firmware | UEFI (EDK2) on `uefi_a`, application processors start via PSCI |

Details: `docs/hardware.md`.

---

## Debugging channels used

No UART is reachable, so the project bootstrapped its own channels — all of
them are documented in `docs/debugging.md`:

1. `fbcon` on the ABL framebuffer (`simple-framebuffer` + `simpledrm`).
2. Coloured progress markers painted into the framebuffer from `start_kernel()`
   (debug patch), plus a "last stage" colour on panic.
3. `pstore`/`ramoops` at `0x97700000`; the record is **raw deflate**, readable
   from TWRP after a deliberate panic.
4. USB CDC-ACM serial console (`/dev/ttyACM0` ↔ `/dev/ttyGS0`).
5. USB NCM network + SSH.
6. Persistent `dmesg` on UFS (`/mnt/log/dmesg-last.txt`).

---

## Releases

Released boot images and what to do with them: `docs/install-and-usage.md`.

## License

- `shim/`, `tools/`, `initramfs/`, `docs/`: GPL-2.0-or-later (see `LICENSE`).
- `kernel/patches*/`: GPL-2.0, matching the kernel.
- `dts/`: BSD-3-Clause (device tree convention).

Nothing here is upstream-ready yet; the patches are bring-up quality and
deliberately minimal.

### WiFi (M8) handoff

* `docs/m8-wifi.md` - full experiment log (what the firmware wants, phase 1/2/3)
* `docs/wifi-handoff/HANDOFF-snoc-mainline.md` - **working handoff for the
  mainline ath11k SNOC port**: what works, the one blocker, next steps, the
  runtime knobs, and every trap that cost time
* `docs/wifi-handoff/NEXT-PROMPT.md` - ready-to-paste brief for a fresh session
