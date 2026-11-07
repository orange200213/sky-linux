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
| **Touch on other variants** | the same vendor build also ships `focaltech_8725_fw.bin` + `focaltech_ft8720/8725_hx.ini`, i.e. some Redmi 12 5G variants use a **FocalTech** controller instead of the Novatek NT36672C — another reason the touch recipe here is JP-unit specific |
| **Bluetooth (WCN6750)** | M9, not started | `hci_qca` already matches `qcom,wcn6750-bt`; needs `enable-gpios`, the BT firmware from the vendor partition and the WPSS/AON state that Wi-Fi already sets up. |
| **Fingerprint** | no driver | the vendor DT carries **two** variants: `fpc,fpc1020` and `goodix,fingerprint` (side-mounted). The vendor split-firmware also has `fpctzappfingerprint.b*` (TEE app), so the sensor itself is likely FPC on this unit. |
| **NFC** | no driver | **NXP SN100/SN220** — the vendor image ships `libnfc-SN220.conf`, `libnfc-mtp-SN100*.conf`, `nfc_nci.nqx.default.hw.so`, `vendor.nxp.hardware.nfc@2.0-service` and `init_nfc_sn1xxx.rc`; our DT has only the `nfc_enable` pinctrl group. Mainline has no SN100 driver (only `nfcsim`/PN544 class). |
| **Haptics (linear motor)** | no driver | `aw8695` (AWINIC) appears in the vendor image; the motor is not modelled in our DT. |
| **GPU (Adreno 613)** | node exists but `status = "disabled"` | no 3D acceleration; the display pipeline (DPU) does not need it. `CONFIG_DRM_MSM=y` is built, so the work is DT (GMU/OPP/IOMMU) + firmware (`a615_zap.mbn`-style) bring-up. |
| **Video encode/decode (Venus)** | `CONFIG_VIDEO_VENUS` not set, no DT node | not startable without the firmware (`venus.mdt`) and the SM4450 Venus compatible. |
| **USB host / OTG** | peripheral-only (`dr_mode = "peripheral"`) | no role switch wired up; there is no USB3 on this board (HS PHY only). |
| **Deep suspend** | untested | display blank/unblank and panel sleep work, touch follows the panel, but `s2idle`/`mem` has not been validated end to end. |

## 3. Sensors — which parts are in the phone

The AP DT has no sensor nodes because they sit behind the sensor DSP. The part numbers are
recoverable from the vendor partition: the SSC registry entries are JSON files named
`jsonsky_<driver>_<instance>_*.json`, and the ADSP firmware carries the driver strings.

The phone's own registry files were pulled from the running unit (`/vendor/etc/sensors/`,
copies in `~/redmi125g/firmware-jp-2024/sensors/`). The `config/sky_*.json` set is what this
device actually registers:

| Function | Part | Registry file |
|---|---|---|
| Accelerometer (primary) | **Bosch BMA4xy** (BMA4xx family) | `sky_bma4xy_first_0.json` |
| Accelerometer (secondary) | **ST LIS2HH12** | `sky_lis2hh12_second_0.json` (also `lis2hh12` ×153 in `adsp.b*`) |
| Magnetometer | **MEMSIC MMC56X3** or **QST QMC630x** (second source) | `sky_mmc56x3x_0_{NO,WITH}-NFC.json`, `sky_qmc630x_0_{NO,WITH}-NFC.json` |
| Ambient light | **ROHM BU27030** | `sky_bu27030_0.json` |
| Proximity / ALS combo | **Semtech SX933x** | `sky_sx933x_0_final.json` |
| Gyroscope | **none** | there is no `sky_*gyro*` registry entry — the phone has no gyro (the `lsm6dst` strings in the vendor image are generic library support) |

The `{NO,WITH}-NFC` variants confirm the board exists in NFC and non-NFC configurations; the
JP unit sold here **has NFC** (`libnfc-SN220.conf`, `libnfc-mtp-SN100*.conf`,
`vendor.nxp.hardware.nfc@2.0-service` and `init_nfc_sn1xxx.rc` are all present in `/vendor`,
see §2).

