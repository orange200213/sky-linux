# Improved DSC buffer startup state

Preserved at user's request after first clear improvement. Do not alter running display until user supplies SVG.

boot.img SHA256: 50846c5b4503e78302ed39e8ca388351e42243c4027cbe470ad31b95e6e0a840
Written only to boot_a (/dev/block/sde13). Current recovery remains accessible via recovery boot.

Baseline: build/phy-vendor-timing/sky-vendor-phy.img. Same baseline DTB and physical load addresses (Image 0xc0000000, DTB 0xc4a00000).
Diagnostic patch: SM4450 output-buffer capacity 600 / active slices - 1 ->299 for2 slices. ENC_DF_CTRL=04ac0203 at first modeset, confirmed by logs and readback. No multiplex or packetization changes. Panel module backed up from device, SHA256 b26ea6bbb0eddafa96b0654046697b44029f841470d59c09ab56fcee6bb2ee1f.

User visual report: clearly improved; upper half uniform diagonal alternating red/green stripes; lower half uniform diagonal blue/white stripes; no flicker; kernel text visible before test pattern. SVG pending. Kernel text report is user observation, not independently verified.

Live MMIO change1199->299 previously had essentially no effect; startup299 does. Do not conflate these test outcomes. Do not yet claim full display fix or proven latch behavior.

SHA256SUMS contains hashes of boot image, Image, DTB, panel module, DSC source, patch and runtime log. Pattern remains sky-mainline/pattern.rgb (BGRA1080x2460 four quadrants). No further display changes after this snapshot.
