# qcacld-3.0 → mainline：Q1 源码基线与移植入口

> **状态：历史（同日被取代）。** 本文记录 Sony `aosp/LA.UM.7.3.r1` 作为 q1 基线的
> 评估过程与失败点；当天稍后主基线已改为 MiCode `sky-t-oss`
> （`FINDINGS-qcacld-branches-github.md` §1–§3）。Sony 树仍保留在
> `third_party/qcacld-3.0-sony-la73/` 作只读对照，不再推进。

日期：2026-11-03
目标：SM4450/ravelin + WCN6750/adrastea（Xiaomi Redmi 12 5G/sky）
目标内核：sky-mainline Linux 7.3

## 1. 当前结论

Sony Xperia Dev 的 `vendor-qcom-opensource-wlan-qcacld-3.0` 仓库**有用，但不能直接作为 sky 的运行基线**。

它适合作为：

- qcacld-3.0 的公开源码基线；
- Adrastea 条件编译和 `ADRASTEA_SHADOW_REGISTERS` 路径参考；
- out-of-tree Kbuild/Makefile 模板；
- Linux 6.x/7.x API 适配的起点。

它目前不能证明：

- 包含 SM4450/WCN6750 专用 target profile；
- 包含本机原厂使用的完整 12-CE 配置；
- 与本机 WPSS `0x3431fb72` 固件 ABI 匹配；
- 可以不经适配就在 Linux 7.3 上加载。

## 2. 已冻结的 Sony 候选

仓库：<https://github.com/sonyxperiadev/vendor-qcom-opensource-wlan-qcacld-3.0>

### 2.1 LA.UM.7.3 候选

- 分支：`aosp/LA.UM.7.3.r1`
- commit：`8dcf1fbf8bbfadbe6fbad47f9b9389f41221477d`
- 日期：2019-03-01
- 提交说明：Qualcomm LA.UM Android BSP 7.3 / QCACLD release 5.1.1.69T
- 本地工作树：`sky-linux/third_party/qcacld-3.0-sony-la73/`

这里的“7.3”是 Qualcomm Android BSP 版本，**不是 Linux kernel 7.3**。

### 2.2 当前默认分支

- 分支：`wlan-cld3.driver.lnx.2.0.r155-rel`
- commit：`5badf8e096acd1056080b00f2c3736bd495cb3c5`
- 日期：2026-05-22

默认分支较新，但提交主题是 Kalama Kiwi v2，不能据此假定它比 LA.UM.7.3 更匹配本机 Adrastea。后续应逐分支比较，而不是直接使用默认分支。

## 3. Sony 仓库中已确认的 Adrastea 相关能力

LA.UM.7.3 的 `Kbuild` 明确包含：

- `CONFIG_ICNSS` / `CONFIG_CNSS_ADRASTEA` 的 SNOC/Adrastea 条件；
- `QCA_WIFI_3_0_ADRASTEA`；
- `ADRASTEA_SHADOW_REGISTERS`；
- `ADRASTEA_RRI_ON_DDR`；
- `CONFIG_HIF_SNOC`；
- Adrastea 专用的若干 DP/HTT 条件路径。

关键位置：

- `third_party/qcacld-3.0-sony-la73/Kbuild:1328-1342`
- `third_party/qcacld-3.0-sony-la73/core/pld/src/pld_snoc.c`
- `third_party/qcacld-3.0-sony-la73/core/pld/inc/pld_common.h`

其中 `pld_wlan_enable_cfg` 已包含：

- CE target 配置；
- service-to-CE 配置；
- v1 `shadow_reg` 配置；
- v2/v3 shadow 配置；
- RRI-over-DDR 配置。

这与本机实测的 Adrastea `WLAN_CFG.shadow_reg` v1 机制相符，但 Sony 源码本身还不能证明其表格就是本机原厂的 12 条表。

## 4. GitLab postmarketOS sdm845 参考的价值

参考：<https://gitlab.com/-/snippets/3713157>

