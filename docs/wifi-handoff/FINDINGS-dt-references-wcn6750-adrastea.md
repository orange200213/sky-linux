# WCN6750 / adrastea 设备树参考汇总

日期：2026-11-03
适用设备：Xiaomi Redmi 12 5G (`sky`) / SM4450 (`ravelin`) / WCN6750 (`adrastea`, SNOC)
用途：qcacld-3.0 → mainline 移植的 DT 依据；也用于快速识别「哪种 wifi 节点对应哪条驱动路线」

---

## 1. 为什么需要这份文档

WCN6750 这一代在设备树里有**三种完全不同的描述方式**，看起来都叫 "wifi 节点"，
但指向三条互不兼容的驱动路线。搞混会直接浪费几天：

* 抄成 PCIe 的节点 → 在 SNOC 平台上永远 probe 不到；
* 抄成 ath11k 的 `qcom,wcn6750-wifi` → 驱动能 probe，但在本机 adrastea 固件下
  访问 CE/UMAC 寄存器会 bus abort（2026-11-02 已实测，见 §7.7）；
* 抄成 `qcom,icnss` → 才是原厂真正在用的路线（qcacld-3.0 + icnss2）。

---

## 2. 决策表（先看这张）

| 节点写法 | compatible | 总线 | 驱动 | 主线可用 | 出现平台 |
|---|---|---|---|---|---|
| `qcom,icnss@22800000` | `qcom,icnss` | **SNOC**，membase 8 MiB | **icnss2 + qcacld-3.0**（out-of-tree） | ❌ 主线无 | **本机**、Lineage ravelin、QC sm8750 新树 |
| `wifi@17a10040` | `qcom,wcn6750-wifi` | SNOC，msi_addr + membase | **ath11k**（主线 ahb/snoc） | ✅ | kodiak（QCS6490 等 generic WCN6750） |
| `wifi@0`（`&pcieport0` 下） | `pci17cb,1107` | **PCIe** | **ath12k**（主线） | ✅ | sm8750 OnePlus Dodge（WCN7880/7850 级） |
| `wcn7880-pmu` | `qcom,wcn7880-pmu` | — | 无（DTS 自己注释 "No compatible drivers"） | ❌ | 同上 |

**识别口诀**：看到 `<0x22800000 0x800000>` + `qcom,icnss` + `adrastea/wpss.mdt` 三件套
才是我们要的；见到 `pci17cb,…` 或 `qcom,wcn6750-wifi` 都不是。

---

## 3. 参考一：原厂日版 ROM（唯一权威，抄它）

### 3.1 它在哪

**不在基础 DTB 里**，而在 `dtbo.img` 的 overlay 中。

基础 DTB（`vendor_boot.img` 内的 DTB）里三个 wifi 相关节点全是 `status = "disabled"`：

* `qcom,wcn6750`（PCIe/MSI 型，`reg = <0x17210040 0x0 0xb0000000 0x10000>`）
* `qcom,cnss-qca6490@b0000000`
* `qcom,icnss@22800000`（由 overlay 注入并启用）

```
ROM   : sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0/images/dtbo.img
扫描  : 40 个 FDT 中，15 个 overlay 带 qcom,icnss（同机型不同面板/配置变体）
存档  : wifi-backup/stock-icnss-nodes.dts
```

### 3.2 icnss 节点原文

