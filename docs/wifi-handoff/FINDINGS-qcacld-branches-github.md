# GitHub qcacld-3.0 分支搜索结果

日期：2026-11-03
目标：为 Redmi 12 5G / SM4450 / WCN6750 Adrastea 选择比 Sony LA.UM.7.3 更合适的 qcacld-3.0 基线。

## 1. 结论

目前找到的最佳候选不是 SM8450/SM8750 仓库，而是 Xiaomi/MiCode 的：

- 仓库：<https://github.com/MiCode/vendor_qcom_opensource_wlan>
- 分支：`sky-t-oss`
- commit：`02e10575f1357db03d74b9d59a1edd3a88614181`
- 日期：2023-07-31
- 提交说明：`Kernel: Xiaomi wlan changes for Redmi Note 12R Android T`
- Qualcomm 基线：`LA.VENDOR.1.0.r1-21200-WAIPIO.QSSI13.0-1`
- 本地副本：`sky-linux/third_party/micode-sky-t-oss/`

该分支一次性包含配套的：

```text
qcacld-3.0/
qca-wifi-host-cmn/
fw-api/
```

这是目前唯一同时满足“Xiaomi、sky 命名、较新 Android/QCOM WLAN 组合、qcacld/host-cmn/fw-api 同批提交”的公开候选。它仍不能证明与本机 WPSS 固件完全 ABI 匹配，但比 Sony 2019 LA.UM.7.3 单独 qcacld + 2026 host-cmn 混搭明显更合适。

## 2. MiCode sky-t-oss 的硬证据

### 2.1 真实 Adrastea 支持

`qca-wifi-host-cmn` 中确认存在：

- `TARGET_TYPE_ADRASTEA = 19`；
- `HIF_TYPE_ADRASTEA`；
- `adrastea_reg_def.h`；
- Adrastea target interface；
- Adrastea 专用 CE 配置；
- Adrastea shadow register 配置。

### 2.2 影子寄存器表与本机实测完全吻合

文件：

```text
qca-wifi-host-cmn/hif/src/ce/ce_main.c
```

表为：

```text
src: CE 0/3/4/5/7 -> offset 0x3c
dst: CE 1/2/7/8/9/10/11 -> offset 0x40
```

并且没有加入 destination CE5。这与本机 k98/k99/k100 的硬件实验完全一致：destination CE5 会触发固件断言，正确表是 12 个 shadow 配置项。

关键位置：`ce_main.c:511-529`。

### 2.3 CE 几何参数与本机记录吻合

文件：

```text
qca-wifi-host-cmn/hif/src/ce/ce_assignment.h
```

`host_ce_config_wlan_adrastea[]` 与 `target_ce_config_wlan_adrastea[]` 明确存在，包含 CE0–CE11：

- CE0：host → target HTC control/raw streams；
- CE1：target → host HTT；
- CE2：target → host WMI + HTC control；
- CE3：host → target WMI；
- CE4：host → target HTT；
- CE5：IPA → target，`PIPEDIR_OUT`、1024/64、disable interrupt；
- CE6：保留给 autonomous HIF memcpy；
- CE7：`PIPEDIR_INOUT_H2H`；
- CE8：IPA；
- CE9–CE11：target → host HTT/PKTLOG。

关键位置：`ce_assignment.h:926-1017`。

这比 Sony LA.UM.7.3 分支更重要，因为它直接提供了与本机 Adrastea 运行态和固件约束相符的 CE target/host 表。

### 2.4 原厂平台入口

`qcacld-3.0/core/pld/src/pld_snoc.c` 中包含：

- `ADRASTEA_DEVICE_ID 0xabcd`；
- `icnss` 平台注册；
- Adrastea SNOC WLAN enable 路径；
- v1 `shadow_reg` 配置传递。

这与本机原厂 `qca_cld3_adrastea.ko`/`icnss2` 路线一致。

## 3. 本地候选状态

已克隆：

```text
/home/orange/redmi125g/sky-linux/third_party/micode-sky-t-oss/
```

三部分均来自同一提交：

```text
qcacld-3.0       02e10575f1357db03d74b9d59a1edd3a88614181
qca-wifi-host-cmn 02e10575f1357db03d74b9d59a1edd3a88614181
fw-api            02e10575f1357db03d74b9d59a1edd3a88614181
```

## 4. 与其他 GitHub 候选的比较

### A. MiCode `sky-t-oss` —— 当前首选

<https://github.com/MiCode/vendor_qcom_opensource_wlan/tree/sky-t-oss>

优点：

- Xiaomi 官方组织；
- 分支名包含 `sky`；
- 提交说明明确 Redmi Note 12R；
- qcacld、host-cmn、fw-api 同一提交；
- 直接包含 Adrastea target、12 管 CE 表和本机实测一致的 shadow 表；
- Qualcomm 基线为较新的 `LA.VENDOR.1.0`/WAIPIO 系列。