## 4. Cameras — which sensors the board supports

The camera HAL ships one module per sensor variant, and the names carry the part number. From
`/vendor` of the JP package (`com.qti.sensor.m19_*`):

| Slot (vendor DT) | Variants |
|---|---|
| `cam-sensor0` (csiphy0, cci-master 0) — **main** | **OV50D40** (`m19_truly_ov50d40`), **SC520CS** (`m19_truly_sc520cs`), **S5KJNSSQ33** (`m19_sunny_s5kjnssq33`) — all 50 MP class |
| `cam-sensor1` (csiphy2, cci-master 1) — front/aux | **OV8856** (`m19_sunny_ov8856`, `m19_ofilm_ov8856`), **S5K5E9YU05** (`m19_sunny_s5k5e9yu05`) |
| `cam-sensor2` — aux | **OV02B10** (`m19_truly_ov02b10`), **OV2180** (`m19_sunny_ov2180`) — 2 MP class |

So the phone is a 50 MP main + 2 MP aux + (8 MP or 5 MP) front design with **three
second-source options for the main camera**.

### Which cameras does *our* unit actually have? — answered from the running HyperOS

Booted the stock Android 14 image on this unit and read the camera characteristics
(`adb shell dumpsys media.camera`):

| camera | facing | active array | verdict |
|---|---|---|---|
| 0 (main) | back | 4080×3072 (12.5 MP binned → 50 MP full, 8160×6144) | **Samsung S5KJN1** — the reported physical size 5.304×3.994 mm matches a 1/2.76", 0.64 µm sensor (OV50D40 is 1/2.88", SC520CS 1/2.5", both off by ≥0.3 mm). Module vendor is **AAC or Sunny** (both goldens ship: `golden_aac_s5kjn1_wide_ii.bin`, `golden_sunny_s5kjn1_wide_i.bin`) |
| 1 (front) | front | 2592×1944 (5 MP) | **Samsung S5K5E9YU05** (the OV8856 alternative is 8 MP / 3264×2448, so it is *not* fitted) |
| 2 (aux) | back | 1600×1200 (2 MP) | **OV02B10 or OV2180** (both are 2 MP; needs an EEPROM read to separate) |
| 3–5 | — | duplicates of the above | CamX exposes extra logical/physical ids |

So the phone is 50 MP main (S5KJN1) + 2 MP aux + 5 MP front, not the 8 MP front /
OV50D40 combination that the module list alone suggested.

The rest of this section records what was tried *before* Android was booted, i.e. why the
images alone could not answer it.

Checked and ruled out (2026-11-06, in TWRP):

* `dtbo_a/b` — only names panel variants (`m19_36_02_0a_fhd_dsc_vid`, `m19_42_03_0b/0c`), no
  camera strings;
* `/vendor/etc/camera/` — CamX configs, AI-ISP models and Xiaomi goldens
  (`golden_aac_s5kjn1_wide_ii.bin`, `golden_ofilm_ov48b2q_wide_i.bin`,
  `golden_sunny_s5kjn1_wide_i.bin`), i.e. IQ-validation files for **S5KJN1** (AAC / Sunny) and
  **OV48B2Q** (Ofilm) wide modules — these add two more candidates but do not say what is
  fitted;
* `/persist/camera/` — empty on this unit;
* `cust`, `opconfig`, `build_*.prop` — no camera/sensor identifiers;
* `/data` — was taken over by our mainline rootfs, so the Android CamX logs are gone.

The three ways to settle it:

1. **Boot stock Android once** and read the log:
   `adb logcat -d | grep -iE "camxsensor|camera.*sensor_open|probe.*sensor"` (CamX prints the
   module name it opened, e.g. `m19_truly_ov50d40`);
2. **Read the sensor ID over CCI** once a CAMSS driver exists for SM4450 (the sensor's chip-id
   register is at a well-known I²C address per part);
3. **Physical inspection** (teardown photos of the main module).

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
