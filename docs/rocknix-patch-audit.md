# ROCKNIX SM4450 patch audit (relative to Linux 7.3)

Source: `projects/ROCKNIX/devices/SM4450/linux/patches/` from the ROCKNIX
distribution (Horcruxes/distribution @ 3fb19ba4 == ROCKNIX/distribution@next).
Their kernel base is Linux **7.2.11**; this audit checks every patch against
**Linux 7.3**.

Classification:

* **S** — required for sky (SoC level support)
* **O** — optional / useful later
* **O-** — audio long tail, postponed
* **X** — not needed for this device

## Summary

* 48 patches, **24 are S-class**; 21 of those apply to 7.3 cleanly (some with
  fuzz).
* Only **2 need a real rebase** on 7.3:
  * `0009` (UBWC table) — 7.3 collapsed the per-SoC `qcom_ubwc_cfg_data` into
    shared descriptors.
  * `0022` (rpmh regulator) — a header define moved.
* `0041` and `0042` are already upstream in 7.3 and can be dropped.
* 12 X-class patches are handheld-specific (panel/joypad/WCN3950/RGB LEDs).
* 6 `O-` patches are audio (`lpass-cdc` has no mainline driver yet).
* `0024` (geni serial RX restart) **does not compile on 7.3** — dropped from
  our tree; it is not needed for the boot path.

## Per-patch table

| # | patch | what it does | class | on 7.3 |
|---|---|---|---|---|

