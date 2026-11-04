# Next prompt: qcacld-3.0 → mainline 移植（第 1 轮）

> **状态：已被取代（2026-11-03 14:12 之后）。**
> 本轮 q1 的实际结论是 Sony `aosp/LA.UM.7.3.r1` 不适合，主基线已改为
> MiCode `sky-t-oss`（见 `FINDINGS-qcacld-branches-github.md`）。
> 当前进度以 `STATUS-qcacld-mainline-2026-11-03.md` 为准，本文件仅作历史记录。

先读（按顺序）：

1. **`docs/wifi-handoff/HANDOFF-qcacld-mainline.md`** ← 主入口（结论/资产/计划/风险）
2. `docs/wifi-handoff/FINDINGS-session8-stock-qmi-order.md`（原厂 QMI 顺序、
   `ADRASTEA_SHADOW_REGISTERS`、icnss2 分支选择）
3. `docs/wifi-handoff/FINDINGS-session9-shadow-reg-confirmed.md` §11（12 管 CE 表 +
   12 条 shadow_reg 的实测配置，qcacld 侧必须一致）
4. `docs/wifi-handoff/FINDINGS-dt-references-wcn6750-adrastea.md`（**DT 已交叉验证，含可直接用的 icnss 节点草稿**）
5. 仓库根 `qcacld3-qca6750-sm4450-sky-report.md`（更早的 qcacld 调研）

## 本轮目标（q1）

**拿到能在 mainline 7.3 头文件下编译的 qcacld 栈源码**，并把 API 报错清单摸清楚。

1. 取源码（6.6 时代，不要用 ROM 里的 5.10 版本）：
   - `qcacld-3.0`、`qca-wifi-host-cmn`、`fw-api`（CodeLinaro `wlan` 或厂商 6.6 内核）
   - 参考已有的 `wifi-backup/stock-wlan/qcom/opensource/wlan/`（只有 host-cmn + fw-api）
2. 用 mainline 头文件试编 `qca-wifi-host-cmn/hif` 与 `core`：
   `make -C sky-mainline/kernel/linux-7.3 M=<path> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules`
3. 产出：`docs/wifi-handoff/FINDINGS-qcacld-q1-build.md`，内容：
   - 源码版本/来源/commit
   - 编译命令
   - **API 报错分类清单**（cfg80211 / skb / timer / string / sched / wiphy / nl80211 …）
   - 需要的内核导出符号缺口
   - 建议的最小改法（每类一条）

## 需要同时确认的三件事（ath11k 线已验证，qcacld 侧要对照）

1. `ADRASTEA_SHADOW_REGISTERS` 在该源码里是否默认启用；
2. `target_ce_config_wlan_adrastea` 是否 12 管（CE0–CE11，含 CE5=OUT/1024/64/DIS_INTR、
   CE7=INOUT_H2H）；
3. `icnss_send_wlan_enable_to_fw()` 里 `shadow_reg`(v1) 是否是 12/13 条
   （我们实测 12 条可用、dst CE5 会触发固件断言）。

## 设备提醒

* 设备现在跑主线 Linux，SSH `root@192.168.7.1`/`sky`；`/` 是 `/dev/sda20`(ext4)。
* 刷机：`dd if=img of=/dev/sde13 bs=4096`；fastboot 也可用。
* 急救镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`。
* 原厂 Android 可随时刷回（`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`，
  `/data` 需先 `make_f2fs`）。
* 不要再做 pre-BDF QMI 实验；实验参数走 `/etc/sky-wifi-params.try`。
