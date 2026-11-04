# HANDOFF：WCN6750/adrastea —— 转向 qcacld-3.0 移植到 mainline

日期：2026-11-02（session 9 结束）
状态：**ath11k 移植线正式关闭**，转入 qcacld-3.0 → mainline 路线。

> **进度入口（2026-11-03 更新）**：日常进度看
> `STATUS-qcacld-mainline-2026-11-03.md`；当前任务看
> `HANDOFF-cfg80211-api-linux73.md`。本文件继续作为路线结论、资产和 q1–q5 计划的总纲。
> 注意 §4 的 q1 建议（取 Sony 6.6 系源码）已被当天实践推翻：主基线最终定为
> MiCode `sky-t-oss`（qcacld/host-cmn/fw-api 同批提交）。

---

## 0. 一页结论

设备（Xiaomi Redmi 12 5G / `sky` / SM4450 ravelin + WCN6750 adrastea）的 WLAN
在 APSS 侧**只暴露 SR_APSS 块 + 12 个 CE 写指针影子槽**，其余寄存器（CE 环 base/size、
UMAC/WBM/REO/TCL）在 8 MiB `membase` 窗口里**全部不可访问**（硬件实测）。
因此 mainline ath11k「主机编程 CE/DP 环」的模型在此平台不可能工作；
而「固件自建环、主机只踢指针」也被实测否定（固件不主动发 HTC ready）。
⇒ 唯一已知可行实现是 Qualcomm 自家 **qcacld-3.0 `adrastea` profile**（原厂量产验证），
下一步就是把它搬到 mainline。

---

## 1. ath11k 线为什么关闭（证据索引，别再回头重试）

全部细节在 `FINDINGS-session9-shadow-reg-confirmed.md`（791 行，§1–20）。

| 结论 | 证据 |
|---|---|
| 8 MiB `membase` 窗口里只有 `0x20000..0x52000`（SR_APSS）可访问 | skyprobe 全窗口扫描，123 探针 0 live（§19.2） |
| `membase+0x240000`（CE0）、`+0x24C000`（CE wrapper）等全 DEAD | 逐点直读（§19.2） |
| 影子槽机制可用：`WLAN_CFG.shadow_reg`(v1) 12 条 → `ADDRESS_VALID=0xf860b9` | §11.2（k98） |
| 影子写入会被转发到 CE 寄存器 | 写 `SHADOW_VALUE0` → DIRTY 消费、`INVALID_ADDR_ACCESS=0`（§12） |
| 影子表约束 | 表必须 12 条、offset 必须与 CE 方向一致、dst CE5 不可映射（§10/§14） |
| **CE 配置必须是原厂 adrastea 12 管表**（qca6390 的 9 管会触发固件断言） | §11.1（k98） |
| 零 MMIO 时 bring-up 能跑到 HTC 然后超时 | §18（k102） |
| mode 9 实测：主机**从未踢过** CE 写指针，固件也不发 HTC ready | §20（k104） |

附带成果（对新路线也有用）：
* 固件已换原厂 2024 版（`fw_version 0x3431fb72`，来源 `NON-HLOS.bin` 的
  `image/adrastea/wpss.*`），旧版备份在设备 `/root/wpss-old/`。
* 原厂 QMI 顺序、icnss2 分支选择、`ADRASTEA_SHADOW_REGISTERS` 等结论见
  `FINDINGS-session8-stock-qmi-order.md`。
* 实验开关现成可用：`snoc_reg_mode` 0–9、`snoc_shadow_reg_*`、`snoc_probe`、
  `snoc_device_info`、`bdf_total_full`、`snoc_membase_mib`；
  一次性参数机制 `/etc/sky-wifi-params.try`。

---

## 2. 手头资产清单

### 2.1 源码

| 路径 | 内容 |
|---|---|
| `wifi-backup/stock-wlan/`（198 MB，git clone） | `qcom/opensource/wlan/qca-wifi-host-cmn`（HTC/CE/HIF/PLD 层）+ `fw-api`。**只有 host-cmn，没有 qcacld-3.0 本体** |
| `sky-mainline/kernel/linux-7.3/` | 我们在用的 mainline 树（ath11k 改动都在里面，可作为 QMI/remoteproc 参考实现） |
| 参考（需重新拉取） | `github.com/sky-lineage/android_kernel_xiaomi_sky`（分支 `lineage-23.2`）里的 `drivers/soc/qcom/icnss2/` 与设备树；qcacld-3.0 本体需从 CodeLinaro / 厂商仓库取 |

