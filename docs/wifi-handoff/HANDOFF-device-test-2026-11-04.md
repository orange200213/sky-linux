# 上机测试交接：sky / Adrastea ICNSS2 + qcacld Linux 7.3

## 1. 测试目标

验证当前离线构建成果在 Redmi 12 5G / sky（SM4450、WCN6750/Adrastea）上的最小驱动启动、模块依赖、固件通信、Wi-Fi 接口创建、扫描/连接，以及 SSR/WPSS 恢复路径。

本交接只包含测试所需成果，不包含源码。测试由专用上机测试 prompt 执行；本会话不执行安装、加载、刷机或设备操作。

## 2. 成果包

压缩包：`artifacts/sky-wifi-test-artifacts-2026-11-04.tar.gz`

包内包含：

- `modules/wlan.ko`：qcacld 主模块
- `modules/icnss2.ko`：Adrastea ICNSS2 平台模块
- `metadata/Module.symvers.icnss2`：ICNSS2 构建符号记录，仅供核对
- `metadata/modinfo-wlan.txt`、`metadata/modinfo-icnss2.txt`
- `metadata/sha256sums.txt`
- `logs/qcacld-semantic-round4.log`：运行语义审查后的严格构建日志
- `logs/qcacld-cfg80211-round16.log`：cfg80211 严格构建日志
- `logs/qcacld-default-round17.log`：默认构建日志
- `handoff/HANDOFF-device-test-2026-11-04.md`：本文档
- `handoff/HANDOFF-cfg80211-api-linux73.md`：cfg80211 适配说明
- `handoff/STATUS-qcacld-mainline-2026-11-03.md`：当前状态记录
- `handoff/model-handoff-2026-11-03-HANDOFF.md`：模型交接摘要
- `handoff/TEST-CHECKLIST.md`：上机测试检查表

## 3. 构建证据

`/tmp/qcacld-semantic-round4.log`：

```text
BUILD_EXIT=0
LD [M] wlan.o
MODPOST Module.symvers
LD [M] wlan.ko
```

当前 `wlan.ko` 的构建依赖为 `icnss2`。模块没有被安装或加载过，产物只代表源码编译和链接成功。

## 4. 测试前硬性条件

1. 测试设备型号必须是 sky / Redmi 12 5G 或兼容的 SM4450 + WCN6750/Adrastea 平台。
2. 测试内核必须与模块编译所用 Linux 7.3 内核配置、符号和 ABI 一致。
3. 必须准备匹配版本的 Adrastea/WCN6750 固件及其许可文件；成果包不包含固件。
4. 测试前保存原有 wlan/icnss2 模块和相关固件状态，确保可回滚。
5. 先执行最小启动测试，再执行 Wi-Fi 功能测试；不要同时修改多个变量。

## 5. 推荐测试阶段

### A. 静态核对

- 校验 `metadata/sha256sums.txt`。
- 核对 `modinfo wlan.ko` 的依赖、vermagic、架构。
- 核对 `modinfo icnss2.ko` 与目标内核匹配。
- 确认当前运行内核、设备树、固件版本和平台型号。

### B. 最小加载

- 先加载 `icnss2.ko`，再加载 `wlan.ko`，或使用系统依赖解析方式。
- 记录 `dmesg`、模块状态、符号解析错误、QMI/remoteproc/PLD/SNOC 初始化结果。
- 不要在出现明显 probe/SSR 错误后反复强制重载。

### C. 基本功能

- 确认 wlan 网卡/wiphy 创建。
- 执行一次扫描并记录结果。
- 执行一次 WPA2/WPA3 连接和断开。
- 若设备支持 P2P，再测试一次 ROC、P2P listen 或简单 action-frame 路径。
- 记录 cfg80211、wlan、icnss2、WPSS、QMI、firmware 相关日志。

### D. SSR/WPSS 恢复

只在 A-C 稳定后进行，并确保有串口/ADB/恢复手段：

- 触发一次受控 WPSS/firmware SSR。
- 记录 SSR 前后 dmesg、模块状态、wiphy/网卡是否恢复。
- 检查 `ICNSS_FW_READY`、QMI client 重建、PLD/SNOC 状态和 Wi-Fi 重连。
- 不要把一次恢复成功解释为压力稳定性通过。

### E. cfg80211 重点观察

- ROC ready/expired 的 cookie 是否与请求一致。
- mgmt TX status 的 cookie 是否与请求一致，失败是否正确返回。
- change_beacon 失败后旧 beacon 是否保留。
- 非零 link/radio 请求是否被拒绝，而不是落到 link/radio 0。
- control-port TX 的 cookie/status 是否完整；如无 status，必须记录。
- key 安装、重连、删除和默认 beacon/mgmt key 行为；这些路径仍有已知历史实现风险。

## 6. 回传资料

请至少回传：

- 设备型号、内核 commit/config、设备树版本
- 包内两个 `.ko` 的 sha256
- `modinfo` 输出
- 模块加载顺序和完整命令
- 加载前后 `dmesg`（至少覆盖 ICNSS2、WPSS、QMI、PLD、SNOC、wlan）
- `ip link` / `iw dev` / `iw phy` 输出
- 扫描、连接、断开结果
- ROC/mgmt TX/P2P 结果（若执行）
- SSR 前后完整日志和恢复结论
- 任何 kernel warning、oops、UAF、hung task、firmware crash

请保留失败现场，不要先卸载或清理日志。

## 7. 已知边界

- 未进行设备端测试。
- 成果包不含源码和固件。
- key 真实读取/删除/default management/beacon key、control-port status、异步 cancel 生命周期仍需通过代码后续专项处理或在测试中重点观察。
- 不得将模块成功加载等同于完整 Wi-Fi 运行语义已经验证。