该 APKBUILD 是一个实际的 **qcacld-3.0 + qca-wifi-host-cmn + fw-api → Linux 6.9** 构建案例，目标为 SDM845。它的价值主要在工程方法，而不是硬件配置：

- 使用与 qcacld 同 tag 的 `qca-wifi-host-cmn`、`fw-api` 和内核；
- 先执行 `olddefconfig` / `modules_prepare`；
- 通过外部 Makefile/Kbuild 构建模块；
- 用 `KBUILD_MODPOST_WARN=1` 先完成对象/模块 smoke test；
- 记录并处理现代内核 API 变化，例如 `strlcpy`、`PDE_DATA`、`netif_rx_ni`、`class_create`、NAPI、debugfs、cfg80211、timer 等；
- 将兼容性修改写成可重复的构建脚本，而不是手工改设备源码。

但它也明确显示了风险：即使 Linux 6.9 能编译，仍可能有 `wireless_send_event`、`do_exit` 等未解析符号；而且 SDM845/旧 qcacld 分支与本机 SM4450/Adrastea 的 CE、QMI、固件 ABI 不同。因此它应作为**API 适配参考**，不应直接照搬其硬件/配置补丁。

## 5. 本地依赖状态

已存在：

- `wifi-backup/stock-wlan/qcom/opensource/wlan/qca-wifi-host-cmn/`
- `wifi-backup/stock-wlan/qcom/opensource/wlan/fw-api/`
- `sky-mainline/kernel/linux-7.3/`
- `backup/stock-jp-2026-11-02/jp_qca_cld3_adrastea.ko`

本地 host-cmn/fw-api 来自 Xiaomi/Lineage Android 模块树的后续合并版本，不能假定与 Sony LA.UM.7.3 qcacld ABI 完全匹配。第一阶段应先做静态版本/接口核对，不应立即混合编译。

## 6. DT 对 qcacld 移植的影响

参考 [FINDINGS-dt-references-wcn6750-adrastea.md](FINDINGS-dt-references-wcn6750-adrastea.md) 后，当前 `sm4450-xiaomi-sky.dts` 的 WLAN 节点仍是 ath11k 形态：

- `compatible = "qcom,wcn6750-wifi"`；
- `reg = <0x22800000 0x800000>`；
- 12 条 LEVEL_HIGH CE IRQ 和 SMMU `0x4c0` 已基本对应硬件；
- MSA 仍为 3 MiB；
- 节点当前为 `status = "disabled"`。

qcacld/icnss2 路线不能只把 compatible 改名。需要在后续平台胶水阶段切换为原厂 `qcom,icnss` 语义，并补齐/确认：

- `qcom,rproc-handle` 指向 WPSS remoteproc；
- `qcom,fw-prefix`、`qcom,wlan`、`bdf-download-support`、`wpss-support-enable`；
- `qcom,wlan-msa-fixed-region`；
- `qcom,iommu-dma`、fault、DMA pool 和 geometry 属性；
- 原厂 12 MiB MSA reserved-memory（当前 DTS 是 3 MiB）；
- `smp2p_wlan_1_out/in` 及 force-fatal/early-crash 通道；
- 原厂 icnss 所需供电描述，尤其是否需要 `vdd-3.3-ch1`。

但在 mainline 尚无 icnss2 驱动的前提下，不应提前把现有 ath11k 节点直接替换并刷机。DT 改动应与新的 icnss/PLD 驱动绑定，先通过 `dtc`/schema 和静态检查，再进入设备实验。原厂模板优先级高于 Lineage 和通用 `qcom,wcn6750-wifi` 节点。

## 7. 第一阶段实施边界

本阶段只做源码和构建验证：

1. 固定 qcacld、host-cmn、fw-api、mainline kernel 的 commit/版本；
2. 为 Sony LA.UM.7.3 保留只读 worktree；
3. 建立独立 out-of-tree 构建目录和日志目录；
4. 先解析 Kbuild/Kconfig，不修改设备树、rootfs 或 ath11k；
5. 分层编译：
   - Kbuild parse/Kconfig；
   - QDF/HIF/CE/HTC 对象；
   - qcacld 核心对象；
   - 最后才尝试完整 `.ko` 和 modpost；
