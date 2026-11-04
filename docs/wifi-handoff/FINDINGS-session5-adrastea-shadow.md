# Session 5: Adrastea SR_APSS read-only hardware probe

## Image and device

- Image: `wifi-backup/out/sky-shim-boot-v4-wifi-k91-snoc-shadow-status.img`
- Kernel: `7.3.0-g78458d4eea4e-dirty #90`
- DTB: `wifi-snoc-fdt.dtb`
- Device: Xiaomi Redmi 12 5G (`sky`), WCN6750 Adrastea on SM4450

Runtime parameters:

```text
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
snoc_shadow_probe=y
```

## Read-only result

The diagnostic worker read only registers defined in the public Adrastea header:

```text
+0x30000 SR_CONTROL          = 0x00000000
+0x30008 SHADOWREG_STATUS    = 0x00000008
+0x3000c SR_INTERRUPT_STATUS = 0x00000000
+0x30010 SR_INTERRUPT_ENABLE = 0x000007ff
+0x30014 SR_INTERRUPT_CLEAR  = 0x00000000
+0x30080 DIRTY               = 0x00000000
+0x300e0 ADDRESS_VALID       = 0x00000000
+0x32060 FORCE_WAKE          = 0x00000000
+0x32064 SW_SCRATCH          = 0x11530000
+0x32068 PMM_SR_LSB          = 0x32002002
+0x3206c PMM_SR_MSB          = 0xbb01f000
+0x32070 WCSSAON_SR_LSB      = 0x0010200c
+0x32074 WCSSAON_SR_MSB      = 0x01bcf808
+0x32078 INVALID_ADDR_ACCESS = 0x00000000
```

## Interpretation

- The SR_APSS register block is directly readable via the `0x22800000` DT
  membase.
- `SHADOWREG_STATUS=0x8` confirms the WCSS core wake/sleep status bit is set.
- `SR_INTERRUPT_ENABLE=0x7ff` is already configured.
- `DIRTY=0`, `ADDRESS_VALID=0`, and `INVALID_ADDR_ACCESS=0` immediately before
  DP failure: no shadow slots are valid or dirty at that point.
- `SW_SCRATCH=0x11530000` is a useful nonzero APSS-local sanity value.
- The read-only probe did not cause the external abort.
- The old `0x300e4` status address is invalid and must not be used.

## Remaining failure

The normal ath11k sequence still completes QMI/MSA/BDF/cal report, then fails:

```text
ath11k_hal_srng_setup() -> write offset 0xa34874
synchronous external abort 0x96000010
WPSS copy_engine.c:_CE_misc_handler:2227
```

## Next work

Stock source only exposes CE write-index shadow slot mappings; it does not show
where `SHADOW_ADDRESS[n]` and `ADDRESS_VALID` are initialized. Do not enable the
experimental mode 5 or redirect `0xa34874` to slot 0. Recover the exact slot
initialization order first, then test one harmless slot and inspect status,
dirty, invalid-address, and shadow value behavior.