```dts
remoteproc-wpss@8a00000 {
        firmware-name = "adrastea/wpss.mdt";     /* 覆盖基础 DTB 的 "qca6750/wpss.mdt" */
        phandle = <0x01>;
};

qcom,icnss@22800000 {
        compatible = "qcom,icnss";
        reg = <0x22800000 0x800000>;             /* membase：唯一寄存器窗口，8 MiB */
        reg-names = "membase";
        qcom,rproc-handle = <0x01>;              /* → 上面的 wpss remoteproc */

        iommus = <0xffffffff 0x4c0 0x01>;        /* SMMU stream id 0x4c0（S1） */
        interrupts = <0x00 0x185 0x04  0x00 0x19d 0x04  0x00 0x1a0 0x04
                      0x00 0x1a1 0x04  0x00 0x1b2 0x04  0x00 0x1b3 0x04
                      0x00 0x1b6 0x04  0x00 0x1b7 0x04  0x00 0x1b8 0x04
                      0x00 0x1c3 0x04  0x00 0x1c5 0x04  0x00 0x1c6 0x04>;
        /* 十进制 389 413 416 417 434 435 438 439 440 451 453 454
         * = CE0..CE11 的 12 条平台 IRQ（LEVEL_HIGH，不是 MSI） */

        qcom,iommu-dma = "fastmap";
        qcom,iommu-faults = "stall-disable", "HUPCF", "non-fatal";
        qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>;
        qcom,iommu-geometry      = <0xa0000000 0x10000000>;

        qcom,fw-prefix;                 /* 配合 ADRASTEA 分支 → 固件前缀 "adrastea/" */
        qcom,wlan;
        bdf-download-support;           /* ★ icnss2 走 BDF 下载路径 */
        wpss-support-enable;            /* ★ 由 WPSS 协处理器承载 */

        qcom,wlan-msa-fixed-region = <0x02>;   /* → wlan_msa_mem_region@82a00000, 12 MiB */

        vdd-cx-mx-supply   = <0xffffffff>;     /* 占位：实际值在另一 fragment 填入 */
        vdd-1.8-xo-supply  = <0xffffffff>;
        vdd-1.3-rfa-supply = <0xffffffff>;
        vdd-3.3-ch1-supply = <0xffffffff>;
        vdd-3.3-ch0-supply = <0xffffffff>;
        qcom,vdd-cx-mx-config   = <0x00 0x00>;
        qcom,vdd-3.3-ch1-config = <0x2dc6c0 0x328980>;   /* 3000000..3312000 µV */
        qcom,vdd-3.3-ch0-config = <0x2dc6c0 0x328980>;

        qcom,smem-states = <0xffffffff 0x00>;
        qcom,smem-state-names = "wlan-smp2p-out";

        qcom,smp2p_map_wlan_1_in {
                interrupts-extended = <0xffffffff 0x00 0x00  0xffffffff 0x01 0x00>;
                interrupt-names = "qcom,smp2p-force-fatal-error",
                                  "qcom,smp2p-early-crash-ind";
        };

        phandle = <0x31>;
};
```

### 3.3 配套的 reserved-memory（基础 DTB）

```dts
wlan_msa_mem_region@82a00000  { no-map; reg = <0x0 0x82a00000 0x0 0xc00000>; };   /* 12 MiB */
wpss_moselle_region@85300000  { no-map; reg = <0x0 0x85300000 0x0 0x1900000>; };  /* 25 MiB */
cnss_wlan_region { compatible = "shared-dma-pool"; reusable;
                   alloc-ranges = <0x0 0x0 0x0 0xffffffff>;
                   alignment = <0x0 0x400000>; size = <0x0 0x2000000>;
                   status = "disabled"; };
```

### 3.4 wpss remoteproc 本体（基础 DTB）

```dts
remoteproc-wpss@8a00000 {
        compatible = "qcom,ravelin-wpss-pas";
        reg = <0x8a00000 0x10000>;
        memory-region = <&wpss_moselle_region>;
        clocks = <&rpmhcc 0>;  clock-names = "xo";
        cx-supply = <&…>; cx-uV-uA = <0x180 0x186a0>;
        mx-supply = <&…>; mx-uV-uA = <0x180 0x186a0>;
        qcom,qmp = <&aoss_qmp>;
        interrupt-names = "wdog","fatal","handover","ready","stop-ack","shutdown-ack";
        qcom,smem-states = <&smp2p_wpss_out 0>;  qcom,smem-state-names = "stop";
        firmware-name = "qca6750/wpss.mdt";      /* overlay 覆盖成 adrastea/wpss.mdt */

        glink-edge {
                qcom,remote-pid = <0x0d>;  label = "wpss";  qcom,glink-label = "wpss";
                qcom,wpss_qrtr { qcom,glink-channels = "IPCRTR";
                                 qcom,intents = <0x800 0x5 0x2000 0x3 0x4400 0x2>; };
        };
};
```