### 2.2 二进制（原厂，未 strip，可反汇编）

| 文件 | 大小 | 说明 |
|---|---|---|
| `backup/stock-jp-2026-11-02/jp_qca_cld3_adrastea.ko` | 13,901,632 | 原厂 **qcacld-3.0 adrastea profile** 模块（5.10 GKI 编译） |
| `backup/stock-jp-2026-11-02/jp_icnss2.ko` | 429,320 | PLD 平台胶水（SNOC + QMI + remoteproc） |
| `wifi-backup/stock-modules/qca_cld3_qca6750.ko` 等 | — | 欧版 ROM 的同族模块（对照用） |

### 2.3 文档

```
docs/wifi-handoff/
  HANDOFF-qcacld-mainline.md            ← 本文件（新入口）
  FINDINGS-session9-shadow-reg-confirmed.md   （§1–20，ath11k 全部实测）
  FINDINGS-session8-stock-qmi-order.md     （原厂 QMI 顺序 / icnss2 分支 / 影子寄存器）
  FINDINGS-session7-stock-jp-baseline.md   （日版基线、固件、regulator 对照）
  FINDINGS-session6-apss-aperture.md       （寄存器探针方法与早期孔径测量）
  HANDOFF-snoc-mainline.md                 （ath11k 线历史，§16 摘要）
  NEXT-PROMPT-session10.md                 （ath11k 线遗留，仅作参考）
```
另：仓库根 `qcacld3-qca6750-sm4450-sky-report.md`（更早的 qcacld 调研）。

### 2.4 设备与运维

* 主线 Linux：SSH `root@192.168.7.1` / 密码 `sky`；`/` = `/dev/sda20`（ext4，105.7G），
  rootfs 在 `/data/skyroot`（切换后即 `/`）。
* 原厂 Android：`adb shell su -c`（Magisk root），当前**未运行**（跑的是主线 Linux）。
* 刷机：`dd if=img of=/dev/sde13 bs=4096`（boot_a）——无需 fastboot；
  fastboot 也可用（`fastboot flash boot`）。
* 急救镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`（DTB 关 WiFi，
  实验参数导致 panic 循环时用它救）。
* rootfs 备份：`backup/pre-stock-2026-11-02/rootfs-skyroot-2026-11-02.tar.gz`
  + `sky-wifi-up.sh.oneshot`（恢复后覆盖 `/usr/local/sbin/sky-wifi-up.sh`）
  + README 里的 TWRP 恢复配方。
* 探针工具：`wifi-backup/regtool/{skyreg.ko,skyprobe}`（mmap 任意物理地址 +
  SIGBUS 安全读写/扫描）。
* 可启动镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k{91..104}-*.img`（内核 #90–#103）。

---

## 3. 新路线的技术形态

### 3.1 目标架构（原厂那套，原样搬到 mainline）

```
用户态 (wpa_supplicant / hostapd)
   ↓ cfg80211 / nl80211
qcacld-3.0  (HDD → WMA/UMAC components → WMI → HTC → CE/HIF)
   ↓ PLD 抽象层（pld_* ops）
icnss2（平台胶水：SNOC 总线、QMI(WLFW)、remoteproc 启动、regulator/clock、PDR/SSR）
   ↓
mainline 内核服务：remoteproc / qcom_q6v5_pas、rpmh-regulator、interconnect、
                   iommu、qrtr、qmi_helpers、pdr/subsys notifier
```

### 3.2 三个必须先解决的技术点

1. **内核 API 代差**：原厂模块是 **5.10 GKI**，目标 mainline 是 **7.3**。
   `cfg80211`/`nl80211`、`skb`、定时器、`strlcpy`、`sched`、`wiphy` 等 API 都变了。
   ⇒ **强烈建议不要用 ROM 里那份 5.10 源码**，改用 **6.6 时代的 qcacld-3.0**
   （CodeLinaro `wlan-host-6.6` / 厂商 6.6 内核里的 `qcacld-3.0`），代差小得多。
