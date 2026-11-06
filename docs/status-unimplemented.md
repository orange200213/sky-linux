# What still does not work (status 2026-11-06)

Everything below was checked against this tree and the stock images of the Japanese variant
(`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`), not from memory. "Missing" means no
mainline driver, no DT node, or both.

## 1. Working today

| Area | State |
|---|---|
| boot (shim + mainline kernel + DTB) | ✅ |
| display (DPU/DSI, `m19` panel, backlight, fbcon) | ✅ |
| storage (UFS, `userdata`) | ✅ |
| USB gadget: CDC ACM + CDC NCM + SSH | ✅ (needs `iommus = <&apps_smmu 0x540 0>` once high memory is enabled) |
| RAM | ✅ 4 GB unit → ~3.2 GB usable (five segments) |
| power / charging (ADSP + QRTR + `pmic-glink`) | ✅ |
| touch (NT36672C, GPI DMA, runtime recovery) | 🚧 **partial** — raw evdev only, no desktop/WM test |
| Wi-Fi (WCN6750 via the out-of-tree `qcacld-3.0` port) | ✅ works, still being optimised |

## 2. Not implemented

| Area | What is missing | Notes |
|---|---|---|
| **Sensors** (accel, gyro, magnetometer, ALS/proximity) | no mainline path at all | they hang off Qualcomm's **SSC** (sensor DSP, `sensors.ssc.so` in the vendor image), not the AP I²C bus. Parts are listed in §3. A driver would have to talk QMI/SSC to the ADSP — nothing upstream. |
| **Camera** | `CONFIG_QCOM_CAMSS` is not enabled and there are no `cci`/`csiphy`/`camss` nodes in our DTS | the vendor DT has `csiphy0..2` (`qcom,csiphy-v1.2.3`) + `cci0@ac15000` + 3 `qcom,cam-sensor` slots. Mainline has no SM4450 CAMSS support yet. Sensors in §4. |
| **Audio** | `CONFIG_SND_SOC=y` but no sound card, no codec node, no LPASS | the codec is **WCD937x** (vendor DT: `qcom,wcd937x-codec`, `qcom,wcd937x-slave`). Needs an ASoC machine driver + codec + LPASS macro/tx-macro bring-up. |
| **Modem / calls / SMS / mobile data** | no MPSS remoteproc node in our DTS (only ADSP and WPSS exist) | MPSS would need `qcom,sm4450-mpss-pas`, the `modem.mdt` split firmware from `NON-HLOS.bin`, and a data path (rmnet) — a large project on its own. |
| **Bluetooth (WCN6750)** | M9, not started | `hci_qca` already matches `qcom,wcn6750-bt`; needs `enable-gpios`, the BT firmware from the vendor partition and the WPSS/AON state that Wi-Fi already sets up. |
| **Fingerprint** | no driver | the vendor DT carries **two** variants: `fpc,fpc1020` and `goodix,fingerprint` (side-mounted). The vendor split-firmware also has `fpctzappfingerprint.b*` (TEE app), so the sensor itself is likely FPC on this unit. |
| **NFC** | no driver | vendor strings point at an **NXP SN100** (34 hits, plus `nfc_nci`/`pn557` library strings); DT has only the `nfc_enable` pinctrl group. |
| **Haptics (linear motor)** | no driver | `aw8695` (AWINIC) appears in the vendor image; the motor is not modelled in our DT. |
| **GPU (Adreno 613)** | node exists but `status = "disabled"` | no 3D acceleration; the display pipeline (DPU) does not need it. `CONFIG_DRM_MSM=y` is built, so the work is DT (GMU/OPP/IOMMU) + firmware (`a615_zap.mbn`-style) bring-up. |
| **Video encode/decode (Venus)** | `CONFIG_VIDEO_VENUS` not set, no DT node | not startable without the firmware (`venus.mdt`) and the SM4450 Venus compatible. |
| **USB host / OTG** | peripheral-only (`dr_mode = "peripheral"`) | no role switch wired up; there is no USB3 on this board (HS PHY only). |
| **Deep suspend** | untested | display blank/unblank and panel sleep work, touch follows the panel, but `s2idle`/`mem` has not been validated end to end. |

## 3. Sensors — which parts are in the phone

The AP DT has no sensor nodes because they sit behind the sensor DSP. The part numbers are
recoverable from the vendor partition: the SSC registry entries are JSON files named
`jsonsky_<driver>_<instance>_*.json`, and the ADSP firmware carries the driver strings.

| Function | Part | Evidence |
|---|---|---|
| Accelerometer #1 | **Bosch BMA4xy** (BMA4xx family) | registry entry `jsonsky_bma4xy_first_0` |
| Accelerometer #2 | **ST LIS2HH12** | registry `jsonsky_lis2hh12_second_0`; `lis2hh12` ×153 in `adsp.b*` |
| Gyroscope / 6-axis | **ST LSM6DST** | `lsm6dst` ×10 in the vendor image |
| Magnetometer | **MEMSIC MMC56X3** *or* **QST QMC630x** | registry `jsonsky_mmc56x3x_0_`, `jsonsky_qmc630x_0_` (supplier variants) |
| ALS + proximity | **Semtech SX933x** | registry `jsonsky_sx933x_0_final` |
| ALS (alternate) | **ROHM BU27030** | registry `jsonsky_bu27030_0` |

Supplier variants mean a given unit gets one of the alternatives (that is how Xiaomi handles
second sources); which one is fitted can only be confirmed by probing the SSC of that unit.

## 4. Cameras — which sensors the board supports

The camera HAL ships one module per sensor variant, and the names carry the part number. From
`/vendor` of the JP package (`com.qti.sensor.m19_*`):

| Slot (vendor DT) | Variants |
|---|---|
| `cam-sensor0` (csiphy0, cci-master 0) — **main** | **OV50D40** (`m19_truly_ov50d40`), **SC520CS** (`m19_truly_sc520cs`), **S5KJNSSQ33** (`m19_sunny_s5kjnssq33`) — all 50 MP class |
| `cam-sensor1` (csiphy2, cci-master 1) — front/aux | **OV8856** (`m19_sunny_ov8856`, `m19_ofilm_ov8856`), **S5K5E9YU05** (`m19_sunny_s5k5e9yu05`) |
| `cam-sensor2` — aux | **OV02B10** (`m19_truly_ov02b10`), **OV2180** (`m19_sunny_ov2180`) — 2 MP class |

So the phone is a 50 MP main + 2 MP aux + (8 MP or 5 MP) front design with **three
second-source options for the main camera**. Which one this unit has needs a probe of the CCI
bus (or reading the sensor EEPROM) — the images cannot tell us.

## 5. How to reproduce this inventory

```sh
# JP stock package
IMG=/home/orange/sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0/images
# super.img is an Android sparse image; LP metadata puts vendor_a at ~4096 MB (831 MB)
# camera + sensor identifiers live in the vendor (erofs) part:
strings -a <vendor_a-extract> | grep -oE 'com\.qti\.sensor\.[A-Za-z0-9_]+' | sort -u
strings -a <vendor_a-extract> | grep -oE 'jsonsky_[a-z0-9_]+' | sort -u
# sensor driver strings are in the DSP firmware:
sudo mount -o ro,loop,offset=0 $IMG/NON-HLOS.bin /mnt
cat /mnt/image/adsp.b* | grep -aoE '(lis2hh12|lsm6ds[a-z]*|bma4[a-z0-9]*)' | sort | uniq -c
```

`build/sky.dts` (the decompiled vendor DT in the main tree) is the source for the codec, the
CSIPHY/CCI instances, the fingerprint/NFC pin groups and the three camera slots.