### 3.5 相关的 smp2p（基础 DTB）

```dts
qcom,smp2p-wpss {
        compatible = "qcom,smp2p";  qcom,smem = <0x269 0x268>;
        qcom,remote-pid = <0x0d>;
        master-kernel { qcom,entry-name = "master-kernel"; };
        slave-kernel  { qcom,entry-name = "slave-kernel";  interrupt-controller; };
        qcom,smp2p-wlan-1-in  { qcom,entry-name = "wlan";            interrupt-controller; };
        qcom,smp2p-wlan-1-out { qcom,entry-name = "wlan"; };
        qcom,smp2p-wlan-2-in  { qcom,entry-name = "wlan_soc_wake";   interrupt-controller; };
        qcom,smp2p-wlan-2-out { qcom,entry-name = "wlan_soc_wake"; };
        qcom,smp2p-wlan-3-out { qcom,entry-name = "wlan_ep_power_save"; };
};
```

---

## 4. 参考二：LineageOS `ravelin-adrastea.dtsi`

```
来源 : LineageOS/android_kernel_xiaomi_sm8450-devicetrees
提交 : 2e649a7f3e762903eedad43d8256b91e28b512d8
路径 : qcom/ravelin-adrastea.dtsi （61 行）
存档 : wifi-backup/lineage-ravelin-adrastea.dtsi
```

内容与原厂节点**逐字段一致**，只有两处差异（§7.1、§7.2）：

```dts
&reserved_memory {
        wlan_msa_mem: wlan_msa_mem_region@82a00000 {
                no-map; reg = <0x0 0x82a00000 0x0 0x300000>;    /* 3 MiB，原厂是 12 MiB */
        };
};
&soc {
        wpss_pas: remoteproc-wpss@8a00000 { firmware-name = "adrastea/wpss.mdt"; };
        icnss: qcom,icnss@22800000 {
                compatible = "qcom,icnss";
                reg = <0x22800000 0x800000>;  reg-names = "membase";
                qcom,rproc-handle = <&wpss_pas>;
                iommus = <&apps_smmu 0x4c0 0x1>;
                interrupts = <GIC_SPI 389 … 454>;        /* 12 条，带 CE0..CE11 注释 */
                qcom,iommu-dma = "fastmap";
                qcom,iommu-faults = "stall-disable","HUPCF","non-fatal";
                qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>;
                qcom,iommu-geometry      = <0xa0000000 0x10000000>;
                qcom,fw-prefix;  qcom,wlan;
                bdf-download-support;  wpss-support-enable;
                qcom,wlan-msa-fixed-region = <&wlan_msa_mem>;
                vdd-cx-mx-supply   = <&L11B>;    /* 0.348 V */
                vdd-1.8-xo-supply  = <&L23B>;    /* 1.8 V   */
                vdd-1.3-rfa-supply = <&L14B>;    /* 1.15 V  */
                vdd-3.3-ch0-supply = <&L7E>;     /* 3.3 V   */
                qcom,vdd-cx-mx-config   = <0 0>;
                qcom,vdd-3.3-ch0-config = <3000000 3312000>;   /* 与原厂 0x2dc6c0/0x328980 等价 */
                qcom,smem-states = <&smp2p_wlan_1_out 0>;
                qcom,smem-state-names = "wlan-smp2p-out";
                qcom,smp2p_map_wlan_1_in { … };
        };
};
```

---

## 5. 参考三：Qualcomm sm8750 树的 `parrot-wcn3990.dtsi`（命名陷阱）

