# Handoff：Adrastea/WPSS SSR 恢复适配到 Linux 7.3

状态：**已完成一轮源码修复；运行时 SSR 仍需设备侧验证**
范围：只修改 ICNSS2 Adrastea SSR/recovery、QMI/remoteproc/notifier 生命周期；不处理 cfg80211，不安装固件，不加载模块，不做设备测试。

## 1. 目标

把 MiCode sky-t-oss 原厂 Adrastea ICNSS2 的 WPSS SSR 语义迁移到 Linux 7.3 mainline 服务，同时明确 mainline notifier 与原厂 early-SSR 的不可等价部分。

## 2. 当前平台路径

目标 DTS：`sky-linux/dts/sm4450-xiaomi-sky.dts`。

当前静态路径已验证：

- WLAN compatible：`qcom,icnss`；
- 8 MiB membase；
- 12 条 CE level-high IRQ；
- fixed MSA `0x82a00000 + 0xc00000`；
- `qcom,rproc-handle` 指向 WPSS remoteproc；
- `wpss-support-enable`；
- WLAN SMP2P out state 及 force-fatal/early-crash input 子节点；
- IOMMU stream/pool/geometry 属性。

离线 `cpp` + `dtc` 产物：`/tmp/sky-icnss-adrastea-round3.dtb`（源文件更新后应重新生成，不可把旧产物当最新证据）。

## 3. 已完成的驱动侧修复

ICNSS2：`third_party/micode-sky-icnss2/drivers/soc/qcom/icnss2/main.c` / `qmi.c`。

已完成并通过源码联编：

- fixed MSA `memremap()` 的 device-managed `memunmap()`；
- QMI lookup 失败释放 handle；
- DMS QMI 先于 WLFW lookup 初始化；
- sysfs/debugfs/probe 失败回滚；
- SSR notifier、timer、event workqueue、WPSS loader 注销顺序；
- mainline WPSS notifier 拒绝空通知数据，并在 `AFTER_SHUTDOWN` 清除 stale `ICNSS_FW_READY`；
- modem SSR notifier 同样拒绝空通知数据；
- WPSS `rproc_boot()` 失败清空 stale pointer；
- remove 取消 WPSS work 后释放 rproc 引用；
- `wpss_self_recovery` 对空 `priv/rproc` 防护。

最近 ICNSS2 联编证据：`/tmp/ssr-adrastea-round9b-platform.log` 退出 0。严格 qcacld 类型探测已推进并持续暴露真实遗留问题；最新日志为 `/tmp/qcacld-strict-round9r.log`，其当前阻塞点是 cfg80211 Linux 7.3 ABI 回调签名不匹配，不能把该阶段称为严格通过。

模块依赖：

```text
icnss2.ko -> wlan_firmware_service
wlan.ko  -> icnss2
icnss2 alias -> qcom,icnss
```

## 4. 核心未解决问题

### 4.1 notifier 时序不等价

原厂有独立 early-SSR 回调；当前 Linux 7.3 端只有 WPSS `QCOM_SSR_BEFORE_SHUTDOWN` notifier chain。原厂 early `FW_DOWN` 和 QMI timeout flag 操作被放到普通 shutdown callback 开头，但这不证明时序等价。

需要核对：

- firmware-down 标志设置时刻；
- QMI transaction timeout 抑制/恢复时刻；
- qcacld 收到 SSR down/up 事件的先后；
- remoteproc shutdown/boot 与 WLFW server delete/new 的顺序；
- recovery timer 是否会在 SSR 已注销后重新排 work。

### 4.2 自恢复 work 与 remoteproc 状态

当前代码有 `wpss_ssr_work`、`wpss_loader` 和 recovery timers。源码已经做基本取消与 rproc 引用清理，但全局 work item 与多个实例/重复 SSR 的并发语义尚未在硬件上验证。

重点审计：

- `cancel_work_sync()` 与 notifier callback 的锁顺序；
- `rproc_shutdown()`/`rproc_boot()` 是否可能与 qcom_q6v5_pas 状态机重入；
- rproc boot 失败后是否需要再次 `rproc_get_by_phandle()`；
- `ICNSS_SSR_REGISTERED` 与 QMI `DEL_SERVER` 状态位的一致性；
- SSR 期间 event list 是否仍投递到已撤销资源。

### 4.3 DTS/固件边界

本地备份目录：`wifi-backup/firmware/image/adrastea/`。

已确认存在：`wpss.mdt`、`wpss.mbn`、`wpss.b00`–`wpss.b10` 和多个 `bdwlan*` board-data 文件。该检查不是安装/加载/运行验证。

DTS remoteproc firmware name 与 ICNSS2 `ADRASTEA_PATH_PREFIX`（`adrastea/`）之间需要由后续 prompt 做路径一致性审查；不要通过复制/重命名伪造固件。

## 5. 推荐修复顺序

1. 读取 `main.c` 的 WPSS notifier 注册、回调、timer/work 路径及 mainline `qcom_q6v5_pas` notifier 实现。
2. 画出原厂 early-SSR、当前 mainline BEFORE_SHUTDOWN、QMI server delete/new、remoteproc state 的事件时序。
3. 只修复能由 Linux 7.3 API/锁语义直接证明的问题；无法证明的保留为风险。
4. 对 SSR work/timer 使用明确的实例状态和取消顺序，避免全局 work 使用已释放 `priv`。
5. 在源码联编中验证 modpost、真实符号及错误路径；不得用假 notifier 或空实现。

## 6. 验收

```bash
make -f port.mk wlan > /tmp/ssr-adrastea-linux73.log 2>&1
```

要求：

- 退出码 0；
- `icnss2.ko` 通过 modpost；
- qcacld `wlan.ko` 继续由真实 ICNSS2 exports 链接；
- 不声称 SSR 已在设备上工作，除非另有明确的设备测试 prompt 和日志。

## 7. 禁止事项

- 不用空 notifier、伪 QMI 或假 remoteproc 代替真实实现；
- 不把 `BEFORE_SHUTDOWN` 直接宣称为 vendor early-SSR 等价物；
- 不安装/加载模块，不刷写，不复制固件到 rootfs；
- 不把静态 DTS 编译或模块联编当作 SSR runtime 验证；
- 不在此 handoff 中修改 cfg80211 API。
