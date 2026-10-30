# DSI video host register audit

Current runtime DSI raw registers (logical offsets; controller base ae94000 with4-byte shift) were read after baseline reboot:

```
VID_CFG0 0x02009230
VID_CFG1 0x00000000
ACTIVE_H 0x01b00048
ACTIVE_V 0x09b00014
TOTAL    0x09b00014
HSA      0x000c0000
HBP      0x000a0000
HFP      0x14000000
BLLP     0x06100006
```

`VID_CFG0` contains the expected four-lane/widebus/burst-video settings. DSC packet and timing audit remains pending vendor comparison; no runtime register has been modified.
