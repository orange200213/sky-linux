# Session 7: 日版 HyperOS 基线（原厂运行态证据）

日期：2026-11-02
设备：Xiaomi Redmi 12 5G（sky），日版机器（`ro.product.name = river_jp`）
原厂镜像：`/home/orange/sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0/`
（OS1.0.13.0.UMWJPXM_14.0，HyperOS 14，可完整刷入；此前用的
`sky_in_global_images_V14.0.7.0.TMWINXM_13.0`（欧版 MIUI 13）刷 super 分区会中断，
是**错的基线**）
原厂内核：`5.10.198-android12-9-00016-g26d1d5093064-ab11282459 #1`
取证环境：原厂系统 + Magisk root（`adb shell su -c`），WLAN 正常工作（`wlan0` 存在）。

## 1. 结论速览

| 问题 | 答案 |
|---|---|
| 原厂 DTS 的 wifi 节点变了吗？ | **没有变化**，与欧版逐字段一致 |
| 原厂跑哪个驱动？ | **`qca_cld3_adrastea.ko`**（不是 `qca6750.ko`） |
| 它是 wifi3.0 HAL 那套吗？ | **不是**：二进制里没有 `hal_attach` / `hal_srng_setup` / `hw_srng_table` / `TARGET_TYPE` |
| 它的寄存器表在哪？ | CE wrapper `0x24C000`、SI `0x84000`、RTC `0x45000`、SOC_CORE `0x113000`——**全在 8 MiB 内，没有 0xa00000 的 UMAC** |
| 主机编程 DP ring 吗？ | **不编程**（无 WBM/REO/TCL 访问）→ DP 由固件自己管 |
| 有几个寄存器区？ | 只有 `0x22800000`（8 MiB+1 页）和 MSA `0x82a00000`（3 MiB+1 页），**没有第二个 BAR** |

⇒ 这一条解释了我们之前所有 abort：**mainline ath11k 的 `ath11k_dp_srng_setup()` /
`ath11k_ce_init_ring()` 在本硬件上踩的是"主机根本不该访问"的寄存器**。地址公式没错，
是**架构不同**：adrastea/SNOC 属于"CE + 固件自管数据面"的设计。

## 2. DTS 对比（欧版 MIUI 13 vs 日版 HyperOS 14）

两版 `dtbo.img` 里 `qcom,icnss@22800000` 节点完全一致：

```
compatible = "qcom,icnss";
reg = <0x22800000 0x800000>;
reg-names = "membase";
interrupts = <... 12 条 CE 中断 ...>;
qcom,iommu-dma = "fastmap"; qcom,iommu-faults = "...";
qcom,wlan; bdf-download-support; wpss-support-enable;
qcom,wlan-msa-fixed-region = <...>;
```
`vendor_boot.img` 里那个 `qcom,wcn6750`（`reg-names = "msi_addr\0smmu_iova_ipa"`，
32 条 MSI）在两版里都是 `status = "disabled"`。
⇒ 我们 port 的 DTB 方向（`qcom,wcn6750-wifi` + `qcom,snoc-ahb` + 8 MiB membase）在
**地址/中断上与硬件一致**，不需要按日版重做 DTS。

## 3. 原厂运行态（dmesg，`backup/stock-jp-2026-11-02/stock_jp_dmesg.txt`，9984 行）

```
[ 2.632] icnss2 22800000.qcom,icnss: Adding to iommu group 9
[ 2.641] icnss2: Platform driver probed successfully
[ 3.477] modprobe: Loading qca_cld3_qca6750.ko      ; 失败（Invalid argument），见下
[ 3.484] adrastea: Loading driver v2.0.9.17N +PANIC_ON_BUG; cld:; cmn:; dev:adrastea
[ 3.500] modprobe: Loaded qca_cld3_adrastea.ko      ; ✔ 真正在跑的
[ 3.523] wlan_pld:pld_register_driver:347:: Fail to register pcie driver
[ 3.535] wlan_pld:pld_register_driver:389:: Fail to register ipci driver
[ 7.206] icnss2_qmi: QMI Server Connected: state: 0x480
[ 7.331] firmware load: adrastea/regdb.bin
[ 7.362] firmware load: adrastea/bdwlant_jp.bin     ; ← JP 专用板文件
[ 7.390] icnss2_qmi: Sending cal report request, state: 0x483
[10.320] icnss2: WLAN FW is ready: 0x200487
[10.452] wlan_pld:pld_get_user_msi_assignment:2241:: Not supported on type 1
[10.633] firmware load: wlan/qca_cld/adrastea/wlan_mac.bin
→ wlan0 / p2p0 出现，WiFi 可用
```

`/proc/modules`：`icnss2 204800 1 adrastea, Live`（icnss2 被 `adrastea` 使用）。
`pld_get_user_msi_assignment ... type 1` ⇒ PLD 总线类型 = SNOC(1)，与我们的判断一致。

