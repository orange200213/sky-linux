# sky Wi-Fi 项目目录交接索引

工作区：`/home/orange/redmi125g/sky-linux`；实际 Linux 7.3 内核树在工作区外：`/home/orange/redmi125g/sky-mainline/kernel/linux-7.3`。

## 当前驱动工作路径

| 路径（相对 sky-linux） | 用途 |
| --- | --- |
| `third_party/micode-sky-icnss2/` | MiCode 平台驱动和构建入口 `port.mk`；含 `wlan_firmware_service.ko`、`cnss_utils.ko` |
| `third_party/micode-sky-icnss2/drivers/soc/qcom/icnss2/` | ICNSS2 实际源码 `main.c`/`main.h`、构建产物 `icnss2.ko` 和 `Module.symvers` |
| `third_party/micode-sky-t-oss/qcacld-3.0/` | 当前 Adrastea qcacld 源码、Kbuild、产物 `wlan.ko` |
| `third_party/micode-sky-t-oss/qcacld-3.0/core/hdd/` | Linux 7.3 cfg80211、P2P、hostapd 等适配 |
| `third_party/qca-wifi-host-cmn/` | qcacld 共享组件；qcacld 目录的 `cmn` 是指向它的符号链接 |
| `third_party/fw-api` | 指向 `/home/orange/redmi125g/wifi-backup/stock-wlan/qcom/opensource/wlan/fw-api` 的符号链接，不是随仓库独立复制的目录 |
| `third_party/qcacld-3.0-sony*` | 对比/参考树，不是本轮 sky-t-oss 模块构建来源 |

## DTS 与内核

- `dts/sm4450-xiaomi-sky.dts` 是符号链接，真实文件为 `/home/orange/redmi125g/sky-mainline/dts/sm4450-xiaomi-sky.dts`。当前实际 DTS 内 `wifi@22800000` 的 compatible 为 `qcom,icnss`；不要把它当成独立副本，也不要用 `tools/mksky_dts.py` 覆盖。
- `dts/README.md` 说明构建与手工 DTS 修改限制；`tools/dtsbuild.sh` 用于 DTS 构建。
- `/home/orange/redmi125g/sky-mainline/kernel/linux-7.3` 是目标内核源码及构建目录；`sky-linux/kernel/` 主要放配置、补丁和本项目驱动，不等于完整内核树。
- `shim/`、`initramfs/`、`rootfs/`、`dist/` 属于启动镜像/根文件系统等其他工作范围，不是当前 qcacld 驱动源码。刷机和镜像测试请依专门测试流程进行。

## 文档与测试成果

- `docs/wifi-handoff/FINDINGS-qcacld-device-test-2026-11-04.md`：最新设备测试记录。最后一次记录中 `wlan_firmware_service.ko` 与 `icnss2.ko` 可加载，但 ICNSS2 probe 报 `Fail to get IRQ-0`；`wlan.ko` 随后无法注册 SNOC。先从这一卡点继续，而非重做早期的未知符号分析。
- `docs/wifi-handoff/HANDOFF-device-test-2026-11-04.md`、`TEST-CHECKLIST.md`：早期上机测试流程及回传清单，必须结合上述最新测试记录阅读。
- `docs/wifi-handoff/HANDOFF-cfg80211-api-linux73.md`：cfg80211 API 与第一轮静态语义审查。
- `docs/wifi-handoff/HANDOFF-ssr-recovery-adrastea-linux73.md`、`HANDOFF-snoc-mainline.md`：SSR 和 SNOC 背景。
- `docs/wifi-handoff/model-handoff-2026-11-03/HANDOFF.md`、`STATUS-qcacld-mainline-2026-11-03.md`：较早构建状态；不可代替最新设备测试结果。
- `artifacts/sky-wifi-test-artifacts-2026-11-04.tar.gz`：早期不含源码测试包（SHA-256 `78a53ffdbf74a0fdf737c1a2ba59a1d353694fe574ccb21a7e1202a3e10a79f5`），含 `icnss2.ko` 和 `wlan.ko`，**不含**后续测试所需的 `wlan_firmware_service.ko`，也不含固件或当前更新的 DTS/DTB。不能单独视作新的完整上机包。

## 当前状态与交接注意

1. 最后设备测试结果卡在 ICNSS2 probe 的 `Fail to get IRQ-0`，不是 `wlan.ko` 编译或 cfg80211 ABI 错误。最新测试记录中的“缺 smp2p_map_wlan_2_in 导致 IRQ-0”假设已通过重测排除。
2. 最新测试记录末尾称“本机没有 ICNSS2 源码树”；**当前检查确认源码存在**于 `third_party/micode-sky-icnss2/drivers/soc/qcom/icnss2/`。应直接审查该源码的 IRQ 获取逻辑，并以实际 DT/日志核对。
3. `git status` 显示工作区有未提交改动与未跟踪的 `third_party/`、文档、`artifacts/`；DTS 也从常规文件变为符号链接。不要盲目清理、重生成 DTS、或将未提交状态视为已归档。
4. 已完成的离线构建只证明模块编译/链接；上机测试迄今未进入完整 cfg80211 扫描、连接、ROC/mgmt_tx 和 SSR 功能验证。
5. 下一位接手者应先读最新测试记录、检查 ICNSS2 IRQ 获取及实际 `qcom,icnss` 设备树资源，再决定是否重新构建和更新测试包。不要未经确认复用旧镜像或进行设备写入。