风险：

- Redmi Note 12R 与当前设备是否完全同板、同固件 ABI 仍需核实；
- Android T/QCOM Kbuild 仍不是 mainline 7.3；
- 可能包含与 Xiaomi Android kernel/ICNSS2 耦合的 API。

### B. Sony `aosp/LA.UM.7.3.r1` —— 次选/历史参考

<https://github.com/sonyxperiadev/vendor-qcom-opensource-wlan-qcacld-3.0/tree/aosp/LA.UM.7.3.r1>

commit：`8dcf1fbf8bbfadbe6fbad47f9b9389f41221477d`。

优点：

- 有 Adrastea 宏和 SNOC/Kbuild 结构；
- 可作为外置模块构建模板。

缺点：

- 2019 年 LA.UM.7.3 BSP；
- 当前构建使用的是另一套 2026 host-cmn/fw-api，已经出现 timer、objmgr、fw-api assert 和头文件代际冲突；
- 没有当前本机相关的 Xiaomi sky CE 证据。

### C. OnePlus-12-Development `lineage-21`

<https://github.com/OnePlus-12-Development/vendor_qcom_opensource_wlan_qcacld-3.0>

只有 `lineage-21` 分支，commit `8e5facdc9474b352d461cecc7e6c01625caac0aa`。它可能对较新 Android API 有帮助，但没有目前证据证明它比 MiCode sky-t-oss 更接近 Adrastea/SM4450，不作为首选。

### D. arter97-mirror `caf_qcacld-3.0`

<https://github.com/arter97-mirror/caf_qcacld-3.0>

该仓库分支很多，包括 `LA.UM.7.3.c25`、`LA.UM.9.x` 等，适合查 Qualcomm CAF 历史版本和 API 演进，但它是通用 CAF 镜像，不是本机配套组合，也没有当前证据证明有 sky/Adrastea 12 管配置。暂作为比较数据库。

### E. TheNotOnly `sdm845-caf-qcacld-3.0`

<https://github.com/TheNotOnly/sdm845-caf-qcacld-3.0>

仓库 2020 年即停止更新，目标是 SDM845 老平台，只适合历史参考，不适合作为当前基线。

### F. metacrito `qcom-wlan-qcacld-3.0`

<https://github.com/metacrito/qcom-wlan-qcacld-3.0>

是 fork，缺少本机平台匹配证据，不优先。

## 5. 为什么目前不优先 SM8450/SM8750

SM8450/SM8750 相关公开树有参考价值，尤其可用于：

- 较新的 cfg80211/mainline API 适配；
- 现代 Kbuild 和 host-cmn 版本；
- QCOM ICNSS2/PLD 接口对照。

但它们常见的是：

- 不同 SoC 的 Adrastea/Helium/Lithium 组合；
- 不同 WPSS/WLAN 固件 ABI；
- 不同 CE target geometry；
- 甚至 PCIe/WCN7880 路线，而非本机 SM4450 SNOC。

因此，最合理的组合不是直接换成 SM8750 源码，而是：

```text
主基线：MiCode sky-t-oss 三件套
辅助参考：Sony/SM8450/SM8750 的现代 API/Kbuild 适配
硬件约束：本机原厂二进制、DT 和 12 管 shadow/CE 实测
```

## 6. 下一步建议

1. 暂停继续修 Sony LA.UM.7.3 与当前 host-cmn/fw-api 的混编；
2. 以 `micode-sky-t-oss` 的三个同提交目录建立新的隔离 worktree；
3. 先静态核对：
   - `qcacld-3.0/Kbuild`；
   - `qca-wifi-host-cmn/Kbuild`/HIF CE 对象；
   - `fw-api` 的 Adrastea/QMI/HTC ABI；
4. 用这三件套重新对 Linux 7.3 做第一轮 smoke build；
5. 记录它与当前 Sony 构建的 API 错误差异；
6. 再决定是否从 SM8450/SM8750 分支挑选单独的兼容补丁；
7. 仍然不刷机，直到源码构建和 icnss/PLD 胶水层闭环。

## 7. sky-t-oss Linux 7.3 首轮 smoke build

用户已确认 Redmi Note 12R 中国版与当前 sky 为同一机型变体，因此 `sky-t-oss` 现在确立为主基线，而不是仅候选。

已使用同一 commit 的 qcacld、host-cmn、fw-api 运行 Linux 7.3 ARM64 外置构建：

