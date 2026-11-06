# Firmware inventory (JP unit, 2026-11-06)

Everything was pulled from the phone in TWRP or out of the stock package
`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`. **The binaries are not in this repository**
(Qualcomm / NXP / Novatek / Xiaomi licensing); they live on the build machine under
`~/redmi125g/firmware-jp-2024/` (checksums in its `SHA256SUMS.txt`). This file documents what
exists and which subsystem it belongs to, so a future session knows where to look.

All of it came off the phone in TWRP or out of the stock package. **Nothing here is
redistributable** (Qualcomm / NXP / Novatek / Xiaomi licenses) — keep it out of git.

| dir / file | size | source | what it is |
|---|---|---|---|
| `bt/` (30 files) | 768 KB | `/dev/block/by-name/bluetooth_a` (FAT16; Android mounts it at `/vendor/bt_firmware`) | Bluetooth firmware: `apbtfw*`/`apnv*`, `cmbtfw*`/`cmnv*`, `crbtfw*`/`crnv*` (+ `.ver`). **No `ms*` files** → see `sky-linux/docs/bt-handoff/HANDOFF.md` for the `qcom,wcn3990-bt` + `cr*` route |
| `vendor-firmware/` (134 entries, 32 MB) | | `/vendor/firmware/` | everything the vendor ships, categorised below |
| `sensors/` (62 files) | 304 KB | `/vendor/etc/sensors/` | SSC config; `config/sky_*.json` is what this unit registers |
| `camera-etc/` (30 files, 17 MB) | | `/vendor/etc/camera/` | CamX configs, AI-ISP models, Xiaomi dcal goldens (`golden_*_wide_*.bin`) |
| `wpss.*` (11 files) | 6.1 MB | `NON-HLOS.bin` → `image/adrastea/` | WPSS remoteproc firmware (Wi-Fi) |
| (kept elsewhere) | | | `~/redmi125g/sky-mainline/firmware/novatek_ts_fw.bin` = touch FW 0x18; `~/redmi125g/wifi-backup/firmware/image/adrastea/` = WCN6750 WLAN set (bdwlan*, regdb) |

Checksums: `SHA256SUMS.txt` (bt + sensors + wpss).

## `vendor-firmware/` breakdown

| group | files | size | subsystem |
|---|---|---|---|
| `CAMERA_ICP*` (`.b00..b24`, `.elf`) | 25 | 11 MB | camera ICP (image co-processor) firmware |
| `evass*` | 44 | 4.5 MB | EVA (video-analytics) subsystem firmware |
| `vpu20_4v*`, `vpu20_1v*` | 17 | 8.6 MB | VPU (video processing unit) firmware |
| `venus.mbn`, `venus_v6/v7.mbn`, `venus_unsigned.mbn` | 4 | ~7 MB | Venus video codec firmware |
| `a610/a662/a710/a730_zap.*`, `gen6_3_26_0_zap.*` | 29 | 180 KB | Adreno zap shaders (several GPU SKUs in one build) |
| `*_sqe.fw`, `a662_gmu.bin`, `gmu_gen70000.bin` | 6 | — | GPU SQE/GMU firmware |
| `novatek_ts_fw.bin`, `novatek_ts_mp.bin` | 2 | 272 KB | **Novatek touch** (this unit's touch FW, 0x18) |
| `focaltech_8725_fw.bin`, `focaltech_ft8720_hx.ini`, `focaltech_ft8725_hx.ini`, `focaltech_ts_fw_.bin` | 4 | — | **FocalTech touch** firmware — proof that other variants of this model ship a *different touch controller* |
| `aw87xxx_acf.bin` | 1 KB | AWINIC AW87xxx **smart PA** (speaker amplifier) tuning |
| `fs1599.fsm` | 920 B | fingerprint sensor firmware blob (FocalTech FS1599 class) |
| `wlan/qca_cld/{adrastea,qca6490,qca6750}/` | empty dirs | — | qcacld looks up its `bdwlan.bin`/`regdb.bin` here at runtime; the actual files come from `NON-HLOS.bin` (`image/adrastea/`) |
