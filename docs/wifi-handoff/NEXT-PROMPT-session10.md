# Next prompt: WCN6750 adrastea SNOC（session 9 之后）

按顺序读：

1. `docs/wifi-handoff/FINDINGS-session9-shadow-reg-confirmed.md` ← **最重要，§11–13**
2. `docs/wifi-handoff/FINDINGS-session8-stock-qmi-order.md`（原厂 QMI 顺序/影子寄存器机制）
3. `docs/wifi-handoff/HANDOFF-snoc-mainline.md` §16
4. `docs/wifi-handoff/FINDINGS-session7-stock-jp-baseline.md`（日版基线、固件来源）

## 已经打通的事实（别再重复验证）

* 设备是日版（river_jp）；原厂基线 HyperOS 14（`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`）。
* WPSS/WLAN 固件已换成原厂 2024 版（`fw_version 0x3431fb72`），来自 `NON-HLOS.bin`
  的 `image/adrastea/wpss.*`；旧版在设备 `/root/wpss-old/`。
* **CE 配置必须用原厂 adrastea 12 管表**（`ce.c`/`hw.c` 已有
  `ath11k_host_ce_config_adrastea` / `ath11k_target_ce_config_wlan_adrastea`，
  wcn6750 hw_params 已指向它们）。用 qca6390 的 9 管表 → 固件
  `platform_helium_init.c:platform_config_shadow_reg:241` 断言。
* `WLAN_CFG` 带 `shadow_reg`(v1) 12 条表（`snoc_shadow_reg_v1=y`、`mode=0`）→
  `ADDRESS_VALID=0xf860b9`（12 槽）。写 `SHADOW_VALUE[n]` 会被转发（DIRTY 消费、
  `INVALID_ADDR_ACCESS=0`）。
* 仍 abort 的位置：`ath11k_dp_srng_setup()`（DP ring，架构性不可达）。

## 下一步（k99）

1. **CE 写指针走影子通道**：在 `ahb.c` 加 `snoc_reg_mode=6`，把命中 CE 写指针的写
   （`CE0_SRC + n*0x1000 + 0x3C` / `CE0_DST + n*0x1000 + 0x40`）改写成
   `membase + 0x32000 + 4*slot`（slot 映射见 FINDINGS §11.2），其它 CE 寄存器先 warn 跳过；
   记得把 `{5,0x40}`（dst CE5→slot17）补进表，凑齐原厂 13 项。
2. 观察 CE 初始化 + HTC 是否能有进展（判断 ring base/size 是否由固件预配置）。
3. DP：`ath11k_dp_srng_setup()` 需禁用或按"固件自管数据面"改造 —— 这是路线可行性的判断点。

## 设备与工具

* 主线 Linux：SSH `root@192.168.7.1`/`sky`；原厂 Android：`adb shell su -c`（Magisk）。
  固件/内核改动都可用 `dd` 直接写 `/dev/sde13`（boot_a），无需 fastboot。
* `/data` = ext4（105.7G），rootfs 在 `/data/skyroot`（切换后即 `/`）；
  备份 `backup/pre-stock-2026-11-02/`（含 `sky-wifi-up.sh.oneshot` 一次性参数机制补丁）。
* 探针：`wifi-backup/regtool/{skyreg.ko,skyprobe}`（mmap + SIGBUS 安全）。
* 一次性参数：`/etc/sky-wifi-params.try`（已被恢复进 rootfs 的脚本支持）。
* 可用镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k9[1-8]-*.img`，
  救命镜像 `...-nofdt-recover.img`。