```text
KERNEL_SRC=/home/orange/redmi125g/sky-mainline/kernel/linux-7.3
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
CONFIG_ICNSS2=y
CONFIG_QMI_SUPPORT=y
CONFIG_PLD_SNOC_ICNSS_FLAG=y
CONFIG_WIFI_3_0_ADRASTEA=y
CONFIG_ADRASTEA_SHADOW_REGISTERS=y
CONFIG_ADRASTEA_RRI_ON_DDR=y
CONFIG_HIF_SNOC=y
```

构建日志：

```text
/tmp/micode-sky-q1-adapt6.log
```

已越过的阻塞：

- 外部 Kbuild 依赖路径；
- Git build-tag 跨仓库错误；
- `stdarg.h` 在 kernel-only include 环境中的缺失；
- QCOM 私有 `qcom-iommu-util.h` 头文件入口；
- 一部分 timer/QDF 兼容入口。

当前首个主要 API 阻塞已经进入 `cfg80211`：`struct cfg80211_ops` 在 mainline 7.3 中大量回调签名变化，包括 `wireless_dev`、key、station、tx power、remain-on-channel、mgmt_tx、antenna 和 channel 回调。这说明新 sky 三件套确实比旧 Sony 混编更完整：构建已经从依赖版本冲突推进到了现代 cfg80211 API 适配层。

本轮为构建隔离增加的临时适配仅在：

```text
sky-linux/third_party/micode-sky-t-oss/qcacld-3.0/
```

其中 `compat/` 和 Kbuild/Makefile 改动不是最终运行时方案，尤其 IOMMU 头仍只是入口占位，不能据此加载模块。

## 8. 正式结构迁移第一轮

已开始对 Linux 7.3 的 cfg80211 结构进行实际迁移，仍只在 sky-t-oss 本地树进行。

### 已完成的第一批改动

- 将 mainline 构建下的 `-Werror` 改为 `-Wno-error`，用于区分签名错误与真正语义错误；
- 增加 `CFG80211_KEY_INSTALL_SUPPORT_ON_WDEV`、`CFG80211_SINGLE_NETDEV_MULTI_LINK_SUPPORT` 和 BIGTK 结构选择，使 qcacld 已有的较新 wrapper 分支生效；
- 修正随机 AP MAC 地址生成：mainline 的 `net_device::dev_addr` 为只读指针，改用局部 `random_addr[]` 后通过 `eth_hw_addr_set()` 写入；
- 将 `cfg80211_ch_switch_notify()` 包装调用补齐 mainline 的 `link_id` 参数；
- 在 qcacld-local compat 中补充 stdarg、timer、string、hex 和 QCOM IOMMU 入口适配。

### 构建效果

日志：

```text
/tmp/micode-sky-cfg4.log
```

`wlan_hdd_cfg80211.c` 的 `.cfg80211_ops` 函数指针不匹配错误已经消失，说明第一轮结构迁移实际生效。构建继续进入：

- QDF lock/timer/hrtimer API；
- DP/NAPI 私有字段依赖；
- `strlcpy` 和旧 kernel helper；
- 这些问题不是 cfg80211 ops 结构本身。

目前仍未生成 `.ko`，也没有设备加载动作。

### 当前下一层阻塞

1. `in_irq()` 在 mainline 7.3 中不再作为旧接口直接提供；
2. timer compat 头未被所有 host-cmn include 路径采用，需要改为明确的 QDF-local include/包装；
3. `struct napi_struct` 的 `rx_count`/`rx_list` 和 `napi_gro_flush()` 属于 Android/旧内核私有实现，不能简单宏替换，需重写 DP 兼容路径；
4. hrtimer 旧 API 需要针对当前 mainline 头文件重新确认；
5. cfg80211 仍需做运行语义审查，当前仅完成函数签名/结构编译适配，不能宣称功能完成。

## 9. QDF/NAPI 下一轮构建进展

本轮继续在本地 sky-t-oss 三件套中推进 mainline 7.3 构建，日志以 `/tmp/micode-sky-next10.log` 为准；仍未生成 `.ko`、未触碰设备。

已解决或绕过的构建入口：

- QDF timer 旧 `from_timer`/`del_timer` 接口与 7.3 `timer_delete` 的名称差异；
- QDF IRQ 状态调用 `in_irq()` 改为 `in_hardirq()`；
- QDF hrtimer 头的缺失与 `hrtimer_setup()` 入口；
- NAPI 路线不再访问 Android 私有的 `napi_struct.rx_count/rx_list`，改用旧分支的 GRO flush 路径；
- `netif_rx_ni()` 替换为 `netif_rx()`，`txq_trans_update()` 增加 `dev` 参数；
- `PDE_DATA()` 改为 `pde_data()`；
- logging thread 不再使用删除的 `complete_and_exit()`；
- `init_dummy_netdev()` 使用本地兼容入口。

