# M2 — USB gadget (CDC ACM serial console)

**Result:** plugging the phone into a PC gives `/dev/ttyACM0` on the host and a
root shell on `/dev/ttyGS0` on the device.

This was the single most valuable step of the whole project: from here on,
debugging no longer needs photos of the screen.

## Hardware path

```
SoC DWC3 controller @ 0xa600000
  └─ glue:      drivers/usb/dwc3/dwc3-qcom.c        ("qcom,snps-dwc3")
  └─ HS PHY:    drivers/phy/qualcomm/phy-qcom-snps-femto-v2.c
                ("qcom,sm4450-usb-hs-phy", "qcom,usb-snps-hs-7nm-phy") @ 0x88e3000
  └─ USB3/DP PHY @ 0x88e8000: present in the vendor DT but *disabled* there too
```

The vendor DT has `ssphy@88e8000: status = "disabled"`, so USB3 is out of scope:
we only need the HS/FS device path.

## The three things that had to be right

### 1. Kernel config for a gadget

```
CONFIG_USB=y CONFIG_USB_GADGET=y CONFIG_USB_DWC3=y CONFIG_USB_DWC3_QCOM=y
CONFIG_PHY_QCOM_USB_SNPS_FEMTO_V2=y
CONFIG_USB_CONFIGFS=y CONFIG_USB_CONFIGFS_ACM=y CONFIG_USB_CONFIGFS_NCM=y
```

Note: `PHY_QCOM_QMP_UFS` (and friends) inherit their tristate from
`PHY_QCOM_QMP`. If the parent is `=m`, the child can never be `=y`. With no
module loader in the initramfs, everything must be built in.

### 2. `qcom,select-utmi-as-pipe-clk` — the actual bug

Symptom (from `dmesg` captured through pstore):

```
[0.214038] phy phy-88e3000.phy.0: hsphy init done
[0.214313] phy phy-88e3000.phy.0: set_mode mode=6   (PHY_MODE_USB_DEVICE)
[0.216167] g_serial gadget.0: Gadget Serial v2.4
[0.219547] dwc3-qcom a600000.usb: failed to enable ep0out
```

The PHY was fine; the **controller** could not even start its control endpoint,
because it had no pipe clock. In `dwc3-qcom.c`:

```c
	ignore_pipe_clk = device_property_read_bool(dev,
				"qcom,select-utmi-as-pipe-clk");
	if (ignore_pipe_clk)
		dwc3_qcom_select_utmi_clk(qcom);
```

The pipe clock normally comes from the USB3/DP QMP PHY. We do not load that PHY
(no USB3), so the DT must say "use the UTMI clock as pipe clock":

```dts
&usb_1 {
	dr_mode = "peripheral";
	qcom,select-utmi-as-pipe-clk;
	phys = <&usb_1_hsphy>;
	phy-names = "usb2-phy";
};
```

### 3. `dr_mode = "otg"` + role switch does *not* work here

With the Anbernic-style node (`dr_mode = "otg"`, `usb-role-switch`, endpoints to
`pmic-glink`) the gadget fails the same way (`failed to enable ep0out`) because
the role is never resolved before gadget start. `dr_mode = "peripheral"` with
the role-switch properties removed is what works.

If the Type-C data path ever needs the PMIC role switch, the ordering problem
has to be solved by making the typec port probe before dwc3 starts its gadget
(not just by enabling `CONFIG_TYPEC_QCOM_PMIC`).

## Gadget design: configfs, not `g_serial`

`CONFIG_USB_G_SERIAL=y` is convenient but it grabs the UDC during
`late_initcall`, so a configfs gadget created later cannot bind. For M3
(ACM + NCM in one composite device) the built-in `g_serial` must be disabled:

```
CONFIG_USB_G_SERIAL=n
```

and the initramfs builds the gadget with configfs:

```sh
G=/sys/kernel/config/usb_gadget/sky
mkdir -p $G; echo 0x1d6b > $G/idVendor; echo 0x0104 > $G/idProduct
mkdir -p $G/configs/c.1 $G/functions/acm.usb0 $G/functions/ncm.usb0
ln -s $G/functions/acm.usb0 $G/configs/c.1/
ln -s $G/functions/ncm.usb0 $G/configs/c.1/
echo $(ls /sys/class/udc | head -1) > $G/UDC
```

## Host side

```
usb 4-1.3: new high-speed USB device number 54 using xhci_hcd
usb 4-1.3: New USB device found, idVendor=1d6b, idProduct=0104
cdc_acm 4-1.3:1.0: ttyACM0: USB ACM device
```

```bash
sudo picocom -b 115200 /dev/ttyACM0      # or screen /dev/ttyACM0 115200
```

## How it was debugged (worth reusing)

* `dmesg` was pushed into ramoops by deliberately panicking (`echo c >
  /proc/sysrq-trigger`) and then read from TWRP — see `docs/debugging.md`.
* A temporary `dev_info()` in the femto PHY driver printed the register state
  after `phy_init()` (`kernel/patches-debug/0003-…`), which proved the PHY was
  programmed correctly and moved the suspicion to the controller.
* `device_property_read_bool()` values can be verified from user space:
  `ls /proc/device-tree/soc@0/usb@a600000/`.