| 0001 | `0001-drm-msm-adreno-add-a613-catalog-entry.patch` | drm/msm/adreno: add Adreno 613 to the a6xx catalog | **S** | clean |
| 0002 | `0002-pmdomain-qcom-rpmhpd-add-lcx-for-sm4450.patch` | pmdomain: qcom: rpmhpd: add LCX to SM4450 | **S** | clean |
| 0003 | `0003-remoteproc-qcom-pas-add-sm4450-adsp-wpss.patch` | remoteproc: qcom: pas: add SM4450 ADSP and WPSS | **S** | clean |
| 0004 | `0004-regulator-qcom-rpmh-add-pm6450.patch` | regulator: qcom-rpmh: add PM6450 regulators | **S** | clean (1 fuzz) |
| 0005 | `0005-soc-qcom-pd-mapper-add-sm4450.patch` | soc: qcom: pd-mapper: add SM4450 domains | **S** | clean |
| 0006 | `0006-interconnect-qcom-add-sm4450.patch` | interconnect: qcom: add SM4450 driver | **S** | clean |
| 0008 | `0008-iommu-arm-smmu-qcom-add-sm4450.patch` | iommu/arm-smmu-qcom: add SM4450 support | **S** | clean |
| 0009 | `0009-drm-msm-add-sm4450-display.patch` | drm/msm: add SM4450 display support (DPU + MDSS + UBWC) | **S** | needs rebase (2 hunks) |
| 0010 | `0010-drm-panel-chipwealth-ch13726a-vendor-timings-rp-classic.patch` | drm/panel: chipwealth-ch13726a: per-board init table, vendor timings, Retroid Pocket Classic | **X** | clean (1 fuzz) |
| 0011 | `0011-clk-qcom-dispcc-sm4450-fix-mdp-clk-src-ops.patch` | clk: qcom: dispcc-sm4450: Fix disp_cc_mdss_mdp_clk_src ops | **S** | clean (1 fuzz) |
| 0012 | `0012-clk-qcom-dispcc-sm4450-quiesce-splash.patch` | clk: qcom: dispcc-sm4450: splash quiesce + DSI RCG parent enable | **S** | clean |
| 0013 | `0013-clk-qcom-gpucc-sm4450-add-hlos1-vote-gpu-smmu.patch` | clk: qcom: gpucc-sm4450: add hlos1_vote_gpu_smmu clock | **S** | clean |
| 0014 | `0014-clk-qcom-gpucc-sm4450-enable-gx-gdsc.patch` | clk: qcom: gpucc-sm4450: let the kernel enable the GX GDSC | **S** | clean |
| 0015 | `0015-drm-msm-a6xx-avoid-gmu-cx-reads-on-a613.patch` | drm/msm/a6xx: avoid SPTPRAC status read on GPUs without SPTPRAC | **S** | clean |
| 0016 | `0016-input-add-rsinput-gamepad.patch` | input: Add driver for RSInput Gamepad | **X** | clean |
| 0017 | `0017-clk-qcom-gpucc-sm4450-fix-gfx3d-rcg-ops.patch` | clk: qcom: gpucc-sm4450: adapt gfx3d RCG for GMU-less use | **S** | clean |
| 0018 | `0018-clk-qcom-gpucc-sm4450-retain-gx-gdsc-regs.patch` | clk: qcom: gpucc-sm4450: retain GX GDSC regs across collapse | **S** | clean (1 fuzz) |
| 0019 | `0019-drm-msm-dpu-stop-boot-scanout.patch` | drm/msm/dpu: stop a timing engine the bootloader left running | **S** | clean |
| 0022 | `0022-regulator-qcom-rpmh-add-pbs-type.patch` | regulator: qcom-rpmh: add PBS trigger resource type | **S** | needs rebase (1 hunk) |
| 0023 | `0023-leds-qcom-lpg-add-pm6450-pwm.patch` | leds: qcom-lpg: add PM6450 PWM support | **O** | clean |
| 0024 | `0024-serial-qcom-geni-restart-terminated-rx-command.patch` | serial: qcom-geni: restart RX after the read command terminates itself | **O** | clean (1 fuzz) |
| 0030 | `0030-ASoC-qcom-sc8280xp-i2s-clk-support.patch` | ASoC: qcom: sc8280xp: add MI2S clock and format support | **O-** | needs rebase（3 hunk） |
| 0031 | `0031-ASoC-codecs-aw88166-support-changing-sample-rate-and-bit-width.patch` | ASoC: codecs: aw88166: support changing sample rate and bit | **O-** | clean |
| 0032 | `0032-ASoC-codecs-aw88166-reduce-log-spam.patch` | ASoC: codecs: aw88166: reduce log spam | **O-** | clean (1 fuzz) |
| 0033 | `0033-ASoC-codecs-aw88166-remove-fade-in-out-on-start-stop.patch` | ASoC: codecs: aw88166: remove fade in/out on start/stop | **O-** | clean |
| 0034 | `0034-ASoC-codecs-aw88166-make-volume-control-usable.patch` | ASoC: codecs: aw88166: make volume control usable | **O-** | needs rebase（3 hunk） |
| 0039 | `0039-soundwire-qcom-arm-wake-detector-for-clock-stop.patch` | soundwire: qcom: use EE0 link manager so wake reaches the CPU | **O** | clean |
| 0040 | `0040-phy-qcom-qmp-combo-add-sm4450.patch` | phy: qcom: qmp-combo: add SM4450 USB3/DP configuration | **S** | clean（2 处 fuzz） |
| 0041 | `0041-phy-qcom-qmp-combo-correct-v4-dp-pre-emphasis.patch` | phy: qcom: qmp-combo: Correct pre-emphasis table for QMP v4 DP PHYs | **X** | already upstream |
| 0042 | `0042-phy-qcom-qmp-combo-prevent-pm-runtime-suspend-at-boot.patch` | phy: qcom: qmp-combo: Prevent unnecessary PM runtime suspend at boot | **X** | already upstream |
| 0043 | `0043-phy-qcom-qmp-ufs-add-sm4450.patch` | phy: qcom: qmp-ufs: add SM4450 UFS PHY | **S** | clean |
| 0045 | `0045-drivers-use-soc-serial-for-wifi-and-bluetooth.patch` | drivers: use soc serial for wifi and bluetooth | **O** | clean |
| 0046 | `0046-bluetooth-hci_qca-include-wcn3950-in-wcn-family-switches.patch` | Bluetooth: hci_qca: include WCN3950 in WCN family switches | **X** | clean |
| 0047 | `0047-bluetooth-hci_qca-drop-baudrate-vendor-event-for-wcn3950.patch` | Bluetooth: hci_qca: drop baudrate-change vendor event for WCN3950 | **X** | clean |
| 0048 | `0048-bluetooth-hci_qca-keep-ibs-disabled-for-wcn3950.patch` | Bluetooth: hci_qca: keep IBS disabled for WCN3950 | **X** | clean |
| 0049 | `0049-usb-typec-ucsi_glink-add-sm4450-quirk.patch` | usb: typec: ucsi_glink: enable UCSI_DELAY_DEVICE_PDOS quirk for SM4450 | **S** | clean |
| 0050 | `0050-drm-panel-add-focaltech-ft7131m.patch` | drm/panel: add FocalTech FT7131M for the Anbernic RG 55G1 | **X** | clean（2 处 fuzz） |
| 0051 | `0051-power-supply-qcom_battmgr-allow-setting-the-USB-input-current-limit.patch` | power: supply: qcom_battmgr: allow setting the USB input current limit | **S** | clean |
| 0052 | `0052-power-supply-qcom_battmgr-report-the-USB-adapter-type.patch` | power: supply: qcom_battmgr: report the USB adapter type | **S** | clean |
| 0053 | `0053-input-add-singleadc-joypad.patch` | input: joystick: add singleadc-joypad for the Anbernic RG 55G1 | **X** | clean |
| 0054 | `0054-power-supply-rename-qcom-battmgr-sysfs.patch` | power:supply: rename qcom battmgr sysfs | **S** | clean |
| 0055 | `0055-bato-singleadc-joypad-rgb-leds.patch` | input: singleadc-joypad: drive the joystick RGB LEDs | **X** | needs rebase（0 hunk，多半是依赖/已应用） |
| 0056 | `0056-bato-ASoC-wcd937x-fix-headphone-volume-tlv.patch` | ASoC: codecs: wcd937x: fix the HPHL/HPHR Volume dB scale | **O-** | clean |
| 0057 | `0057-bato-arm64-dts-qcom-sm4450-add-cpu-capacity-and-power-coefficients.patch` | arm64: dts: qcom: sm4450: add CPU capacity and power coefficients | **S** | clean |
| 0058 | `0058-bato-singleadc-joypad-park-leds-across-suspend.patch` | input: singleadc-joypad: park the stick LEDs across suspend | **X** | needs rebase（0 hunk，多半是依赖/已应用） |
| 0059 | `0059-input-rsinput-add-digital-triggers.patch` | input: rsinput: add digital-triggers | **X** | needs rebase（0 hunk，多半是依赖/已应用） |
| 0060 | `0060-leds-qcom-lpg-support-inversed-pwm-polarity.patch` | leds: qcom-lpg: support inversed PWM polarity | **O** | clean |
| 0061 | `0061-input-edt-ft5x06-follow-the-panel-power-state.patch` | input: edt-ft5x06: follow the panel power state | **O** | clean |


## What this means for the build

`kernel/README.md` lists the subset that is actually required for M1–M4, plus the
two rebases. Everything in the *X* and *O-* classes can be ignored until the
display, audio and Wi-Fi milestones.
