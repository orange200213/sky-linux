# Next prompt: WCN6750 adrastea SNOC 端口（session 7 之后）

先读（按顺序）：

1. `docs/wifi-handoff/FINDINGS-session7-stock-jp-baseline.md` ← **本轮新增，最重要**
2. `docs/wifi-handoff/HANDOFF-snoc-mainline.md`（§13 session6、§14 session7）
3. `docs/wifi-handoff/FINDINGS-session6-apss-aperture.md`（寄存器实测与 k92/k93）

## 当前现实（结论，不要再推翻）

* 本机是**日版**（`river_jp`）。正确原厂基线 = `sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0`
  （HyperOS 14）。欧版 MIUI 13 是错基线（super 刷不进去）。
* 原厂 DTS 的 wifi 节点与我们 port 的**完全等价**（`qcom,icnss@22800000`, 8 MiB membase）。
* 原厂跑 `qca_cld3_adrastea.ko`，它**没有 wifi3.0 HAL**，**不编程 WBM/REO/TCL**；
  主机侧只有 CE（wrapper `0x24C000`）、SI、RTC、SOC_CORE，全在 8 MiB 内。
* ⇒ mainline ath11k 的 `ath11k_ce_init_ring()` / `ath11k_dp_srng_setup()` abort 是
  **架构性**的，不是地址算错。`device_info` BAR 路线已被固件否决（94 NOT_SUPPORTED）。

## session 8 已做完的三项验证（全部否定原假设，别重复做）

1. **供电不是原因**：mainline 与 stock 的 regulator 快照对比，WLAN 相关
   `l11/l13/l14/l19/l20/l21/l23/l25` 两边都是 enabled（见 `FINDINGS-session7` §9.1）。
2. **NoC 时钟**：mainline `qxm_wlan_q6 / qhs_wcss / qxm_wcss` 三条 interconnect 都是 0 投票，
   clk_summary 里没有 wcss 时钟条目（是否影响未验证）。
3. **原厂顺序发 WLAN_INI/CFG/MODE（k94，`snoc_post_bdf`）**：
   `WLAN_INI` 成功、`WLAN_CFG`/`WLAN_MODE` 返回 QMI **90 INCOMPATIBLE_STATE**；
   而且**只发 ini 后寄存器窗口依然全死**（`0x22800000..0x22820000` 0 个可读页）。

⇒ 唯一推荐方向：**拿原厂完整 QMI 序列**（回原厂系统开 icnss2 调试日志，或反汇编
`jp_icnss2.ko` 的 `icnss_wlan_enable`），确认"让固件进入可访问状态"的确切前置条件。
详见 `FINDINGS-session7-stock-jp-baseline.md` §9.5。

## 早前的备选实验（已被上面取代）

1. **regulator/clk 差异**：刷回 k91/k93，生成与
   `backup/stock-jp-2026-11-02/stock_regulators.txt` 同格式的快照并 diff。
   重点看 icnss2 消费的 8 个 rail（`rpmh-regulator-ldob11/14/23/ldoe7`、
   `regulator.18/21/31/44`）在 mainline 是否 enabled。
2. 查 `ath11k_ahb_power_up()`（wcn6750/SNOC）是否缺 `regulator_bulk_enable()`
   与 `cxo_ref_clk_pin` 时钟；补上后再读 `0x22824C000` 验证是否变活。
3. 若 CE wrapper 变活：只做 CE + HTC/WMI/HTT（跳过 DP srng 编程）的最小验证。
4. 若 1–3 都不行：明确记录"mainline ath11k 路线在该 SoC 上不可行"的结论，
   转回 downstream（qcacld-3.0 adrastea，见仓库根 `qcacld3-qca6750-sm4450-sky-report.md`）。

## 设备操作约定

* SSH `root@192.168.7.1` / `sky`（mainline 时）；stock Android 时用 `adb shell su -c`（Magisk）。
* 需要 fastboot 前先问用户。**不要**再发 pre-BDF WLAN_MODE（`snoc_pre_bdf=5` 会整机挂死）。
* 安全参数：`bdf_total_full=y / snoc_reg_mode=4 / snoc_membase_mib=0 / snoc_probe=n /
  snoc_shadow_probe=y / snoc_device_info=n`
* 救命镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`
* 设备全量备份：`backup/pre-stock-2026-11-02/`（boot_a/rootfs/misc，md5 已校验）

## 可用资产

```
wifi-backup/out/sky-shim-boot-v4-wifi-k61..k93-*.img      内核迭代镜像（k93 = device_info 版）
wifi-backup/regtool/{skyreg.ko,skyprobe,*.c,Makefile}      免刷机寄存器探针（mmap + SIGBUS 安全）
wifi-backup/stock-modules/{qca_cld3_qca6750,qca_cld3_adrastea,icnss2}.ko  欧版 ROM 提取
backup/stock-jp-2026-11-02/{stock_jp_dmesg.txt,jp_qca_cld3_adrastea.ko,jp_icnss2.ko,stock_regulators.txt}
sky-linux/kernel/patches-m8/000{1,2,3}-*.patch            内核补丁基线（0003 含 snoc 模式）
```