2. **PLD/icnss2 必须在 mainline 里重写或移植**：mainline 没有 `icnss2`。
   好消息是 ath11k 的 `qmi.c` 已经把 WLFW 握手全实现了
   （ind_register / msa_mem_info / msa_ready / cap / bdf / cal_report / wlan_ini / cfg / mode），
   可以直接拿来当 icnss2 的 QMI 客户端内核部分。
3. **CE/HIF 的寄存器访问**：必须保留 qcacld 的 `ADRASTEA_SHADOW_REGISTERS` 路径
   （写 CE 写指针 → SR_APSS 影子槽），并且 **WLAN_CFG 必须带 `shadow_reg`(v1) 12 条表**
   + 原厂 adrastea 12 管 CE 配置（§11）。这三条我们已经用硬件验证过，
   ~~正好可以照抄~~ 直接给 qcacld 侧对照确认。

---

## 4. 分阶段计划（建议按 q1–q5）

### q1：拿到可编译的源码（0.5–1 天）
- 目标：`qca-wifi-host-cmn` + `fw-api` + `qcacld-3.0` 三个树在 mainline 头文件下
  **先编过 `hif`/`ce` 子目录**（不链接内核）。
- 做法：从 6.6 时代 QCOM 发布（CodeLinaro `wlan/...`）或厂商 6.6 内核取；
  用 `make -C <mainline> M=<qcacld> modules` 试编，收集 API 报错清单。

### q2：icnss2 站在 mainline 里（1–2 天）
- 移植 `drivers/soc/qcom/icnss2/`（源码来自 `sky-lineage` 5.10 树）到 mainline：
  - 用 mainline `remoteproc`/`qcom_q6v5_pas` 启动 wpss（我们已经能用它跑起来固件）
  - QMI 客户端直接用 ath11k 的 `qmi.c` 逻辑改造
  - regulator/clock/interconnect 用 mainline 框架（regulator 对照已在 FINDINGS §9.1）
- **里程碑 M1**：`dmesg` 里出现 `QMI Server Connected` + BDF/cal report 成功
  （这在 ath11k 里已经做到过，可复用）。

### q3：qcacld 起来（最大工作量）
- 先让 `qca_cld3_adrastea`-等价模块在 mainline 下 `insmod` 成功、注册 wiphy；
- CE/HIF 走影子槽路径（`ADRASTEA_SHADOW_REGISTERS`）；
- **里程碑 M2**：HTC 连接成功（这是我们 ath11k 卡住的地方，qcacld 的原厂路径应该能过）。

### q4：功能验证
- **M3**：`wlan0` 出现、`iw dev` 能扫描；M4：连上 AP 收发；M5：吞吐/稳定性跑分。

### q5：可选的上游化讨论
- 若目标是上游，qcacld 本身几乎不可能进 mainline；可讨论的是把
  「adrastea 型固件自管数据面」抽象成 mainline 新驱动（ath11k 之外的 `ath1x`），
  但那需要原厂固件的接口文档，短期不做。

---

## 5. 已知风险与坑

1. **qcacld 与固件版本耦合**：原厂 ROM 的 qcacld 与 wpss 固件是同批编译的；
   我们已把固件换成 2024-09 版（与 ROM 一致），**qcacld 也要用同批源码**，
   否则 WMI TLV 版本会不匹配。
2. **GKI/符号导出**：qcacld 依赖大量内核符号，mainline 未必导出（`EXPORT_SYMBOL` 差异）；
   需要 `Module.symvers` 对齐或打补丁。
3. **`ADRASTEA_SHADOW_REGISTERS` 与 CE 12 管表**：这两个必须与原厂一致，
   我们已有实测数据（§11.2 的 slot 映射、`ADDRESS_VALID=0xf860b9` 作为"配置正确"的判据）。
4. **不要**再用 pre-BDF 的 QMI 实验（会整机挂死），也不要盲扫 SoC 物理地址。
5. **实验参数一律走 `/etc/sky-wifi-params.try`**（一次性），否则 panic 会每次开机复现。
6. 设备 `/data` 现在是 ext4；若要回原厂 Android，需要先 `make_f2fs`
   （见 `backup/pre-stock-2026-11-02/README.md`）。

---

## 6. 交接时设备状态

