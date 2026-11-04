# Next prompt: WCN6750 adrastea SNOC 端口（session 8 之后）

先读（按顺序）：

1. `docs/wifi-handoff/FINDINGS-session8-stock-qmi-order.md` ← **最新、最关键**
2. `docs/wifi-handoff/FINDINGS-session7-stock-jp-baseline.md`（原厂基线 + §9/§10 实测否定项）
3. `docs/wifi-handoff/HANDOFF-snoc-mainline.md` §13/§14/§15
4. `docs/wifi-handoff/FINDINGS-session6-apss-aperture.md`（寄存器实测方法）

## 已经钉死的事实（不要再重新推导）

* 本机是**日版**（river_jp）。正确原厂基线 = `sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`
  （HyperOS 14）。欧版 MIUI 13 是错基线。
* 原厂跑 `qca_cld3_adrastea.ko`（ADRASTEA_DEVICE_ID=0xabcd 分支）：**不发 HOST_CAP / DEVICE_INFO**，
  没有 wifi3.0 HAL，**不编程 WBM/REO/TCL**；寄存器只用 CE/SI/RTC/SOC_CORE。
* **`WLAN_INI/CFG/MODE` 必须在 `WLFW_FW_READY_IND` 之后发**；提前发 → QMI 90。
* **首次 CE MMIO 在 `WLAN_MODE` ACK 之后**；APSS→WCSS 访问门由**固件**在 WLAN_MODE 时打开。
* **原厂 build 启用 `ADRASTEA_SHADOW_REGISTERS`**：CE write-index 写 SR 影子槽
  `SHADOW_VALUE[n]`（0x32000+4n，我们在 mainline 上实测这块**可读写**），
  且 `WLAN_CFG` 必须携带 `shadow_reg`(v1) 的 24 槽表（{ce_id, offset}，见 §4）。

## 下一步（k95）

1. 关掉全部实验开关：`snoc_host_cap=0`、`snoc_device_info=n`、`snoc_pre_bdf=0`、`snoc_post_bdf=0`。
2. `ath11k_qmi_wlanfw_wlan_cfg_send()`：把 `shadow_reg_valid=1`、`shadow_reg_len=12`，
   内容为 CE 源 0/3/4/5/7（off 0x3C）+ 目的 1/2/5/7/8/9/10/11（off 0x40）→ 槽 0,3,4,5,7,13,14,17,19,20,21,22,23。
   （现在代码里是 `shadow_reg_valid = 0` 且 `core.c` 用的是 `shadow_reg_v2`——WCN6750 变体，必须换掉。）
3. `ath11k_qmi_firmware_start()` 返回非 0 **立即停止**，不要继续 CE/DP。
4. 起来后：`skyprobe rd 0x228300e0`（ADDRESS_VALID）——**非零 = 机制证实**；
   再读 `0x22832000+4n`（SHADOW_VALUE）与 `0x22830020+4n`（SHADOW_ADDRESS，只读）对照。
5. 只有 CE 打通后才谈 DP：mainline 的 `ath11k_dp_alloc()/ath11k_dp_srng_setup()` 在这颗芯片上
   需要**禁用或彻底改造**（原厂根本不编程 DP ring，数据面由固件自管）。

## 设备与工具

* 设备：主线 Linux 时 SSH `root@192.168.7.1`/`sky`；原厂 Android 时 `adb shell su -c`（Magisk）。
  需要 fastboot 前先问用户。
* 现状：设备当前跑的是**原厂 HyperOS**（用户已刷回 + Magisk root），
  主线 Linux 的 `boot_a`/rootfs 备份在 `backup/pre-stock-2026-11-02/`（含 TWRP 恢复配方）。
* 探针：`wifi-backup/regtool/{skyreg.ko,skyprobe}`（mmap + SIGBUS 安全，免刷机）。
  注意 **`copy_from_kernel_nofault()` 在 arm64 兜不住 external abort**，内核态探针会 oops。
* 一次性实验参数：`/etc/sky-wifi-params.try`（开机消费一次即删，防止挂死复现）。
* 原厂取证：`backup/stock-jp-2026-11-02/`（dmesg、两个 .ko、logcat、regulator 快照）。
* Magisk 里还留着两个脚本（可删）：`/data/adb/post-fs-data.d/qmi-trace.sh`、
  `/data/adb/service.d/qmi-dump.sh`（kprobe 抓 QMI，因 Magisk 启动时机晚于握手而未生效）。