## 4. 原厂 ioremap（`/proc/vmallocinfo`）

```
8392704 icnss_probe [icnss2] phys=0x0000000022800000 ioremap   ; 8 MiB + 4 KiB
3149824 icnss_probe [icnss2] phys=0x0000000082a00000 ioremap   ; MSA 3 MiB + 4 KiB
```
没有第二块寄存器区、没有 device_info BAR（我们 k93 实测该固件回 `94 NOT_SUPPORTED`）。

## 5. 原厂驱动的真实寄存器表（从正在运行的 .ko 的 .data 里读出来的）

`jp_qca_cld3_adrastea.ko`（= `/vendor/lib/modules/qca_cld3_adrastea.ko`，13,901,632 B）
`adrastea_ce_targetdef` @ .data+0x48ab8：

```
+0x3c = 0x24C000      ← CE wrapper / host IE 基址
+0xc4 = 0x20000       ← CE 区大小
+0x00/0x04/0x18/0x1c = 0x40/0x4C/0x44/0x48   ← 各 CE 寄存器内偏移
```

`adrastea_targetdef` @ .data+0x485e0：

```
+0x18 = 0x113028   +0x20 = 0x113000   （SOC_CORE）
+0x2c = 0x45000                       （WIFI_RTC）
+0x3c = 0x85050    +0x40 = 0x85054    （GPIO/ATHR_WLAN）
+0x80..0x94 = 0x84000/0x84008/0x8400C/0x84010/0x84014/0x84004   （SI）
```

⇒ 与原厂头文件 `adrastea_reg_def.h` 完全吻合（CE `0x24C000`、SI `0x84000`、
RTC `0x45000`、SOC_CORE `0x113000`），**没有任何 0xa00000 的 UMAC 项**。
配合"无 `hal_attach`/`hal_srng_setup`/`hw_srng_table`"，可以确认：
**adrastea 主机侧不编程 WBM/REO/TCL ring**。

## 6. 仍未解释的一点（下轮核心）

我们的 mainline 内核在 `0x22800000` 这 8 MiB 里只有 4 页可读
（`0x22820000/0x22822000/0x22830000/0x22832000`，且后两者与 SR_APSS 互为别名），
连原厂 CE wrapper `0x22824C000` 都是 DEAD；但原厂在同一硬件上用这块地址工作。
⇒ **存在"环境使能"差异**（供电 / 时钟 / SoC remap），不是地址问题。

已存档的原厂对照数据：`backup/stock-jp-2026-11-02/stock_regulators.txt`
（60 个 regulator 的名称/state/num_users，其中 16 个 enabled）。
原厂 icnss2 消费的 rail（sysfs supplier 链接）：
`rpmh-regulator-ldob11 / ldob14 / ldob23 / ldoe7` + `regulator.18 / 21 / 31 / 44`。

下一步（明确、成本低）：
1. 把设备刷回 k91/k93，跑同一份 regulator 快照，与 stock 的逐条 diff；
   凡是 stock `enabled` 而 mainline `disabled` 的 rail 就是嫌疑人。
2. 检查 mainline `ath11k_ahb_power_up()` 对 wcn6750 到底有没有
   `regulator_bulk_enable()` + `clk`（`cxo_ref_clk_pin`）——很可能整段缺失。
3. 补上之后，先只读 `0x22824C000`（CE wrapper）验证是否变活，再谈 CE/DP。
4. 架构层面：即使寄存器变活，mainline 的 DP ring 编程在这颗芯片上也无意义
   （固件自管数据面）。要评估"CE+HTC/WMI/HTT-only"的最小改造，
   或者干脆承认这条 mainline 路线在此 SoC 上不可行（见 §7）。

## 7. 路线评估（诚实结论）

* mainline ath11k 的 AHB 数据面假设（主机可编程 UMAC/DP ring）在 adrastea/SNOC
  上**不成立**；原厂靠 adrastea 专用驱动（无 HAL、CE-only）绕过。
* 因此"把 ath11k 跑通"需要的不只是找对一个基址，而是**替换数据面模型**，
  工作量远超移植某个 SoC 支持。若 §6 的供电/时钟补齐后 CE 能活，
  仍有理论路径（只用 CE 跑 HTC/WMI/HTT），但这是一次架构级改造。

## 8. 本轮产物

```
backup/stock-jp-2026-11-02/
  stock_jp_dmesg.txt            原厂完整 dmesg（9984 行）
  jp_qca_cld3_adrastea.ko       正在运行的 WLAN 驱动（含符号表）
  jp_icnss2.ko                  正在运行的 icnss2
  stock_regulators.txt          原厂 60 个 regulator 的 state/users 快照
backup/pre-stock-2026-11-02/    恢复原厂前的备份（boot_a / rootfs / misc，已校验）
/home/orange/sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0/   原厂日版 ROM（DTS 已比对）
```