6. 生成 API 报错分类和未定义符号清单；
7. 第一阶段不刷机、不 `insmod`、不修改设备参数。

建议的外部构建变量：

```text
KERNEL_SRC=/home/orange/redmi125g/sky-mainline/kernel/linux-7.3
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
WLAN_ROOT=<独立 qcacld 工作树>
WLAN_COMMON_ROOT=<匹配版本的 qca-wifi-host-cmn>
WLAN_FW_API=<匹配版本的 fw-api>
CONFIG_QCA_CLD_WLAN=m
CONFIG_ICNSS=y
CONFIG_HIF_SNOC=1
CONFIG_QMI_SUPPORT=1
CONFIG_CNSS_ADRASTEA=y
```

这些变量目前只是构建实验配置，不能直接视为最终设备配置；尤其 `CONFIG_ICNSS`、`CONFIG_CNSS_ADRASTEA` 需要先在 mainline 的平台胶水层定义并实现。

## 7. 必须保持的本机硬件事实

后续 qcacld 移植不得退回 ath11k 已经排除的模型：

- Adrastea 使用 `shadow_reg` v1，不是 WCN6750 hybrid 的 v2；
- 原厂使用 Adrastea 12 管 CE 配置；
- 已验证的 shadow 映射为：
  - source：CE 0/3/4/5/7，offset `0x3c`；
  - destination：CE 1/2/7/8/9/10/11，offset `0x40`；
  - 不加入 destination CE5；
- WLAN 配置必须在 FW_READY 后按 INI → CFG → MODE；
- 不能启用 ath11k 的 UMAC/DP 主机寄存器编程路径；
- 最终需要定位原厂 qcacld/icnss2 如何把 CE ring 信息交给固件。

## 8. 选择 Sony 分支的暂定结论

当前优先级：

1. 先以本地 `aosp/LA.UM.7.3.r1` 作为结构和 Adrastea 逻辑基线；
2. 再比较 `LA.UM.9.16/9.14/9.12` 与默认分支的现代 API 改动；
3. 选择同时满足“Adrastea 路径存在、源码可追溯、host-cmn/fw-api 可配套”的候选；
4. 若没有与本机固件同批的 qcacld/host-cmn/fw-api，则把 Sony 仓库定位为开源移植基线，不宣称 ABI 匹配。

## 9. 当前阻塞项

- 缺少与原厂 `qca_cld3_adrastea.ko` 同批次的完整 qcacld-3.0 源码；
- 本机已有 host-cmn/fw-api 与 Sony qcacld 的版本耦合关系尚未确认；
- mainline 7.3 没有 icnss2 公共 API，需要重写/适配 PLD 平台胶水；
- mainline 7.3 的 cfg80211、debugfs、timer、NAPI、crypto、procfs 和符号导出差异预计较大；
- Sony Kbuild 仍假设 Android/CNSS/ICNSS 构建环境，不能直接作为最终 mainline Kbuild。

## 10. Q1 初次构建 smoke test

已准备 mainline kernel headers：

```text
make -C sky-mainline/kernel/linux-7.3 \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
```

并对 LA.UM.7.3 worktree 运行了外置构建，使用：

```text
KERNEL_SRC=/home/orange/redmi125g/sky-mainline/kernel/linux-7.3
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
WLAN_COMMON_INC=/home/orange/redmi125g/wifi-backup/stock-wlan/qcom/opensource/wlan/qca-wifi-host-cmn
CONFIG_ICNSS=y CONFIG_QMI_SUPPORT=1 CONFIG_CNSS_ADRASTEA=y CONFIG_HIF_SNOC=y
```

结果：**尚未进入真正 API 适配阶段，构建在依赖/平台头文件层失败**。已确认的第一批问题：