```
来源 : MrBadger183/kernel_sm8750-devicetrees
提交 : 6fb47beb03f128edd11c6300951c88e3a5333249
路径 : qcom/wlan/parrot-wcn3990.dtsi （71 行）
存档 : wifi-backup/qualcomm-sm8750-parrot-wcn3990.dtsi
```

**文件名写着 wcn3990，内容却是 adrastea/icnss2 节点**：

* 同样的 `adrastea/wpss.mdt`、`reg = <0x22800000 0x800000>`、12 条 CE 中断、`0x4c0` SMMU；
* 稳压器换成 sm8750 平台的轨：`L11B / L1E / L14B / L11E(ch1) / L10E(ch0)`；
* **ch0 与 ch1 都有**（与原厂日版 ROM 一致，与 Lineage 不同）。

结论：这是 QC 在新 SoC 树里把 ravelin 的 adrastea 节点复制过来改电源脚的结果，
文件名是历史残留。它的价值是**证明 icnss 节点写法跨平台通用**，可以直接照抄骨架。

同目录还有 `alor-wcn7750.dtsi` 等 WCN7750（ath12k / wcn7850 级）配置，属另一条路线。

---

## 6. 参考四/五：两条「主线已支持」的路线（识别用，对我们不可用）

### 6.1 SNOC + ath11k：主线 `kodiak.dtsi` 的 `qcom,wcn6750-wifi`

```dts
wifi: wifi@17a10040 {
        compatible = "qcom,wcn6750-wifi";
        reg = <0 0x17a10040 0 0x0>;
        iommus = <&apps_smmu 0x1c00 0x1>;
        interrupts = <GIC_SPI 768 IRQ_TYPE_EDGE_RISING>,
                     … /* 共 32 条，GIC_SPI 768..799，EDGE_RISING（MSI 轮询式） */
                     <GIC_SPI 799 IRQ_TYPE_EDGE_RISING>;
        qcom,rproc = <&remoteproc_wpss>;
        memory-region = <&wlan_fw_mem>, <&wlan_ce_mem>;   /* ★ 有独立的 CE ring 内存区 */
        qcom,smem-states = <&wlan_smp2p_out 0>;
        qcom,smem-state-names = "wlan-smp2p-out";
        status = "disabled";
};
```

关键差别（决定两条路线不可互换）：

| 项 | `qcom,wcn6750-wifi`（ath11k） | `qcom,icnss`（icnss2/qcacld） |
|---|---|---|
| reg | `0x17a10040`（msi_addr，size 0） | `0x22800000 0x800000`（membase） |
| 中断 | 32 条 **EDGE_RISING**（MSI 型） | 12 条 **LEVEL_HIGH**（每 CE 一条） |
| SMMU stream | `0x1c00` | `0x4c0` |
| CE ring 内存 | **有** `<&wlan_ce_mem>` 专用保留区 | **没有**（主机不建 CE 环） |
| 谁编程 CE/DP 环 | **主机 ath11k** | 固件；主机只写 SR_APSS 影子槽的写指针 |
| 专有属性 | `qcom,msa-fixed-perm` | `bdf-download-support`、`wpss-support-enable`、`qcom,fw-prefix` |

> `wlan_ce_mem` 这一条尤其重要：它说明 ath11k 路线的前提是「CE 环在主机 DDR 里、
> 由主机编程 CE 寄存器指过去」。adrastea 路线没有这个 region，正是因为它不走这条路。

### 6.2 PCIe + ath12k：sm8750 OnePlus Dodge（主线 v6.16）

```
来源 : AaronKel/sm8750-mainline
路径 : arch/arm64/boot/dts/qcom/sm8750-oneplus-dodge.dts
存档 : wifi-backup/mainline-sm8750-oneplus-dodge.dts
```

