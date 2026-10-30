# M6 — power and charging

Battery and charger reporting work: `/sys/class/power_supply/` exposes
`battery`, `qcom-battmgr-usb` and `qcom-battmgr-wls`, and the values are real
(measured on the device: 4.44 V, `status=Full`, `current_now=0`, 28 C,
`health=Good`, VBUS 5.09 V).

## How charging works on this board

The charger is managed by the ADSP, not by the AP. The vendor DT describes it as

```dts
pmic-glink {
	compatible = "qcom,pmic-glink";
	qcom,pmic-glink-channel = "PMIC_RTR_ADSP_APPS";
	qcom,subsys-name = "lpass";
	qcom,protection-domain = "tms/servreg", "msm/adsp/charger_pd";
	qcom,battery_charger {
		compatible = "qcom,battery-charger";
		qcom,shutdown-voltage = <0xce4>;
		/* 36 thermal-mitigation steps */
	};
};
```

so the path is

```
ADSP firmware up
  -> glink channels (IPCRTR, PMIC_RTR_ADSP_APPS, adsp_apps)
  -> qrtr_smd binds IPCRTR, qcom-pd-mapper serves the SM4450 PD table
  -> PDR indication for msm/adsp/charger_pd
  -> pmic_glink creates the power-supply auxiliary device
  -> qcom_battmgr answers the QMI requests
```

Four things had to be fixed to get there:

| Fix | Detail |
|---|---|
| ADSP firmware | not in the fastboot images; extract `adsp.mdt` + `adsp.b00..b41` from `image/` in `NON-HLOS.bin` (split-mdt format, mainline loads it) and install under `/lib/firmware/qcom/sm4450/xiaomi-sky/` |
| firmware path | the DTS inherited Anbernic's `qcom/sm4450/RG55G1/adsp.mbn`; use our own directory and `.mdt` |
| ADSP start time | the remoteproc auto-boot runs at 0.37 s, long before `switch_root`, so the firmware is not there yet. The rootfs `/init` retries in the background until the ADSP stays up |
| QRTR stack | `CONFIG_QRTR`, `CONFIG_QRTR_SMD`, `CONFIG_QCOM_PD_MAPPER` were all off, and the ADSP `glink-edge` had no `IPCRTR` / `PMIC_RTR_ADSP_APPS` child nodes |

Also fixed: our `pmic-glink` node carried the machine compatible list instead of
`qcom,pmic-glink`, so the driver never bound and `power_supply` stayed empty.

## Registering the retry loop correctly

The first attempt at the ADSP retry was appended to the end of `/init`, after a
blocking loop that keeps spawning shells on `ttyGS0`, so it never ran. It now sits
at the top of the script (right after `export PATH`, using `/bin/busybox`
explicitly because `$BB` is not defined yet).

## Known noise

```
qcom_battmgr.pmic_glink_power_supply ...: unknown notification: 0x50
```

Xiaomi-specific QMI notification that mainline does not decode. Harmless.

## Still to do

* charging current limits / thermal mitigation from DT (`qcom,thermal-mitigation`)
* `charge_type`, `time_to_full`, cycle count sanity
* power-key driven shutdown and suspend/resume with the charger PD alive
