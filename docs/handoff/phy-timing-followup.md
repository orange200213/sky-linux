# Vendor PHY timing experiment

User reports initial_lines=3 makes no visible difference. Current baseline retains3 (vendor-derived), long-write initialization, NO_EOT_PACKET, 90 Hz /276180 kHz, vendor-correct PPS and2 packets of540 bytes.

Read-only static ARM64 utility `build/phy-vendor-timing/read-phy` maps `/dev/mem` at0xae94000 and reads PHY0 timing registers at offset0x400+0xb4+i*4. Actual14 bytes:

`00 18 06 07 21 20 06 06 04 02 04 00 16 16`

Vendor DT:

`00 18 06 06 15 20 06 06 07 02 04 00 15 0b`

Differences: CLK_TRAIL7->6, HS_EXIT33->21, HS_RQST4->7, CLK_PRE22->21, CLK_POST22->11 (decimal). Matching bytes otherwise.

Vendor mapping verified in `dsi_phy_hw_timing_val_v4_0`0x13a968: accepts14 parameters and copies each to contiguous descriptor bytes52..65. `dsi_phy_hw_v4_0_commit_phy_timing`0x1380dc writes them in order to PHY offsets0xb4..0xe8. This matches mainline dsi_phy_7nm.c DPHY register programming.

Diagnostic override in dsi_phy_7nm.c runs after timing calculation, before PHY and host timing programming. It applies only to xiaomi,sky DPHY mode at bitclk670..674MHz, setting all variable fields to vendor values. Fixed fields0/9/10/11 already match. Changes shared clk_pre/post as well, so DSI host sees the same requested shared timings. This is a local diagnostic override, not an upstream-ready board-specific PHY solution.

Build source backup: `build/phy-vendor-timing/dsi_phy_7nm.c.before`. Read utility source: `build/phy-vendor-timing/read-phy.c`.

First diagnostic boot did NOT activate override: actual root compatible is `anbernic,rg55g1`, `qcom,sm4450` despite source dts carrying xiaomi,sky. Register readback stayed unchanged, no sky-phy log. Corrected scope to the actual PHY node compatible `qcom,sm4450-dsi-phy-5nm` (verified via /sys/firmware/devicetree/base/soc@0/display-subsystem@ae00000/phy@ae94400/compatible), retaining rate guard. This first boot is not a PHY timing A/B result.

Corrected image deployed only to boot_a, SHA256 `6f59bd3428d66cfcd7c2e443babc044bcce81f48c376b8d1bbe4d7733a64af2f`. Runtime sky-phy marker bitclk671863152; all14 timing registers independently read back byte-identical to vendor. PPSerr0, IDc01100/power9c, initial_lines3 and90Hz preserved. Complete10627200-byte test pattern written to fb0. Awaiting visual result.

User reports completely unchanged after verified vendor14-byte PHY timing. This is a negative visual result; keep as documented diagnostic baseline for next packet grouping comparison, not a confirmed fix.