```dts
&pcieport0 {
        wifi@0 {
                compatible = "pci17cb,1107";          /* WCN7880/7850 级 PCIe 器件 */
                reg = <0x10000 0x0 0x0 0x0 0x0>;
                vddrfacmn-supply = <&vreg_pmu_rfa_cmn>;
                vddaon-supply    = <&vreg_pmu_aon_0p59>;
                vddwlcx-supply   = <&vreg_pmu_wlcx_0p8>;
                vddwlmx-supply   = <&vreg_pmu_wlmx_0p85>;
                vddrfa0p8-supply = <&vreg_pmu_rfa_0p8>;
                vddrfa1p2-supply = <&vreg_pmu_rfa_1p2>;
                vddrfa1p8-supply = <&vreg_pmu_rfa_1p8>;
                vddpcie0p9-supply= <&vreg_pmu_pcie_0p9>;
                vddpcie1p8-supply= <&vreg_pmu_pcie_1p8>;
        };
};

wcn7880-pmu {                       /* DTS 自带注释：No compatible drivers */
        compatible = "qcom,wcn7880-pmu";
        wlan-enable-gpios = <&tlmm 16 GPIO_ACTIVE_HIGH>;
        bt-enable-gpios   = <&pm8550ve_f_gpios 3 GPIO_ACTIVE_HIGH>;
        … 9 路 vdd*/vddio*/vddaon/vdddig/vddrfa* 供电 …
        status = "disabled";
};
```

要点：这条路线**没有 icnss、没有 wpss remoteproc、没有 membase**。
它不是「wifi 节点没写」，而是**整台机器的 WLAN 走 PCIe**，节点就是 `wifi@0`。
对本项目零参考价值。

---

## 7. 差异点逐条分析

### 7.1 MSA 保留区大小：用原厂的 12 MiB

| 来源 | `wlan_msa_mem` |
|---|---|
| 原厂日版 ROM | `0x82a00000` + **`0xc00000`（12 MiB）** |
| Lineage ravelin | `0x82a00000` + `0x300000`（3 MiB） |
| QC sm8750 parrot | `0x300000`（3 MiB） |
| 我们 mainline `sm4450-xiaomi-sky.dts` | `0x300000`（3 MiB）← 跟随 Lineage |

MSA 是固件运行内存（`WLFW_MSA_MEM_INFO` 交给固件）。**移植 qcacld 时应改成 12 MiB**。

### 7.2 稳压器：ch1 的有无

| 来源 | 供电轨 |
|---|---|
| 原厂日版 ROM | cx-mx、1.8-xo、1.3-rfa、**3.3-ch0、3.3-ch1**（5 路） |
| QC sm8750 parrot | 5 路（L11B / L1E / L14B / **L11E / L10E**） |
| Lineage ravelin | 4 路（**无 ch1**） |
| 我们 mainline | `vreg_l11b_0p348`、`vreg_l23b_1p8`、`vreg_l14b_1p15`、`vreg_l7e_3` |

mainline 侧这 4 路**都已 enabled**（`FINDINGS-session7` §9.1 实测）。
ch1 是否需要，取决于本机板级接线（原厂 overlay 里是 `<0xffffffff>` 占位，值在另一 fragment）。

### 7.3 SMMU stream id 与 iommu 属性

* icnss 路线：`<&apps_smmu 0x4c0 0x1>`
* ath11k 路线（kodiak）：`<&apps_smmu 0x1c00 0x1>`
* PCIe 路线：无

`qcom,iommu-dma = "fastmap"`、`qcom,iommu-faults = "stall-disable","HUPCF","non-fatal"`、
`qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>`、`geometry` 同值 —— icnss 路线必须全有。

### 7.4 中断：icnss 是 12 条 LEVEL_HIGH，不是 MSI

```
GIC_SPI 389 413 416 417 434 435 438 439 440 451 453 454   → CE0..CE11
```

与原厂日志 `pld_get_user_msi_assignment … Not supported on type 1` 完全对应：
SNOC 无 MSI，每 CE 一条独立中断线。移植时不要照搬 MSI 逻辑。

### 7.5 SMEM / smp2p

必须成对，且指向 `qcom,smp2p-wpss` 的 `wlan` / `wlan_soc_wake` / `wlan_ep_power_save`
（`remote-pid = 0x0d`）：

