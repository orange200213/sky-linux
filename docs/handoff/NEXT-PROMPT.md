# Next-session prompt (copy this as the opening message)

I am bringing up mainline Linux on the Xiaomi Redmi 12 5G (`sky`, Qualcomm SM4450)
under `/home/orange/redmi125g/`. Device access: `sshpass -p sky ssh root@192.168.7.1`
over USB-NCM when my kernel is booted; `fastboot` when it is in the bootloader.
Host sudo password: `m912n67oo`.

## Where we are

**Display is working.** The panel (Xiaomi "m19", 1080x2460 IPS LCD, DSC 1.1) shows a
correct image and the framebuffer console prints normally. The root cause was the DPU
DSC rate-buffer size on SM4450: `_dsc_calc_output_buf_max_addr()` reported
`2400 / num_softslice - 1` while the block only holds 600 bytes, so the DSC encoder
produced a malformed stream. The fix is
`kernel/patches/0003-dpu-dsc-sm4450-output-buffer-size.patch`, and the complete working
recipe (vendor PPS with `initial_xmit_delay = 170`, vendor 14-byte PHY timings, one
1080-byte packet per line, generic-long-write init sequence, 600-byte buffer) is in
`docs/panel-handoff/HANDOFF.md` section 0.

Read first:

1. `docs/panel-handoff/HANDOFF.md` — section 0 for the solved display state, the rest
   as panel reference (identity, wiring, vendor DSC truth, register dumps, history).
2. `docs/panel-handoff/NEXT-PROMPT.md` (this file) for the working rules.
3. Only if a display regression appears: `docs/panel-handoff/vendor-dsc-analysis.md`
   and `.../mainline-dsc-analysis.md`, plus the `*-followup.md` notes from the
   debugging sessions.

## Working state (do not re-derive)

* Kernel 7.3 (ROCKNIX SM4450 patches + bring-up patches), boots from `boot_a` via the
  custom shim; only `boot_a` is ever written. Stock recovery image:
  `~/redmi125g/sky_in_global_images_V14.0.7.0.TMWINXM_13.0/images/boot.img`.
* Rootfs: Alpine on UFS `/dev/sda20`, mounted by the initramfs and entered with
  `switch_root /mnt/skyroot`. `/init` in the rootfs auto-loads
  `ktd3136.ko` and `panel-xiaomi-m19.ko` from `/lib/modules/$(uname -r)/extra/`.
* USB gadget: ACM console + NCM network (host profile `sky-usb`, static 192.168.7.20/24,
  device 192.168.7.1), sshd works.
* Backlight: KTD3136 module, `/sys/class/backlight/ktd3136-backlight/`, range 0..2047.
* Display: DPU 8.6 + DSI 2.6 + panel driver; `card1-DSI-1` connected at 1080x2460@90,
  `msmdrmfb` with fbcon printing normally.
* Recorded working artefacts:
  `build/twrp-dsc-buffer/improved-state/` (boot.img, Image, DTB, panel module, dmesg,
  patch, SHA256SUMS) and a trimmed text copy in
  `docs/panel-handoff/fixed-state-reference/`.

## Iteration loops

The panel driver is a module on the UFS rootfs, so most display work needs no flashing:

```bash
cd ~/redmi125g/sky-mainline/kernel/min-tree
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make -j10 modules
scp drivers/gpu/drm/panel/panel-xiaomi-m19.ko \
    root@192.168.7.1:/lib/modules/$(uname -r)/extra/
ssh root@192.168.7.1 "sync; echo b > /proc/sysrq-trigger"
```

Kernel/DTB changes:

```bash
make -j10 Image
cd ../../shim && python3 tools/mklinuxpayload.py \
    ../kernel/min-tree/arch/arm64/boot/Image ../build/sm4450-xiaomi-sky.dtb \
    out/payload.bin --load 0xc0000000
./build-sky.sh out/payload.bin
fastboot flash boot_a out/sky-shim-boot-v4.img && fastboot reboot
```

Test pattern: `python3 docs/panel-handoff/make-test-pattern.py`, then
`scp pattern.rgb root@192.168.7.1:/tmp/` and
`ssh root@192.168.7.1 "dd if=/tmp/pattern.rgb of=/dev/fb0 bs=4096"`.

## Suggested next milestones (pick with me)

1. **Upstreamable display patches.** Turn the working ingredients into a clean series:
   DSC rate-buffer size from the DPU catalog instead of an
   `of_machine_is_compatible()` check, vendor PHY timings through DT, `slice_per_pkt`
   handling, generic-long-write init, and the `initial_xmit_delay` quirk. Then submit
   the panel driver with a proper DT binding.
2. **Backlight polish.** Brightness curve, single backlight device (currently both
   `ae94000.dsi.0` DCS backlight and `ktd3136-backlight` exist), blank/unblank.
3. **Low-power states.** Suspend/resume with panel sleep in/out, DSI clock gating,
   verify the DSC configuration survives resume.
4. **Compositor / usable userland.** Minimal Wayland or framebuffer tooling, page
   flips, vblank, and a 60 Hz mode if the panel supports one.
5. **Then peripherals:** touch (Novatek SPI), Wi-Fi/BT (WCN6750), audio, camera.

## Rules that keep this efficient

* Verify with the device, not by reasoning: read `dmesg | grep sky-`, `/proc/fb` and
  `/sys/class/drm/*/status` after every change.
* Before any display change, snapshot the working `panel-xiaomi-m19.ko` and `boot.img`
  (the `improved-state` folder is the reference that works).
* Keep code comments minimal and factual; the goal is upstreamable patches.
* Write every conclusion (and every failed experiment) into `docs/panel-handoff/` so a
  later session does not repeat it.