注意：上述部分兼容实现仍仅用于向前推进源码编译，特别是 `hrtimer_callback_running` 和 dummy netdev 的临时实现需在可加载模块前重新审查真实运行语义；不能把编译推进当成功能验证。

最新首个阻塞已移动到 QDF AES-CMAC：`qdf_crypto.c` 调用旧 `aes_encrypt(struct crypto_aes_ctx *)`，而 Linux 7.3 的 `aes_encrypt()` 接收 `aes_encrypt_arg`（`aes_enckey`/`aes_key`）。需按新 `aes_prepareenckey()`、`aes_encrypt()` API 重写 CMAC 的 key schedule，不能做指针强转。

## 10. Linux 7.3 AES/连接参数适配

本轮完成 QDF AES-CMAC 的条件化迁移：mainline 构建调用 `aes_prepareenckey()`，保存 `struct aes_enckey`，并将该对象传给 `aes_encrypt()`；原 Android 分支仍使用 `aes_expandkey()` 和 `struct crypto_aes_ctx`。目标 `Module.symvers` 已确认新 AES 函数均导出，`qdf_crypto.o` 已通过编译。**尚未运行 CMAC 已知答案测试，也未完成 modpost。**

同时推进了新版 `skb_frag_ref()` 头文件、`cfg80211_connect_params` 已移除的 WEP sequence 字段、QDF `__free` 命名冲突、CDS CPU mask 输出与线程完成通知等局部 API 迁移。旧 WEP sequence 在 Linux 7.3 的连接参数中不可用，该条件分支只复制其仍提供的 WEP key；需验证连接行为。修正了临时 hrtimer 假返回，改用内核 `hrtimer_bases.h` 提供的真实状态查询；dummy netdev 适配补上 `__LINK_STATE_PRESENT/START`。这些适配仍不代表运行时正确性。

复现构建：使用第 7 节的 ARM64 工具链和 Adrastea 配置，从 `qcacld-3.0` 运行 `make -j4`。本轮将 pktlog 改为带目录路径的 `register_sysctl_sz()` 扁平表注册，handler 使用 const 表项的局部副本；迁移了 `pde_data()`、`DMA_FROM_DEVICE` 和 HIF `netif_napi_add_weight()`，保留原 NAPI budget。最新日志 `/tmp/micode-sky-aes-api-8.log`，退出码 2，已进入 `core/pld/src/pld_common.c`、`pld_snoc.c`：缺少 Android 私有 `soc/qcom/icnss2.h`。这是 SNOC/PLD 平台胶水层的硬边界，涉及设备生命周期、IRQ 和固件交互，不应以空头文件或空桩绕过。构建中的 `-Wno-error` 仍可能遮蔽重要的回调类型警告，现阶段严禁装载模块。

## 11. sky-t-oss ICNSS2 平台迁移审计（Linux 7.3）

配套来源固定为 MiCode `Xiaomi_Kernel_OpenSource:sky-t-oss` commit `9adb4dca796fbc465bca5731feed21641f87bd2b`，公共 ABI 和原厂 `drivers/soc/qcom/icnss2/{main,qmi,power,debug,genl}.*` 暂存于 `sky-linux/third_party/micode-sky-icnss2/`，原厂代码未伪造接口实现。qcacld mainline 构建只追加该目录的 include 路径，并未将原厂 ICNSS2 本体编进 Linux 7.3。

使用第 7 节配置复现 `/tmp/micode-sky-icnss-api-1.log`：全部 qcacld 编译对象链接为 `wlan.o`，`MODPOST` 失败；`aarch64-linux-gnu-nm -u wlan.o` 显示 28 个未定义的 `icnss_*` / `__icnss_register_driver` 符号（注册/卸载、CE IRQ、供电、SOC 信息、QMI 初始化/关闭、SMMU 映射、恢复/idle、诊断等）。目标内核 `Module.symvers` 导出 QMI 和 SMEM 基础 API，但没有 ICNSS2 平台 ABI。**wlan.o 不是可加载模块，不存在可用 wlan.ko。**

独立编译原厂 ICNSS2 的探测日志 `/tmp/micode-icnss2-port-1.log` 在 `main.c`/`main.h` 的 `linux/adc-tm-clients.h` 缺失处停止；同一源码还引用 `linux/ipc_logging.h`、`soc/qcom/{memory_dump,secure_buffer,qcom_ramdump,service-locator,service-notifier,of_common}.h` 等下游设施。原厂 Kconfig 依赖 CNSS_UTILS 和 CNSS_QMI_SVC；需按 Adrastea 路径重建资源、PDR/SSR、WLFW QMI、MSA、CE IRQ、固件时序和错误恢复，不能将头文件或空桩算作平台移植。