```
qcom,smem-states      = <&smp2p_wlan_1_out 0>;
qcom,smem-state-names = "wlan-smp2p-out";
qcom,smp2p_map_wlan_1_in { interrupt-names = "qcom,smp2p-force-fatal-error",
                                            "qcom,smp2p-early-crash-ind"; };
```

### 7.6 icnss 独有的三个行为开关

* `bdf-download-support` —— BDF 走 QMI 下载（而不是 request_firmware）；
* `wpss-support-enable` —— 由 WPSS 协处理器承载；
* `qcom,fw-prefix` —— 触发 `adrastea/` 前缀。

漏写会导致 icnss2 走错分支（如去找 `qca6750/`）。

### 7.7 寄存器可见性（为什么 ath11k 路线在本机行不通）

这是 2026-11-02 用 `skyprobe` 直接对硬件测得（`FINDINGS-session9` §19）：

```
membase 8 MiB 窗口内，只有 0x20000..0x52000（SR_APSS）可读；
membase+0x240000(CE0) / +0x24C000(CE wrapper) / +0x2xxxxx 全部 DEAD；
插值扫描 0x22850000..0x23000000 @0x10000：123 个探针 0 live。
主机唯一可写通路 = SR_APSS 影子槽（12 个 CE 写指针，已验证可转发）。
```

⇒ ath11k 的「主机编程 CE/DP 环」模型在本机不可能工作，**必须走 icnss2/qcacld 路线**。

---

## 8. 移植用 DT 草稿（可直接放进 `sky-mainline/dts/sm4450-xiaomi-sky.dts`）

> 以原厂为准。新增 icnss 节点时，原有 ath11k 节点要删掉或 `status = "disabled"`，
> 两者不能同时 probe。

```dts
/* ---- 保留区：MSA 用原厂的 12 MiB（当前主线是 3 MiB，注意改） ---- */
wlan_msa_mem: wlan-msa@82a00000 {
        reg = <0x0 0x82a00000 0x0 0xc00000>;      /* 12 MiB */
        no-map;
};

/* ---- icnss2 / qcacld 节点 ---- */
icnss: icnss@22800000 {
        compatible = "qcom,icnss";
        reg = <0x0 0x22800000 0x0 0x800000>;
        reg-names = "membase";

        qcom,rproc-handle = <&wpss_pas>;
        iommus = <&apps_smmu 0x4c0 0x1>;
        interrupts = <GIC_SPI 389 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 413 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 416 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 417 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 434 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 435 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 438 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 439 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 440 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 451 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 453 IRQ_TYPE_LEVEL_HIGH>,
                     <GIC_SPI 454 IRQ_TYPE_LEVEL_HIGH>;

        qcom,iommu-dma = "fastmap";
        qcom,iommu-faults = "stall-disable", "HUPCF", "non-fatal";
        qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>;
        qcom,iommu-geometry      = <0xa0000000 0x10000000>;

        qcom,fw-prefix;
        qcom,wlan;
        bdf-download-support;
        wpss-support-enable;
        qcom,wlan-msa-fixed-region = <&wlan_msa_mem>;

        vdd-cx-mx-supply   = <&vreg_l11b_0p348>;
        vdd-1.8-xo-supply  = <&vreg_l23b_1p8>;
        vdd-1.3-rfa-supply = <&vreg_l14b_1p15>;
        vdd-3.3-ch0-supply = <&vreg_l7e_3>;
        /* vdd-3.3-ch1-supply = <…>;  需先确认本机 ch1 接哪路 PMIC */

        qcom,vdd-cx-mx-config   = <0 0>;
        qcom,vdd-3.3-ch0-config = <3000000 3312000>;

        qcom,smem-states = <&smp2p_wlan_1_out 0>;
        qcom,smem-state-names = "wlan-smp2p-out";

        qcom,smp2p_map_wlan_1_in {
                interrupts-extended = <&smp2p_wlan_1_in 0 0>,
                                      <&smp2p_wlan_1_in 1 0>;
                interrupt-names = "qcom,smp2p-force-fatal-error",
                                  "qcom,smp2p-early-crash-ind";
        };
};
```

