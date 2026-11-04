# Model handoff：Adrastea ICNSS2 + qcacld Linux 7.3

## 当前结论

目标是 Redmi 12 5G / sky（SM4450，WCN6750/Adrastea）驱动迁移。平台采用真实 MiCode ICNSS2、PLD/SNOC 与 qcacld 链路；没有使用 fake QMI、fake remoteproc、fake notifier，也没有进行模块安装、加载、刷机或设备测试。

## 最新构建证据

- qcacld 严格 cfg80211 构建：`/tmp/qcacld-cfg80211-round16.log`，退出码 0。
- qcacld 默认构建：`/tmp/qcacld-default-round17.log`，退出码 0。
- 两份日志均完成：`LD [M] wlan.o`、`MODPOST Module.symvers`、`LD [M] wlan.ko`。
- `modinfo -F depends wlan.ko` 返回 `icnss2`。
- round10/11/12/13/14/15 的失败日志保留在 `/tmp/`，用于历史追踪；当前 round16 是最新源码证据。

## 本轮关键改动

### cfg80211 ABI

`core/hdd/src/wlan_hdd_cfg80211.c` 已增加 Linux 7.3 adapter：

- station callbacks 与 `change_station`：接收 `wireless_dev`，判空并通过 `wdev->netdev` 转发。
- `change_beacon`：接收 `struct cfg80211_ap_update`，传递 `&update->beacon`，没有结构体强转。
- default key/default beacon key：校验单 link `link_id == 0`。
- set/get TX power 与 wiphy params：校验单 radio/index；复用旧 netdev/wdev 实现。
- antenna：适配 Linux 7.3 的 `radio_idx`，只接受 0。
- ROC、mgmt TX、control-port：ops 签名改为 value cookie；旧内部出参接口用局部值地址适配，不能把 value cookie 解引用。
- TDLS management：增加 Linux 7.3 link_id wrapper，拒绝非零 link。

`core/hdd/src/wlan_hdd_p2p.c` 和 `core/hdd/inc/wlan_hdd_p2p.h` 的 ROC/mgmt TX 签名同步为 value cookie。

### 既有严格类型修复

- QDF tasklet 使用 wrapper/trampoline，不再强转函数指针。
- scheduler timer 使用独立 timer callback 字段。
- const、enum/int、fallthrough 等严格告警已清理。
- ICNSS2 WPSS/modem SSR 空通知数据校验、FW_READY 清理与生命周期回滚已落地。

## 仍需注意

1. 构建通过不等于运行正确。cfg80211 station/key/mgmt cookie/ROC 语义尚未设备验证。
2. Linux mainline 的 `QCOM_SSR_BEFORE_SHUTDOWN` 不等价于厂商 early-SSR；文档中保留这一限制，不要伪造早期回调。
3. 全局 `wpss_ssr_work`/loader 仍有多实例并发风险，需要后续静态审查。
4. 不要使用旧时间戳的 `wlan.ko` 作为证据；必须以当前构建产物和日志为准。
5. 不要安装/加载模块、复制固件或进行设备测试，除非用户另行明确要求；当前任务是驱动源码和离线构建。

## 运行语义审查结果

已完成第一轮上机前静态审查，并修复明确问题：

- ROC/mgmt_tx 不再把内部 ID 作为 Linux 7.3 外部 cookie；外部 cookie 现在保存在 ROC/TX context，并用于 ready/expired/TX-status 通知。
- mgmt_tx/ROC 失败状态不再无条件返回成功；mgmt buffer 分配失败会回滚 IDR。
- key wdev wrapper 增加 `wdev/netdev` 与单链路 `link_id == 0` 校验；get_key callback 增加 NULL 检查。
- change_beacon 增加输入校验，并改为提交成功后替换 beacon；失败时恢复旧 beacon、释放新 beacon。
- 重新构建 `/tmp/qcacld-semantic-round4.log`：退出码 0，完成 `MODPOST`、`wlan.o` 和 `wlan.ko` 链接。

## 尚未解决、上机前仍需明确的风险

- key 的真实读取/删除/default management/beacon key 仍有历史实现缺口，不能宣称安全密钥路径已完整验证。
- control-port cookie 尚未发现完整的 TX-status 关联链路。
- ROC/mgmt_tx cancel 的异步 IDR 裸指针生命周期仍需专项锁/引用审查。
- auth mgmt_tx 特殊路径的 cfg80211 TX-status 通知仍需确认。
- 未进行设备、固件或运行时测试。

## 建议下一步

- 在解决上述 key/control-port/cancel 生命周期问题前，不应把结果描述为“可直接上机验证全部 Wi-Fi 功能”；可以进行受控的最小启动测试，但应重点观察这些路径。
- 不要安装/加载模块、复制固件或进行设备测试，除非用户明确要求。
