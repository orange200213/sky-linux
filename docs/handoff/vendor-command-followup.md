# Vendor command comparison and long-write experiment

Vendor binary: `/tmp/msm_drm.ko`, downloaded from sm4450-development/device_xiaomi_sky-kernel branch 16.

## Command sets

`cmd_set_prop_map` is `.data+0x9330`, 30 entries, each an 8-byte relocated string pointer. Entries 0/1/4 are pre-on/on/post-panel-on; 2/3 are vid-on/cmd-on; 23 is dynamically generated PPS and 24 is dynamically generated ROI. There is no dedicated DSC-enable command set in this map.

`dsi_panel_parse_cmd_sets_sub` at 0x166184 loads the property name from the map at 0x1661ec–0x166204 and calls `dsi_panel_create_cmd_packets` at 0x1662d0. `dsi_panel_create_cmd_packets` at 0x165b48–0x165b58 copies byte zero of the vendor command header directly into the message type; it does not choose a type based on payload length.

`dsi_panel_tx_cmd_set` at 0x16d27c loads the descriptor array (24-byte command-set stride, 64-byte command descriptor stride), adds LP mode when required, and calls `dsi_host_transfer_sub` at 0x16d330. Post-command wait comes from descriptor+52 at 0x16d338–0x16d348.

`dsi_panel_enable` sends set 1 at 0x16e0f0–0x16e0f8, then optionally set 2 or 3 depending on panel mode. The m19 vendor node has no vid-on/cmd-on/pre-on/post-on payload. `dsi_panel_update_pps` sends set 23 at 0x16d1f0–0x16d1f8. This inspection found no extra compression-enable command in these paths; it does not establish the absence of every possible Xiaomi hook elsewhere.

## Concrete mismatch

All 163 current initialization payloads match `panel-m19-init.json` exactly and in order. Vendor types are 161 DCS long writes (0x39) and two DCS short writes (0x05), with 120 ms / 20 ms delays after sleep-out / display-on.

Of the 161 long writes, 157 have only two payload bytes. Mainline `mipi_dsi_dcs_write_buffer()` selects DCS short write with parameter (0x15) for length 2. Thus the previous driver sent 157 commands with a different packet type, including page/unlock writes and page-0x10 C0=03. Successful DCS ID/power reads do not establish that every vendor register write took effect.

The experiment replaces the 161 initialization writes with an explicit DCS long-write helper, preserving the two short writes, payloads, delays, PPS ordering and runtime clock. The helper preserves LP mode and accumulated error handling. No extra DSC-enable command is invented.

This is an evidence-based A/B experiment, not yet a confirmed noise fix. Existing PPS-after-display-on and redundant final display-on remain unchanged to isolate packet type.

## Build

`make -C kernel/min-tree -j10 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules` succeeded. A pre-existing unused `ctx` warning in `m19_remove` remains.

Artifact: `build/panel-long-write/panel-xiaomi-m19.ko`.
Vermagic: `7.3.0-g4e6c68989319-dirty SMP preempt mod_unload aarch64`.
SHA256: `61ba59c0c5d0b0ae2a65e66c18d2ff7fe463a87deb9b4989027bfb02ec81a175`.
Source before change: `build/panel-long-write/panel-xiaomi-m19.c.before`.

Runtime prior to experiment: clock_khz=290300, mode rounds to 95 Hz, PPS ixd=170, VID_COMP=0x021c0b41, INTF_CONFIG2=0x1011. Keep this clock for the first A/B. Vendor line timing analysis independently finds 276180 kHz corresponds to the intended 90 Hz, rather than 290300 kHz.

## Deployment and runtime validation

Installed on the UFS rootfs and rebooted successfully. Remote previous module backup: `/lib/modules/7.3.0-g4e6c68989319-dirty/extra/panel-xiaomi-m19.ko.before-vendor-long-write`. Remote module SHA256 matches the artifact above. Boot ID: `c0633233-ca9c-4df8-a781-85310373360a`.

New log: `sky-panel: vendor-long-write init; PPS sent, err=0`; DDIC ID remains `c0 11 00`, power mode `9c`. VID_COMP `021c0b41`, INTF_CONFIG2 `1011`, PPS ixd170/final7072/scale_inc113 remain unchanged. Runtime clock remains 290300 kHz. No matching underrun or long-write errors appeared in the captured log.