**纠正第 8 节的过度结论：** `/tmp/micode-sky-icnss-api-1.log` 含至少 22 处 `cfg80211_ops` 函数指针类型不兼容警告，其中 key `link_id`、station `wireless_dev`、management TX/ROC cookie 是运行时危险的 ABI 错配。此前 `-Wno-error=incompatible-pointer-types` 将其降为警告；不能认为 cfg80211 回调结构已完成迁移。应在平台实现完成前恢复该告警为错误并迁移带类型签名的适配器。

当前完成的是**原厂 ICNSS2 ABI 的固定基线接入和缺失符号定位**，并非 ICNSS2 平台运行实现。PLD/SNOC 另修复了 `icnss_wlan_enable_cfg` 原先未初始化的问题：零初始化配置，并按 PLD 配置显式传入 shadow v2 和 RRI 字段。`/tmp/micode-sky-icnss-api-2.log` 验证 `pld_snoc.o` 编译通过，仍在同样的 28 个 ICNSS2 未解析符号处失败。原厂 `qmi.c` 的 `icnss_send_wlan_enable_to_fw()` 对 Adrastea 只消费 shadow v1 而不是 v2；v2/RRI 映射不代表 Adrastea 固件会使用它们。下一阶段须逐项迁移原厂 Adrastea 平台实现及其依赖，在 `Module.symvers` 有真实导出且回调类型全部修复后再谈模块加载，禁止设备试运行。

## 12. PLD/SNOC 固件配置契约复核

从 MiCode 原厂 `icnss2/qmi.c:3153-3250` 可核实：`icnss_wlan_enable()` 要求固件已 READY；先发送包含 CE target/service 和 shadow 的 WLFW CFG 请求，再发送 WLAN MODE 请求。Adrastea 使用 shadow v1，WCN6750 使用 shadow v2；`hif/src/ce/ce_main.c:514-529` 的 Adrastea v1 配置确认为源 CE 0/3/4/5/7 在 `0x3c`、目的 CE 1/2/7/8/9/10/11 在 `0x40`。不能将另一 SoC 的 v2 路径作为 sky 的目标。

`pld_snoc_wlan_enable()` 现检查设备、配置、host_version 和非零数组的指针，并将 `PLD_COLDBOOT_CALIBRATION` 映射到原厂 `ICNSS_CALIBRATION`、`PLD_WALTEST` 映射到 `ICNSS_WALTEST`；没有原厂对应模式的 `PLD_FTM_COLDBOOT_CALIBRATION` 等返回 `-EOPNOTSUPP`，不再静默变成 MISSION。`/tmp/micode-sky-icnss-api-3.log` 确认 `pld_snoc.o` 编译、`wlan.o` 链接，随后仍因未提供 ICNSS2 平台实现而在 modpost 失败。暂未做固件和设备运行验证。

## 13. PLD/SNOC 生命周期回调审计

`pld_snoc.c` 的设备回调已增加防御性迁移：idle restart/shutdown 现在同时检查全局 context、ops 和对应 callback；probe 在 `pld_add_dev()` 后调用平台 probe，若 probe 失败会回滚 `pld_del_dev()`，避免设备节点泄漏；wlan enable 的配置与模式校验仍保留。`/tmp/micode-sky-icnss-api-4.log` 确认 `pld_snoc.o` 编译成功，最终阻塞仍是 ICNSS2 未解析符号。

原厂 ICNSS2 注册契约要求 `probe` 和 `remove` 非空，且 `icnss_wlan_enable()` 要求 `FW_READY`；因此 PLD 不能在缺少真实 ICNSS2 probe/QMI 状态机时用空实现替代。剩余硬依赖包括 CNSS_UTILS/CNSS_QMI_SVC、下游 WLFW QMI 描述、供电/时钟、SMMU/IOMMU、CE IRQ、PDR/SSR、MSA/固件资源和完整 cfg80211 回调类型迁移。

## 14. SNOC 异步回调空指针路径

继续核对 ICNSS2 回调和 `pld_unregister_driver()` 的反注册顺序后，PLD/SNOC 的 remove、SSR reinit/shutdown/crash、PM suspend/resume/noirq 和 uevent 增加 context/ops 判空。remove 即使上层 remove callback 缺失，也仍从 PLD 设备链表删除节点；必需的 PM 回调缺失时返回 `-ENODEV`，不伪造成功。`/tmp/micode-sky-icnss-api-5.log` 证明 `pld_snoc.o` 编译、modpost 仍因真实 ICNSS2 符号未提供而失败。判空不证明注销并发完全安全；后续仍需审查 ICNSS2 callback 序列化及目标设备实测，尤其与 `osif_psoc_sync` 的生命周期协调。

## 15. PLD/ICNSS2 数组 ABI 与阶段验收

