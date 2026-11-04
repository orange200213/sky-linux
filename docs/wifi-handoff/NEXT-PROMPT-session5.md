# Next prompt: continue WCN6750 Adrastea SNOC Wi-Fi port

Read these first:

1. `docs/wifi-handoff/HANDOFF-snoc-mainline.md`
2. `docs/wifi-handoff/FINDINGS-session3-adrastea-shadow.md`
3. `docs/wifi-handoff/FINDINGS-session4-stock-adrastea.md`

Device: Xiaomi Redmi 12 5G (`sky`), SSH `root@192.168.7.1`, password `sky`.
Do not use adb/fastboot through SSH. Ask before requesting fastboot or dmesg.
Keep tool calls minimal; prefer SSH and web search.

Current safe device state:

```text
kernel #90
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
snoc_shadow_probe=y
```

Measured read-only SR_APSS values:

```text
0x30000=0x0
0x30008=0x8
0x3000c=0x0
0x30010=0x7ff
0x30014=0x0
0x30080=0x0
0x300e0=0x0
0x32060=0x0
0x32064=0x11530000
0x32068=0x32002002
0x3206c=0xbb01f000
0x32070=0x0010200c
0x32074=0x01bcf808
0x32078=0x0
```

Normal working milestones:

```text
WPSS/QMI instance 0/MSA/BDF/regdb/cal report/CE direct map
```

Remaining failure:

```text
ath11k_hal_srng_setup() -> offset 0xa34874
external abort 0x96000010
WPSS _CE_misc_handler:2227
```

New source evidence:

- Public `adrastea_reg_def.h` defines SR_APSS.
- Stock `ADRASTEA_SHADOW_REGISTERS` maps only CE write-index operations:
  source CE 0,3,4,5,7 -> slots 0,3,4,5,7; destination CE 1,2,5,7,8,9,10,11 -> slots 13,14,17,19,20,21,22,23.
- Stock reads `SHADOW_ADDRESS[n] << 2`.
- Complete `ADDRESS_VALID` initialization is not in the exposed source.

Next objective: recover exact stock shadow slot initialization and test one
harmless slot. Do not enable guessed mode 5 or write UMAC offset 0xa34874 until
that protocol is established. Extended IRQ dispatch remains a separate task
after DP init.