Wrote all 10627200 bytes of the generated BGRA quadrant pattern into `/dev/fb0` after reboot. Runtime evidence: `build/panel-long-write/runtime.log`. Source patch: `build/panel-long-write/vendor-long-write.patch`.

Visual result is awaiting user observation. Log success does not prove correct DSC decoding. Ask whether the center boundary x=540 is visible/correct, whether horizontal bands remain uniform across both slices, and whether the horizontal color division at y=1230 is recognizable.

## User feedback and second experiment

User reports alternating flicker after the vendor-long-write experiment and supplies two images. Attachment analysis tool failed with `baseURL must be an absolute http(s) URL`; no spatial artifact conclusions can be drawn from these images in this session. Long writes did not establish a clean image.

Next isolated experiment: preserve the same module and change `/etc/sky-panel-clock` from 290300 (about 94.6 Hz) to 276180 (90 Hz). Previous setting backed up as `/etc/sky-panel-clock.before-90hz-long-write`. The vendor line-timing reverse engineering supports 276180 as the correct 90 Hz clock. Earlier 276180 experiments predate this explicit long-write sequence, so the combination remains worth testing.

During the 90 Hz experiment reboot, device entered fastboot. Read-only queries: current-slot=a, slot-successful:a=no, slot-unbootable:a=yes, slot-retry-count:a=0. Ordinary `fastboot reboot` returned to fastboot. Used `fastboot set_active a` to reset retries, then `fastboot reboot`; no partitions flashed. Repeated bring-up reboots can exhaust an unconfirmed A/B slot, independent of any claim about panel behavior.

## Visual result at 90 Hz and EOT experiment

User reports unchanged cyclic flicker at 90 Hz: a sharp left/right division; left upper quarter cycles green stripe / snow / red stripe / snow; second quarter flashes a version of the first; lower half flashes blue/white stripes. Right side is mostly snow bands, with red/green stripes visible in the upper fifth. This is user observation, not image-tool analysis. Color dependence on the framebuffer quadrants persists, and the central division is compatible with the slice boundary, but neither identifies the exact failing stage.

Next single-variable experiment removes `MIPI_DSI_MODE_NO_EOT_PACKET` from the panel flags. Mainline dsi_host.c sets `DSI_EOT_PACKET_CTRL_TX_EOT_APPEND` when this flag is absent. Preserve explicit long writes, 276180 kHz, PPS and packet/slice geometry. This is a diagnostic A/B, not a confirmed fix; EOT is an HS-transmission terminator, not a replacement for DSI packet word count/CRC framing.

EOT experiment deployed successfully, module SHA256 `0b9b4b3a370b72b26245a67c75640bb20f78fd7fb7138feac0b8d4ec199baecd`, artifact `build/panel-eot/panel-xiaomi-m19.ko`. Reboot log confirms flags=0x803 (previous 0xa03), mode 90 Hz / 276180 kHz, PPS err=0, ID c0/11/00 and power9c. Full test pattern written to fb0 after reboot. `busybox devmem` is unavailable, so EOT register readback was not obtained; flags plus inspected host implementation establish the requested configuration, not independent MMIO confirmation. Runtime log: `build/panel-eot/runtime.log`. Awaiting visual comparison.

User reports EOT-enabled experiment is completely unchanged. Reverted the panel source to NO_EOT_PACKET and rebuilt baseline modules.

Next evidence-based experiment: vendor `sde_encoder_dce_setup` 0xa62dc–0xa63a4 calculates initial_lines=3 for this exact geometry; mainline calculates2. Vendor numerator1914 = slice_width540 + 279*2 + xmit170 + mux78 + rate540 +28, then integer divide540. Field dsc+140 consumed by vendor DSC1.2 config0x1205c0. Added a tightly geometry/DSC-param-scoped diagnostic override3 in dpu_encoder.c; this is not an upstream-ready generic fix. Kernel Image rebuild required. Keep panel long writes, NO_EOT_PACKET, 90 Hz and vendor-correct PPS. Prior initial_lines20 experiment used a different PPS and does not settle3.