由于 `pld_snoc_wlan_enable()` 将 CE target/service 和 shadow v1/v2 数组指针强转给原厂 ICNSS2，现为四组源/目标结构添加大小断言，并为 CE `reserved`/`pipe_num` 及 shadow v1 `reg_offset` 加偏移断言。固定版本原厂头文件下 `/tmp/micode-sky-icnss-api-7.log` 证明 `pld_snoc.o` 无本文件警告地通过编译、`wlan.o` 成功链接，modpost 仍明确缺少 ICNSS2；代码与平台 ABI 的编译期迁移可以复现，不是固件运行验收。

本阶段已核对且落地的源码范围：固定 MiCode `sky-t-oss` 原厂 ICNSS2 头文件/实现参考；对接 Linux 7.3 外置 qcacld 的 include；PLD/SNOC 固件配置初始化、mode 映射与无效数据拒绝、probe 回滚、SSR/PM 回调防护及结构布局断言。**不包含原厂 ICNSS2 平台驱动在 Linux 7.3 上的移植**：它仍需补足 CNSS_UTILS/CNSS_QMI_SVC 和 WLFW 协议描述、供电/时钟、CE IRQ、SMMU/IOMMU、MSA/PDR/SSR、DT 资源及安全回收；现有 cfg80211 函数指针错配还须独立修复。没有可加载 `wlan.ko`，不得设备试运行。

## 16. ICNSS2 原厂平台实现实际迁移（首阶段）

用户要求开始平台本体移植，已从 MiCode 内核 `9adb4dca796fbc465bca5731feed21641f87bd2b` 固定提取 `drivers/net/wireless/cnss_utils` 下的 CNSS_UTILS、WLFW/DMS QMI 消息编码源和 `include/net/cnss_utils.h`，以及 ICNSS2 编译需要的原厂下游 API 头文件，集中放于 `third_party/micode-sky-icnss2`。这些下游头文件仅供识别接口，**不代表 Linux 7.3 有相应实现**。新增 `port.mk` 以 `CONFIG_ICNSS2_QMI=y` 独立交叉编译，防止命中原厂 `!CONFIG_ICNSS2_QMI` 的零返回桩。

`make -f port.mk dependencies` 在 Linux 7.3 ARM64 生成了原厂 `cnss_utils.ko` 与 `wlan_firmware_service.ko`，合并导出写入本目录 `Module.symvers`（日志 `/tmp/micode-icnss2-deps-2.log`，退出 0）。ICNSS2 本体的原厂 `qmi.o` 经 `strscpy` 和 `kernel_connect(sockaddr_unsized)` API 迁移后编译成功（`/tmp/micode-icnss2-qmi-2.log`，退出 0，仍有格式/原型警告）；`main.c` 已按 Linux 7.3 改用 `class_create(name)`、IDA range/free、thermal cooling ID、`iommu_map(..., GFP_KERNEL)`、`strscpy`、新版 timer API、`void` platform remove 和成对非 devm sysfs group；`genl.c` 改用 `strscpy`。完整 `make -f port.mk platform` 仍失败于下游 `qcom_register_early_ssr_notifier`/`qcom_unregister_early_ssr_notifier` 与 Android remoteproc hook 注册/注销（`/tmp/micode-icnss2-port-5.log`）；没有用空实现或随意替换 recovery 时序。

此后还须验证/迁移所用下游 IPC logging、ADC TM、内存转储、service locator/notifier 与 PDR 的**真实可链接符号**，确认 DT 供电/SMMU/CE IRQ/MSA 资源和完整固件启动顺序，完成 ICNSS2 的 modpost 与 qcacld 对接，并修复 cfg80211 回调类型问题。生成的两个依赖 `.ko` 仅证明它们各自能构建，不应装入设备；ICNSS2/qcacld 仍没有可加载模块。

## 17. Adrastea 源码级联编状态与阻断（续）

`/tmp/micode-icnss2-port-9.log`：原厂 ICNSS2 的 main/debug/power/genl/qmi 构成 `icnss2.ko` 并通过 Linux 7.3 ARM64 modpost；`/tmp/micode-sky-icnss-linked-1.log`：qcacld 对真实 ICNSS2 `Module.symvers` 联编，得到 `wlan.ko`，其 `modinfo depends` 为 `icnss2`；`icnss2.ko` 依赖原厂 `wlan_firmware_service.ko`。`wlan` 的 28 个 ICNSS 未定义引用已由真实平台模块解析；仅编译链接，未安装或加载。现可从 `third_party/micode-sky-icnss2` 先执行 `make -f port.mk dependencies`，再执行 `make -f port.mk wlan` 复现。`cnss_utils.ko` 虽构建成功，当前 `icnss2.ko` 的模块依赖不含它，不把它虚报为必装依赖。