1. Sony LA.UM.7.3 Kbuild 默认通过 `WLAN_COMMON_ROOT=../qca-wifi-host-cmn` 查找 host-cmn；外部构建必须显式传入绝对 `WLAN_COMMON_INC`，否则会出现错误的相对路径。
2. 加入 include 路径后，qcacld 源文件已开始编译，说明外置 Kbuild 入口有效。
3. 当前 host-cmn 首个硬阻塞是 `linux/qcom-iommu-util.h` 缺失；这是 Android/QCOM 私有头，mainline 7.3 没有该接口。
4. 同时出现 `hex_to_bin`/`hex2bin` 隐式声明，说明 host-cmn 的旧头文件依赖需要按 mainline API 修正。
5. Kbuild 仍生成 `-DANI_OS_TYPE_ANDROID=6`、大量旧 Android 配置，并把 `-Werror` 传入编译；后续必须先建立兼容层/补丁，而不是逐个盲改业务代码。
6. 构建标签阶段还尝试用 host-cmn 的 Git reflog 生成版本信息；跨仓库混合源码时会产生 `d392488~..HEAD` 歧义，应在可重复构建中关闭或重写该逻辑。

本轮没有刷机、没有安装模块、没有修改设备参数。完整原始日志保留在：

```text
/tmp/qcacld-q1-la73-build7.log
```

当前 smoke test 的结论是：Sony 仓库确实能作为 out-of-tree Kbuild 起点，但与 mainline 7.3 的第一道边界已经明确落在 QCOM 私有平台 API，而不是 qcacld 业务逻辑。

## 11. 本轮构建进展（adapt4）

在隔离的 LA.UM.7.3 worktree 中加入了最小构建适配：

- `compat/include/linux/qcom-iommu-util.h`：仅用于让当前 `CONFIG_QCOM_IOMMU_UTIL` 未启用的路径通过头文件解析；不是最终 IOMMU 实现；
- `linux/hex.h` 预包含；
- host-cmn 的 objmgr 和公共 UMAC include 路径；
- `-Wno-error`，用于收集 API 缺口而非把警告伪装成成功；
- 关闭跨仓库 build-tag reflog 逻辑；
- 添加 `QCA_WIFI_MAINLINE_BUILD` 标记和 SMMU S1 条件入口。

适配后构建已经越过 `qcom-iommu-util.h`、hex API 和部分 include 路径，暴露出下一层真实问题：

- `from_timer`、`del_timer`、`del_timer_sync`、`destroy_timer_on_stack` 未被当前 host-cmn 头文件正确解析；
- fw-api 的 `A_COMPILE_TIME_ASSERT` 在当前 GCC/内核组合下出现 file-scope variably-modified 错误；
- host-cmn 与 LA.UM.7.3 qcacld 之间存在接口代际不一致：`wlan_cmn.h` 与 qcacld 自带 `cds_reg_service.h` 重复定义 `enum phy_ch_width`；
- 缺失 `wlan_scan_public_structs.h` 等 host-cmn UMAC include；
- 这证明当前本地 host-cmn/fw-api 不能直接视为 Sony LA.UM.7.3 的配套版本。

这些修改只发生在：

```text
sky-linux/third_party/qcacld-3.0-sony-la73/
```

没有修改 mainline 内核源码、设备树或设备 rootfs。当前仍未生成可加载 `.ko`。

## 12. 结论

GitLab sdm845 参考证明“qcacld-3.0 out-of-tree 到现代内核”在工程上有先例，值得采用其**构建隔离、版本锁定和 API 补丁脚本**的方法。

Sony 仓库证明我们有一个公开的、含 Adrastea/SNOC/shadow-register 代码的 qcacld 源码起点。

但真正的 sky 适配仍必须以：

- 原厂 Adrastea 二进制；
- 本机已验证的 12 管 CE/shadow 表；
- 匹配的 host-cmn/fw-api；
- Linux 7.3 的新 PLD/icnss2 胶水；

为准。下一步应建立独立的 Q1 Kbuild smoke-test，而不是刷写设备。