---

## 9. Session 8（同日继续）：按 §6 计划做的三项验证，全部否定了原假设

前提：设备已恢复成 k93/k94 主线系统（`/data` 已格式化为 ext4 并还原 rootfs，
见 `backup/pre-stock-2026-11-02/README.md`）。

### 9.1 供电对照：mainline 与 stock 基本一致 → 不是缺供电

mainline `/sys/class/regulator/*`（38 条，11 条 enabled）与 stock 快照对照：

```
mainline enabled: vreg_l11b_0p348 vreg_l13b_1p2 vreg_l14b_1p15 vreg_l16b_1p2
                  vreg_l19b_1p8 vreg_l20b_1p7 vreg_l21b_1p8 vreg_l23b_1p8
                  vreg_l25b_3p072 vreg_l5b_0p88 vreg_l7e_3
stock  enabled:   pm6450_l11 l13 l14 l19 l20 l21 l23 l25 l4 s5_level, pm8010_l3/4/5/7, ...
```
`vreg_lXXb_*` ↔ `pm6450_lXX`，WLAN 相关的 l11/l13/l14/l19/l20/l21/l23/l25 两边都是 enabled。
⇒ **原假设"mainline 少开供电"不成立**（快照：`backup/stock-jp-2026-11-02/{stock,mainline}_regulators.txt`）。

### 9.2 时钟 / NoC：mainline 侧 WCSS 路径存在但 0 投票

```
qxm_wlan_q6@19100000.interconnect   0 0
qhs_wcss@1510000.interconnect       0 0
qxm_wcss@1700000.interconnect       0 0
```
`/sys/kernel/debug/clk/clk_summary` 里没有 wcss/wlan 时钟条目（mainline gcc-sm4450 未建模）。
是否有影响未验证（stock 已不在，无法对照）。

### 9.3 k94 实验：按原厂顺序（BDF/cal report **之后**）发 WLAN_INI/CFG/MODE

新增 `snoc_post_bdf=1|2|3|4|5`（ini/cfg/mode/ini+cfg/全部）+ `snoc_stop_after_post_bdf`
（发完即停，让固件停在"已使能"状态）。镜像：
`wifi-backup/out/sky-shim-boot-v4-wifi-k94-snoc-post-bdf.img`（内核 #93）。

实测（`snoc_post_bdf=5`）：

```text
cal report sent
snoc post-bdf ini:  7          ← WLAN_INI 成功（无错误日志；7 是 qmi_txn_wait 的返回值）
wlan config request failed: 1 90   ← WLAN_CFG  → QMI 90 = INCOMPATIBLE_STATE
wlan mode   request failed (mode: 0): 1 90   ← WLAN_MODE → QMI 90
snoc: halted after post-bdf, firmware left enabled
```

⇒ cal report 之后固件**接受 WLAN_INI、但拒绝 WLAN_CFG/WLAN_MODE（状态不兼容）**。
也就是说原厂在 ini 与 cfg 之间还有别的握手，或者要等固件自己发 `FW_READY`。

### 9.4 决定性否定：只发 WLAN_INI、固件停在使能态时，寄存器窗口依然全死

`snoc_post_bdf=1` + 停止后立刻用 `skyprobe` 扫描：

```text
0x22830008 = 0x8            ← SR_APSS 活着（参考点）
0x22800000 / 0x2280C000 / 0x228084000 / 0x228113000 / 0x22824C000 /
0x228240000 / 0x22824000 / 0x228B4874 / 0x23234874   → 全部 DEAD
scan 0x22800000..0x22820000 @4K : 0 live
scan 0x22820000..0x22850000 @4K : 只有 0x22820000 0x22822000 0x22830000 0x22832000
```

⇒ 固件停在 WLAN_INI 后并没有授予 APSS 对 A_WCSS_HM 区的访问。
结合 §5（stock 驱动只用 CE/SI/RTC/SOC_CORE，且无 HAL），最可能的门仍是
`SR_APSS.SR_CONTROL[2] ENABLE_APSS_FULL_ACCESS`（APSS 侧只读，实测写不进），
它应当由固件在完成自身 UMAC 初始化后置位——而 mainline 在到达那一步之前就 abort 了。

### 9.5 下一步（唯一推荐方向）：拿到原厂完整 QMI 序列

现在缺的是"stock 到底按什么顺序发 QMI 才让固件进入可访问状态"。两条路：

1. **回到原厂系统开 icnss2 调试日志**（最直接）：刷回原厂 `boot.img`
   （`sky_jp_global_images_.../images/boot.img`）+ 把 `/data` 格式化为 **f2fs**，
   进系统后调高 icnss2/qcacld 的打印级别（`icnss_pr_dbg`，或
   `/sys/module/icnss2/parameters/`），抓 `QMI Server Connected` →
   `WLAN FW is ready` 之间的全部消息。