**重要纠正**：sky 原厂启用的 Adrastea `qcom,icnss` 节点有 `wpss-support-enable`，不能把 early-SSR 视为非目标路径。Linux 7.3 仅有普通 WPSS SSR notifier，故原厂 early 回调的 `FW_DOWN`/忽略 QMI timeout 操作合并到普通 `BEFORE_SHUTDOWN` 回调开头，保持真实通知但时序不等价。Android remoteproc recovery-set hook 仅属于 WCN6750/WCN6450 分支；当前外置构建的 OF 匹配表只保留 `qcom,icnss`，以免将未经迁移的 WCN 路径标成可用。probe 现在检查 WPSS notifier 注册错误，并在注册之前初始化恢复计时器；remove 清除全局 `penv`。目标内核 `socinfo.c` 同样从 SMEM item 137 读取序列号，故以该已导出的 SMEM API 替代缺失的旧 `socinfo_get_serial_number`；缺失或尺寸不足时返回 0，让 qcacld 的 MAC 回退路径失败。IIO consumer 命名空间已显式导入。

**上机前阻断仍存在**：Linux 7.3 目标 DTS 树搜索不到启用的 sky `qcom,icnss` 设备节点；尚需审查/修复原厂 probe 失败时 ramdump 与 SSR 资源回滚、remove 的 QMI/异步回调和工作队列/计时器生命周期、WPSS 恢复时序，以及此前被 `-Wno-error` 隐藏的 cfg80211 回调类型不匹配。源码 modpost 不证明硬件资源、固件握手和运行正确性。此前本报告第 15/16 节记载的是前一阶段的状态，不再代表当前可链接性。

## 18. ramdump 失败回滚与目标 DT 现状

增量联编 `/tmp/micode-icnss2-wlan-3.log` 退出 0，重新编译 `main.o` 并产出 `icnss2.ko`；qcacld `wlan.ko` 的 28 个 `icnss_*` 导入仍由平台符号满足。现在 `msa0` ramdump 创建设备失败、WPSS SSR notifier 注册失败时可释放已取得的 class/minor/设备资源；remove 对 recovery-disabled、从未分配 ramdump class 的路径不再无条件 `class_destroy(NULL)`。本次只处理确认过的 Adrastea 创建顺序；尚不能保证其他可选 WCN 分支及所有 probe/卸载竞态安全。目标 `sm4450.dtsi` / `sm4450-qrd.dts` 未定义 sky 库存 `qcom,icnss`、WPSS 及其资源，因此源码具备真实 OF alias 和可链接符号，但**Linux 7.3 当前 DTS 没有可实际触发 probe 的设备路径**。不得据此加载模块或编造未经核对的供电/IRQ/phandle。

## 19. 目标 sky 板级 DTS 的 ICNSS2 设备路径

此前第 17/18 节把“通用 `sm4450.dtsi` / `sm4450-qrd.dts` 缺少设备节点”误写成“目标 sky DTS 也没有 WPSS”。用户指明的**目标文件**是 `sky-linux/dts/sm4450-xiaomi-sky.dts`；它已有 WPSS remoteproc、SMP2P、SMMU 和 12 条 CE IRQ，但 WLAN 节点原为 ath11k 的 `qcom,wcn6750-wifi`，MSA 只有 3 MiB。本轮按库存 overlay 对照把同一地址节点转换为 `qcom,icnss`，保留 8 MiB membase/12 IRQ/stream 0x4c0，设置 `qcom,rproc-handle`、12 MiB `qcom,wlan-msa-fixed-region`、BDF/固件前缀/WPSS 支持与 SMMU addr-pool；复用已有 WLAN SMP2P phandle，加入原厂驱动按名字寻找的 force-fatal/early-crash 子节点。没有同时启用 ath11k 兼容节点。

针对该目标文件执行 Linux 7.3 的 `cpp` + `dtc`，`/tmp/sky-icnss-adrastea-round3.dtb` 离线生成成功；反编译确认 OF compatible、12 条 CE IRQ、MSA `0xc00000`、WPSS 与 SMP2P phandle 及 IOMMU 属性存在；`dtc` 仅有两个与 WLAN 无关的既存 graph 警告。此为**可静态验证的板级设备匹配路径**，不代表固件可启动或模块可安全加载。仍待确认库存 ch1 regulator 映射、现有四条供电 rail 的实际板级连接、WPSS early-SSR 差异、cfg80211 ABI 和卸载竞态。`tools/mksky_dts.py` 仍会生成旧 WCN/ath11k 节点，不能运行再生覆盖本次目标 DTS，需后续同步生成器。

## 20. 生成器输入定位与固定 MSA 映射释放