**尚缺的依赖**（我们当前 DT 里没有，需要按原厂补齐）：

* `wpss_pas` remoteproc 节点标签（mainline 已有 `remoteproc-wpss@8a00000`，
  但要加 `firmware-name = "adrastea/wpss.mdt"` 或保留现有路径）；
* `smp2p_wlan_1_out` / `smp2p_wlan_1_in`（`qcom,smp2p-wpss` 下的 `wlan` 通道，
  `remote-pid = 0x0d`）；
* MSA 大小 3 MiB → **12 MiB**。

---

## 9. 复现方法（怎么从 ROM 里再挖一遍）

### 9.1 扫 FDT（最稳，不依赖 header 布局）

```bash
python3 - <<'EOF'
import struct, os
ROM='/home/orange/sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0/images'
for name in ('dtbo.img','boot.img','vendor_boot.img'):
    d=open(os.path.join(ROM,name),'rb').read(); i=0
    while True:
        i=d.find(b'\xd0\x0d\xfe\xed', i)
        if i<0: break
        tot=struct.unpack_from('>I',d,i+4)[0]
        if 0x100 < tot < 0x400000 and i+tot <= len(d):
            print(name, hex(i), tot)
        i+=4
EOF
```

### 9.2 反编译 + 找节点

```bash
dtc -I dtb -O dts -o stock.dts candidate.dtb
grep -n -A40 'qcom,icnss' stock.dts
```

### 9.3 已踩过的坑

1. `boot.img` / `vendor_boot.img` 的 header 字段随版本变化（v3 vs v4），
   **不要按字段偏移解析，直接扫 FDT magic + totalsize 校验**最稳；
2. 解 Android sparse 镜像时，**CRC chunk（`0xCAC4`）必须跳过 `total_sz-12` 字节**，
   否则后续 chunk 全部错位（表现为"读出来的东西不是任何文件系统"）；
3. 日版与欧版 `super.img` 的分区 extent **完全不同**，必须各解各的 LP metadata：
   * 日版：`vendor_a` = extent 4 @ raw `0xf4200000`（793 MiB）
   * 欧版：`vendor_a` = extent 4 @ raw `0x114e00000`
4. `icnss` 节点**不在基础 DTB 里**，一定去 `dtbo.img` 找。

---

## 10. 存档清单

| 文件 | 内容 |
|---|---|
| `wifi-backup/stock-icnss-nodes.dts` | 原厂日版 ROM overlay 原文摘录（**权威模板**） |
| `wifi-backup/lineage-ravelin-adrastea.dtsi` | LineageOS ravelin 参考（61 行） |
| `wifi-backup/qualcomm-sm8750-parrot-wcn3990.dtsi` | QC sm8750 树（文件名误导，内容同 adrastea） |
| `wifi-backup/mainline-sm8750-oneplus-dodge.dts` | 主线 sm8750 PCIe 路线（`wifi@0` + `wcn7880-pmu`） |
| `sky-mainline/dts/sm4450-xiaomi-sky.dts` | 我们当前 DT（ath11k 节点，MSA 3 MiB） |
| `sky-mainline/kernel/linux-7.3/arch/arm64/boot/dts/qcom/kodiak.dtsi` | 主线 ath11k WCN6750 参考 |
| 本文 | 五份参考的对照、差异分析与 DT 草稿 |

---

## 11. 更新记录

* 2026-11-03 初版：汇总原厂 ROM / Lineage ravelin / QC sm8750 parrot / 主线 kodiak /
  主线 sm8750 PCIe 五份参考；给出决策表、字段级差异分析（MSA 大小、ch1 稳压器、
  SMMU stream id、中断类型、`wlan_ce_mem` 有无）、`qcom,icnss` DT 草稿与复现方法。