2. 反汇编 `backup/stock-jp-2026-11-02/jp_icnss2.ko` 的 `icnss_wlan_enable`，
   看 ini/cfg/mode 的调用顺序与前置条件。

### 9.6 新增工具/机制（本轮）

* `snoc_post_bdf` / `snoc_stop_after_post_bdf`（k94，见上）
* **一次性实验参数机制**：`/etc/sky-wifi-params.try` 存在时，`sky-wifi-up.sh`
  先把它拷成 `/etc/sky-wifi-params` 再删除该文件 → 挂死也只发生一次。
  （注意：拷过去后参数仍是实验值，挂死恢复后要手动改回安全值。）

---

## 10. Session 8 续：抓原厂 QMI 时序的尝试记录（未成功，但把方法立起来了）

目标：拿到 stock 在 cal report → `WLAN FW is ready` 之间的**完整 WLFW QMI 消息序列**，
弄清 `WLAN_CFG`/`WLAN_MODE` 为什么在我们这边回 90。

### 10.1 可用的手段（已验证）

stock 内核（5.10.198）打开了 kprobes + tracefs：

```
/sys/kernel/tracing  (tracefs, gid=3012)
echo 'p:qmisend qmi_send_request msgid=%x3' >> kprobe_events   # 第 4 个参数 = QMI msg id
echo 'r:qmiwait qmi_txn_wait ret=$retval'   >> kprobe_events   # 看响应结果
```
两条探针都能注册成功。`/proc/kallsyms` 可读，`qmi_send_request` 可探。

### 10.2 为什么没抓到

| 尝试 | 结果 |
|---|---|
| 手动 arm 探针后 `svc wifi disable/enable` | **0 条事件** —— 关闭/打开 wifi 不会重做 WLFW 握手（固件保持已加载） |
| arm 之后 `rmmod adrastea; rmmod icnss2; insmod …` | 模块能成功重载（rc=0），但**驱动不会 open**：`wlan0` 不再出现，即 `pld_wlan_enable()` 由用户态（wifi HAL/wpa_supplicant）触发，不由 insmod 触发 |
| Magisk `post-fs-data.d` 开机自动 arm + `service.d` 45s 后 dump | 探针注册成功（重启后仍在），但 **trace 为空**：Magisk 的 post-fs-data 阶段在 /data 解密后才跑，**晚于** 2.6s 的 icnss2 probe 和 6.8s 的 QMI 握手 |

### 10.3 logcat 里的意外收获：原厂有两个 WLFW 客户端

```
cnss-daemon: wlfw_start: Starting
cnss-daemon: WLFW service connected
cnss-daemon: wlfw_build_cal_table: not read /data/vendor/wifi/wlfw_cal_00..04.bin
cnss-daemon: wlfw_send_cap_req: chip_id 0x4130, chip_family 0x4007, board_id 0xff,
              soc_id 0x406a0000, fw_version 0x3431fb72, fw_build_timestamp 2024-09-03
cnss-daemon: wlfw_send_bdf_download_req: BDF file : regdb.bin  → Failed to find
cnss-daemon: wlfw_send_bdf_download_req: BDF file : bdwlan.bin → Failed to find
cnss-daemon: Failed to Download the BDF File
```
即：**内核 icnss2** 与**用户态 cnss-daemon** 都会向 WLFW 发 QMI（daemon 发 cap_req + BDF，
且它的 BDF 是失败的；成功的那次在 dmesg 里，用 `adrastea/bdwlant_jp.bin`）。
这说明"谁在什么时刻把固件推进到可访问状态"可能涉及用户态 daemon 的参与，
排查时不能只看内核路径。

存档：`backup/stock-jp-2026-11-02/stock_jp_logcat_wlfw.txt`（56 行）

### 10.4 下次继续抓的正确做法

1. `qca_cld3_adrastea.ko` 的日志会进 **logcat**（不只 dmesg）。先用
   `/vendor/etc/wifi/adrastea/WCNSS_qcom_cfg.ini` 里的日志级别项（如
   `qdf_debug_mask` / `enableFwLogging`）把级别拉高，重启后 `logcat -d -b all` 抓全量；
2. 或者：手动 `rmmod adrastea/icnss2` → `insmod` → **再 `stop`/`start` 供应商 wifi HAL
   （或 `wpa_supplicant`）**，在 kprobes 已 arm 的情况下触发 open，从而抓到完整握手；
3. 子代理正在做的事（反汇编 `jp_icnss2.ko` 的 `icnss_wlan_enable` + 取同版本源码
   `drivers/soc/qcom/icnss2/{main,qmi}.c`）如果先给出答案，就不必再抓了。
