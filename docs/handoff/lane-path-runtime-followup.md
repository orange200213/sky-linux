# Lane path runtime audit

User asks to focus on the most likely remaining issue instead of further DSC parameter sweeps. Selected lane data routing: command reads alone do not establish all four data lane paths.

Actual running DT endpoint data-lanes is0123. Panel requests4 lanes. CTRL physical0xae94004=0x1f3 enables all four data lanes and clock; STATUS0 raw+8=8 shows video engine busy. FIFO raw+c=0x1010 has no defined video overflow bit0 or underflow bit3. Lane status raw+a8=0x1f00 indicates all ULPS-active-not flags asserted. Lane map raw+b0 initially0, legacy enum identity.

Vendor dsi_ctrl_hw_22_setup_lane_map1491ac packs descriptor bytes+4..7 as nibbles; vendor default descriptor in dsi_display_init15accc encodes1,2,4,8, giving8421. **Its applicability and register behavior on this actual DSI2.6 revision is not established by that callback alone.** Actual version raw0=20060000, raw1f4=03000104.

Diagnostic live write8421 to raw+b0 read back ONLY1. Immediately restored0; readback0 confirmed and full10627200-byte pattern rewritten. No kernel/source lane-map change made. Treat8421 as unsuccessful compatibility test, not a working mapping or visual A/B. No visual judgment requested for transient mapping1. User-facing claim of a confirmed lane encoding mismatch was premature and corrected.

Post-test ACK raw68=0, timeout rawc0=1 (could be stale from transient test; no baseline comparison). Controller interrupt raw114=0, error mask raw118=0. These do not establish a fault on lanes1..3. Current evidence does not justify permuting wiring.

Read/write tools and source: build/dsi-lane-map/. Mapping write is volatile and restored to original0. Next investigations should preserve correct version distinctions and avoid claiming lane-map8421 applies until proven. Current panel module is no-PPS diagnostic from preceding experiment; user reports no improvement. Standard-PPS baseline should be restored for further experiments.

Restored standard-PPS diagnostic module SHA256b26ea6bbb0eddafa96b0654046697b44029f841470d59c09ab56fcee6bb2ee1f and matching source. Reboot succeeded, PPS senderr0, beforeC1=8928 after8938 as previously. Test pattern rewritten. No lane override persists.

Additional bounded interface readback: INTF_CONFIG2=1011, displayHCTL01af0048, displayDATAHCTL00fb0048 (180 cycles), PANEL_FORMAT2100. Expected software RGB888213f writes back2100 (low6bits not observable here); no unsupported inference that this is faulty. Single diagnostic write213f left readback unchanged.

Focused next hypothesis: compressed data bus configuration, not DRM plane. Direct vendor INTF timing engine disassembly10ac98..10acb8 confirms1080-byte widebus data window ceil1080/6=180, agreeing liveDATAHCTL00fb0048. No confirmed interface timing mismatch in this immediate path. Vendor lane callback alone remains insufficient to replace working legacy register semantics.

Parallel bounded INTF packing audit also found no actionable mismatch: vendor config2widebus/compression/data_hctl bits0/12/4 and180cycle data window match current. See vendor-intf-packing-followup.md. Vendor host_setup conditional ctrl+560 writes raw2c8=10000000; applicability/meaning not yet resolved. Do not write that register without identifying it and active capability.

Vendor host register identified from Qualcomm reference: raw 0x2c8 is DSI_DEBUG_CTRL; bit28 is phy_isolation_enabled, not a data packing register. Runtime A/B on the active display: before=0x00000004, after setting bit28=0x10000004. Test pattern rewritten; no reboot or source change. Current bit remains set until restored. This is the first experiment targeting a vendor-only PHY isolation control.

Bit28 A/B result: setting DSI_DEBUG_CTRL bit28 changed display to black immediately. It was cleared back to0x00000004, then a full DSI reboot/reinitialization was performed. Fastboot recovery required set_activea; no partition was flashed. After reboot standard baseline logs are present: vendor PHY timings, single-packet DSC, PPSerr0, DDIC reads. Test pattern rewritten. Do not enable bit28 in a display patch; it is required only when the vendor phy_isolation_enabled condition is genuinely configured.