```
内核        #103（k104 镜像）  主线 Linux 7.3
根文件系统  /dev/sda20 ext4，已还原 rootfs（304 MB 已用）
固件        fw_version 0x3431fb72（原厂 2024 版）；旧版在 /root/wpss-old/
WiFi        ath11k 加载但无 wlan0（已知），helium 断言 0，DP abort 1
参数        bdf_total_full=y / snoc_reg_mode=4 / snoc_membase_mib=0 / snoc_probe=n
            snoc_shadow_probe=y / snoc_device_info=n / snoc_shadow_reg_v1=n
SSH         root@192.168.7.1（密码 sky）正常
```

新路线开始前建议：把 `snoc_*` 这些实验参数全部清掉（ath11k 不再需要），
只保留 rootfs/内核本身。

---

## 7. 附：外部 icnss2 源码对照（2026-11-02 调查）

查了 Motorola 公开内核仓库：

```
github.com/MotorolaMobilityLLC/kernel-msm
  commit 0450c50…  drivers/soc/qcom/icnss2/main.h:22
```

结论（**相关但帮助有限**）：

| 检查项 | 结果 |
|---|---|
| 内核版本 | `Makefile` = **5.10.136** —— 与 Xiaomi `lineage-23.2` 同代，**不**更接近 mainline 7.3 |
| `ADRASTEA_DEVICE_ID` | 同样是 `0xabcd`，`ADRASTEA_PATH_PREFIX "adrastea/"`（main.h:22-30） |
| `shadow_reg` 分支 | `qmi.c:3211-3220` **与我们分析的完全一致**：WCN6750 走 `shadow_reg_v2`，ADRASTEA 走 `shadow_reg`(v1) ⇒ 证明这个分支是**通用实现**，不是小米特有 |
| 是否含 qcacld-3.0 源码 | **否**（全树 73070 个文件里没有 `qcacld`；只有 `drivers/net/wireless` 的主线无线驱动 + `icnss2`） |
| 是否含 adrastea 设备的 DTS 参考 | 未深入（Moto 的 adrastea 机型 SoC 与本机 SM4450 不同，参考价值低） |

**可用之处**：作为独立实现，可用来 diff 出「Xiaomi 私货 vs 通用代码」，
例如 Moto 版 `qmi.c:221` 有 `sprintf(filename, "adrastea/%s_%s_%s.bin", …)`
（`ICNSS_MAX_FILE_NAME //modify for different bdf loading`）这种定制点。

**不可用之处**：不能解决我们真正缺的东西 —— 本项目的关键问题是
「qcacld 如何把 CE ring 信息交给固件」，这段逻辑在 **qcacld-3.0（不在内核树里）**，
所以下一步取源码应走 CodeLinaro 的 `wlan/qcacld-3.0`（6.6/6.9 分支），
而不是任何 5.10 的 OEM 内核仓库。

---

## 8. 设备树参考（详见 `FINDINGS-dt-references-wcn6750-adrastea.md`）

五份 DT 参考已交叉对比完毕，结论：

| 参考 | compatible | 总线 | 对我们 |
|---|---|---|---|
| 原厂日版 ROM `dtbo.img` overlay | `qcom,icnss` | SNOC | ✅ **权威模板，照抄** |
| Lineage `ravelin-adrastea.dtsi` | `qcom,icnss` | SNOC | ✅ 与原型一致（MSA 3 MiB、无 ch1） |
| QC sm8750 `parrot-wcn3990.dtsi`（文件名误导） | `qcom,icnss` | SNOC | ✅ 与原型一致（含 ch1） |
| 主线 `kodiak.dtsi` | `qcom,wcn6750-wifi` | SNOC | ❌ ath11k 路线，本机固件不放权 |
| 主线 sm8750 OnePlus Dodge | `pci17cb,1107` | PCIe | ❌ 完全无关（ath12k） |

必须照抄的三件套：`reg = <0x22800000 0x800000>` + `qcom,icnss` + `adrastea/wpss.mdt`；
必须带上的开关：`bdf-download-support`、`wpss-support-enable`、`qcom,fw-prefix`；
必须改的：**MSA 12 MiB**（主线现为 3 MiB）、`vdd-3.3-ch1`（待确认板级接线）。

存档：`wifi-backup/{stock-icnss-nodes.dts, lineage-ravelin-adrastea.dtsi,
qualcomm-sm8750-parrot-wcn3990.dtsi, mainline-sm8750-oneplus-dodge.dts}`；
`sm4450-xiaomi-sky.dts` 可直接用的 icnss 节点草稿在该文档 §8。