经用户指明，ROCKNIX 原板文件实际在 `sky-mainline/rocknix/dts/sm4450-anbernic-rg55g1.dts`，其解析辅助模块位于 `sky-mainline/tools/dts_blocks.py`。修正 `tools/mksky_dts.py` 的输入和导入路径，并为已有 `qcom,icnss` 的默认目标文件加显式防覆盖门禁：`/tmp/sky-icnss-generator-guard-2.log` 提示拒绝覆盖，执行前后目标 SHA256 一致。通过 `SKY_DTS_OUTPUT=/tmp/sky-icnss-generator-round4.dts` 可以隔离生成作对比；与目标 DTS 的 diff 为 223 行，差异涵盖 WLAN 之外的固件路径、SPI 和面板电源，因此没有自动覆盖任何当前板级改动。仅修 WLAN 生成规则不足以安全恢复整份 DTS 的可再生性。

ICNSS2 固定 MSA 路径调用 `memremap()` 后原本没有 `memunmap()`，已在固定映射成功后注册 `devm_add_action_or_reset()` 回收动作；动态 `dmam_alloc_coherent()` 路径不经该动作。`/tmp/micode-icnss2-wlan-round4.log` 证明平台与 qcacld 增量联编退出 0。后续源码审计又修复了固定 MSA 回收、QMI lookup 失败释放、sysfs 创建回滚、DMS 先于 WLFW lookup 初始化、SSR/timer/workqueue 注销顺序，以及全局 WPSS loader 的 remove 取消；`/tmp/micode-icnss2-wlan-round5e.log` 的平台与 qcacld 联编再次退出 0。本轮又修复了 WPSS `rproc_boot()` 失败后的悬空指针，并在 remove 取消 WPSS work 后释放保留的 rproc 引用；`/tmp/micode-icnss2-wlan-round6.log` 联编退出 0。本轮按用户要求只处理驱动和联编：本地 `wifi-backup/firmware/image/adrastea/` 已确认有 `wpss.mdt/.mbn/.b00-.b10` 及多区域 board-data 文件，但没有进行固件安装或设备测试。qcacld monitor-channel 回调已补上 Linux 7.3 所需的 `struct net_device *`；其余 cfg80211 回调仍有大量独立代际签名警告，继续由 Kbuild 的 `-Wno-error` 隐藏，不在本轮扩展。`/tmp/micode-icnss2-wlan-round8-final.log` 退出 0；`modinfo` 显示 `icnss2` 依赖 `wlan_firmware_service`、`wlan` 依赖 `icnss2`，ICNSS2 alias 为 `qcom,icnss`。仍无设备端加载/恢复验证；ch1 rail、通知时序、全局工作项并发和模块卸载的真实硬件行为留给后续测试 prompt。

## 21. 最终排序

```text
1. MiCode/vendor_qcom_opensource_wlan:sky-t-oss
   —— 当前最适合，建议作为主基线

2. Sony vendor-qcom-opensource-wlan-qcacld-3.0 的较新 LA.UM.9.x/LA.VENDOR 分支
   —— 作为 API/Kbuild 对照，需逐分支核对

3. OnePlus-12-Development lineage-21
   —— 较新 Android 参考，但平台匹配未知

4. arter97-mirror/caf_qcacld-3.0
   —— CAF 历史版本数据库，不作为设备基线

5. TheNotOnly/sdm845-caf-qcacld-3.0 / metacrito fork
   —— 仅历史或构建参考
```

## 22. 当天收工状态（严格类型清理完成后）

day 末在严格模式下（`KCFLAGS=-Werror=incompatible-pointer-types -Werror=cast-function-type
-Werror=discarded-qualifiers -Werror=enum-int-mismatch -Werror=implicit-fallthrough`）
复验，结论固定为：

- `cast-function-type` / `discarded-qualifiers` / `enum-int-mismatch` / `implicit-fallthrough`
  四类已清零（tasklet 上下文 wrapper、scheduler 独立 `timer_callback` 字段、`dev_addr_set()`、
  enum 原型统一、`fallthrough;`）；
- 仅剩 **17 条 `cfg80211_ops` 回调签名**，全部位于 `wlan_hdd_cfg80211.c:24621–24713`；
- 复现日志 `/tmp/qcacld-strict-round10.log`（退出 2），与 round9r 行号一致；
- **默认构建同样退出 2**（`/tmp/qcacld-default-round10.log`）：内核 `KBUILD_CFLAGS` 自带
  `-Werror=incompatible-pointer-types`，qcacld 的通用 `-Wno-error` 覆盖不了专用 `-Werror=X`，
  只有 `-Wno-error=incompatible-pointer-types` 能降级。故当前**无可产出 `wlan.ko`**，
  18:45 的 `wlan.ko` 属旧旗标时期产物。

状态汇总与下一步统一收在 `STATUS-qcacld-mainline-2026-11-03.md`，
本文件 §7–§21 保持为当日轮次的原始流水。
