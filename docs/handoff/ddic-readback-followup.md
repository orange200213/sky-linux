# DDIC compression readback

After no visible change from single-packet vendor grouping, added read-only C0/C1/C2 DCS diagnostics after the existing PPS write. Module built, deployed and rebooted successfully; artifact `build/panel-ddic-read/panel-xiaomi-m19.ko`, SHA256 `1671fb2fe7e9a29870146c7b123e5c394501988779a3e204b34c43b8bf4e28bd`.

Actual log:

```
C0 ret=1 03
C1 ret=16 89 38 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16
C2 ret=2 1b a0
```

Expected C1 second byte from vendor init is28, while readback is38. All other C1 bytes match; C0 and C2 match. No datasheet definition for bit0x10 has been established. This must not be treated as proof of corruption or as permission to change bpp/block-pred settings. It could be read-only/reserved/status behavior, or reflect processing after PPS; immediate-before-PPS readback is a concrete follow-up to distinguish ordering effects.

Similar mainline Novatek `panel-novatek-nt36672e.c` uses C1 prefix89/28 and C2=1ba0, suggesting useful DDIC-family reference but not establishing exact controller identity or bit semantics.

Runtime log: `build/panel-ddic-read/runtime.log`. Complete10627200-byte quadrant pattern rewritten after reboot. Display-affecting parameters are unchanged; no new visual comparison is needed solely for these reads.

DSC encoder read-only MMIO audit saved as `build/dsc-register-audit/registers.txt`; tool source `read-dsc.c`. Hardware values include DF_CTRL12bc0203, MAIN10120258, HRD020e00aa, SCALE_INC_DEC00070071, OFFSETS1ba01800, FLATNESS983, wrapper801. CMN offset0 reads0 despite configured0x100; register read semantics unknown, so not diagnosed as a fault.

Vendor output-buffer feature bits remain unresolved by bounded static catalog inspection; see `vendor-dsc-capabilities-followup.md`. Do not claim those features equal mainline0 without evidence.

Second diagnostic adds C1 read immediately before PPS (after display-on), to distinguish PPS side effects from register read semantics. New module SHA256b26ea6bbb0eddafa96b0654046697b44029f841470d59c09ab56fcee6bb2ee1f. Display writes unchanged.

Definitive before/after comparison: before PPS C1=89 28 ...; after PPS=89 38 ... (all other bytes match). This isolates the bit0x10 change to PPS processing or its immediate aftermath, rather than failed init writes. Meaning remains unknown.

Next experiment replays vendor C0=03/C1(original89 28...)/C2=1ba0 after the existing PPS, then reads back C0/C1/C2. Keeps PPS transmission, current PHY, 90Hz, lines3, packet grouping unchanged. Artifact build/panel-ddic-restore/panel-xiaomi-m19.ko SHA256d3bc60a905f0986d1ae4c294bcad45f235ddb4ae342dd66fe7753c47f0cbd3e6. This is diagnostic, not a claim that bit0x10 is erroneous.

Restore experiment runtime confirmed: before PPS89/28; vendor PPS senderr0; replay C0/C1/C2err0; subsequent readback C089/28 correction: C0=03, C1=89 28 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16, C2=1ba0, exactly vendor initialization. Test pattern10627200 bytes rewritten. Runtime log build/panel-ddic-restore/runtime.log. Awaiting visual outcome; no decoding conclusion yet.

User reports post-PPS C0/C1/C2 restore makes no visible change. Next isolated module experiment skips standard PPS packet while preserving DPU DSC config, original private init, replay C0/C1/C2 and other settings. No claim this matches vendor flow; tests whether PPS packet processing has a visual effect.

No-PPS experiment deployed and verified: marker diagnostic PPS skippederr0; C0=03,C1=89 28 00 14 00 aa 02 0e 00 71 00 07 05 0e 05 16,C2=1ba0. Current artifact build/panel-no-pps/panel-xiaomi-m19.ko SHA25644a74be95dde5137584b152cc49980cb5a87d7492cc4e1abeacaa9a63a06a311. Full10627200-byte test pattern written after reboot. Runtime log build/panel-no-pps/runtime.log. Awaiting visual result.

User reports no-PPS experiment also makes no visible change. Do not leave skipping PPS as a proposed fix; restore standard-PPS module for next meaningful display experiment.
