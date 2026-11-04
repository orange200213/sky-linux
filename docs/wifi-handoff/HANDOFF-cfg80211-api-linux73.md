# Handoff：qcacld cfg80211 API 适配到 Linux 7.3

状态：**Linux 7.3 严格构建通过；已完成第一轮运行语义审查，仍有明确未决风险**
范围：只修改 qcacld HDD/cfg80211 适配层和必要构建条件；不修改 ICNSS2/QMI/WPSS，不加载模块，不做设备测试。

## 1. 目标

将 `third_party/micode-sky-t-oss/qcacld-3.0` 的 `wlan_hdd_cfg80211_ops` 回调逐项适配 Linux 7.3 的 `include/net/cfg80211.h`，消除函数指针签名不匹配。禁止用 `-Wno-error=incompatible-pointer-types` 作为最终修复。

## 2. 当前证据

目标内核：`/home/orange/redmi125g/sky-mainline/kernel/linux-7.3`。

qcacld 注册表：`third_party/micode-sky-t-oss/qcacld-3.0/core/hdd/src/wlan_hdd_cfg80211.c:24616`。

Linux 7.3 定义示例：`include/net/cfg80211.h:5304` 起的 key 回调，以及 `:5390` 的 `set_monitor_channel`。

当前 qcacld Kbuild 的状态（2026-11-03 已复核）：

- `Kbuild:26-28`：mainline 构建只加 `ccflags-y += -Wno-error`；
- `Kbuild:2993-3001`：mainline 分支不再加 `-Werror`，非 mainline 分支仍加；
- 原先逐类型的 `-Wno-error=incompatible-pointer-types` **已不存在**。

历史上曾出现：通用 `-Wno-error` 不能覆盖内核 `KBUILD_CFLAGS` 中专用的 `-Werror=X`。该问题已通过源码签名适配解决；当前默认构建 round17 与严格构建 round16 均可生成 `wlan.ko`。

```text
-Werror=incompatible-pointer-types -Wno-error                        -> error（仍然报错）
-Wno-error=incompatible-pointer-types                                -> warning（唯一有效写法）
```

历史证据：2026-11-03 的默认构建 `/tmp/qcacld-default-round10.log` 退出 2、报同样的 17 条；该记录不代表当前源码。当前验证为严格 round16 和默认 round17，均退出 0 并完成 `MODPOST`/`wlan.ko` 链接。

严格验证方式（不依赖修改 Kbuild，只在命令行注入）：

```bash
cd third_party/micode-sky-icnss2
make -C ../micode-sky-t-oss/qcacld-3.0 \
  KERNEL_SRC=/home/orange/redmi125g/sky-mainline/kernel/linux-7.3 \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  KBUILD_EXTRA='CONFIG_ICNSS2=y CONFIG_QMI_SUPPORT=y CONFIG_PLD_SNOC_ICNSS_FLAG=y \
    CONFIG_WIFI_3_0_ADRASTEA=y CONFIG_ADRASTEA_SHADOW_REGISTERS=y \
    CONFIG_ADRASTEA_RRI_ON_DDR=y CONFIG_HIF_SNOC=y' \
  KBUILD_EXTRA_SYMBOLS=$PWD/drivers/soc/qcom/icnss2/Module.symvers \
  KCFLAGS='-Werror=incompatible-pointer-types -Werror=cast-function-type \
    -Werror=discarded-qualifiers -Werror=enum-int-mismatch \
    -Werror=implicit-fallthrough' all
```

2026-11-03 复现结果：`/tmp/qcacld-strict-round10.log` 退出 2，**恰好 17 条错误，全部位于
`core/hdd/src/wlan_hdd_cfg80211.c`**，行号见 §4。其余 `cast-function-type`、`discarded-qualifiers`、
`enum-int-mismatch`、`implicit-fallthrough` 四类在当天已清零——这一层不再是卡点。

## 3. 已完成的适配

round16 严格构建已通过：`/tmp/qcacld-cfg80211-round16.log` 退出码 0，完成 `LD [M] wlan.o`、`MODPOST Module.symvers`、`LD [M] wlan.ko`；当前 `modinfo -F depends wlan.ko` 为 `icnss2`。本轮仍只做源码和离线构建，未安装/加载模块，未进行设备测试。

已完成的 Linux 7.3 adapters：

- station 四项与 `change_station`：从 `wireless_dev` 判空并通过 `wdev->netdev` 转发到既有 netdev 实现。
- `change_beacon`：接收 `struct cfg80211_ap_update`，只传递 `&update->beacon`。
- default key、default beacon key、TX power、wiphy params、antenna：校验 `link_id/radio_idx == 0` 后复用旧实现。
- ROC/mgmt TX/control-port：ops 层使用 Linux 7.3 的 value-cookie 签名；ROC/TX context 保存外部 cookie，异步 ready/expired/TX-status 使用外部 cookie；内部 ID 仅用于 IDR 查找。
- TDLS management：增加 Linux 7.3 `link_id` wrapper，并拒绝非零 link。

## 3a. 已完成的最小适配

`wlan_hdd_cfg80211_set_mon_ch()` 已增加 Linux 7.3 要求的：

```c
struct net_device *dev
```

改动位置：`core/hdd/src/wlan_hdd_cfg80211.c` 的 monitor-channel wrapper。该参数当前不需要内部逻辑，需使用 `(void)dev` 或等价方式避免 unused warning。

## 4. 历史签名差异（已在 round16 清零）

| 行 | 回调 | Linux 7.3 要求 | 现状 |
|---|---|---|---|
| 24621 | `change_station` | `(wiphy, wireless_dev *, …)` | `net_device *` |
| 24623 | `change_beacon` | `(wiphy, net_device *, struct cfg80211_ap_update *)` | `cfg80211_beacon_data *` |
| 24629 | `set_default_key` | `(wiphy, net_device *, int link_id, u8 idx, bool uni, bool multi)` | `(wiphy, wireless_dev *, u8, bool, bool)` |
| 24633 | `set_wiphy_params` | `(wiphy, int, u32)` | `(wiphy, u32)` |
| 24634 | `set_tx_power` | `(wiphy, wireless_dev *, int idx, enum setting, int dbm)` | 缺 `int idx` |
| 24635 | `get_tx_power` | `(wiphy, wireless_dev *, int idx, int *dbm)` | `(wiphy, wireless_dev *, int *)` |
| 24636 | `remain_on_channel` | cookie `u64` 值 + `const u8 *cookie` | cookie `u64 *` 指针 |
| 24638 | `mgmt_tx` | cookie `u64` 值 | cookie `u64 *` |
| 24643 | `set_default_beacon_key` | `(wiphy, wireless_dev *, int link_id, u8)` | `(wiphy, wireless_dev *, u8)` |
| 24646 | `dump_station` | `wireless_dev *` | `net_device *` |
| 24647 | `get_station` | `wireless_dev *` | `net_device *` |
| 24649 | `del_station` | `wireless_dev *` | `net_device *` |
| 24650 | `add_station` | `wireless_dev *` | `net_device *` |
| 24662 | `tdls_mgmt` | `wireless_dev *`（其余参数一致） | `net_device *` |
| 24709 | `set_antenna` | `(wiphy, int, u32, u32)` | `(wiphy, u32, u32)` |
| 24710 | `get_antenna` | `(wiphy, int, u32 *, u32 *)` | `(wiphy, u32 *, u32 *)` |
| 24713 | `tx_control_port` | cookie `u64` 值 | cookie `u64 *` |

注意两点，避免下一轮重复劳动：

- `add_key` / `get_key` / `del_key`（24626–24628）**已经不报错**——`CFG80211_KEY_INSTALL_SUPPORT_ON_WDEV`
  分支已生效，只剩 `set_default_key` 的 `link_id`；
- `set_default_mgmt_key`（24640）未报错，只有 `set_default_beacon_key` 缺 `link_id`。

分类归纳：**wdev/netdev 5 条**、**cookie 值化 3 条**（ROC/mgmt_tx/tx_control_port）、
**link_id 2 条**、**chain index 2 条**、**单点参数变化 3 条**（change_beacon / wiphy_params）、
**idx 参数 2 条**（tx_power 收发各一）。

## 5. 推荐修复方法

1. 以 Linux 7.3 `struct cfg80211_ops` 为唯一 ABI 依据。
2. 对每个旧 qcacld 内部实现增加 Linux 7.3 wrapper；wrapper 接收新参数并转换到已有内部逻辑。
3. `link_id` 必须明确处理：若当前驱动只支持单 link，应校验 `link_id == 0`，不应静默把任意 link 当 link 0。
4. `wireless_dev`/`net_device` 必须通过合法对象关系取得对应 adapter，不能强制转换掩盖类型差异。
   注意 `wdev->netdev` 可能为 NULL（P2P-device wdev），必须判空。
5. `u64` cookie 与 `u64 *` 之间必须按 Linux 7.3 语义正确传递；不能直接改类型后解引用。
   旧代码用 cookie 作为出参回填标识，新 ABI 由 cfg80211 传入定值，需改为“原样回传/忽略”。
6. `change_beacon` 需要把 `cfg80211_ap_update` 中实际使用的 beacon 数据映射到旧内部结构，或单独调整内部调用；不能裸指针转换。
7. 每修一组 API 后按 §2 的严格命令复验；最终构建不得依赖 `-Wno-error`。

## 6. 运行语义审查结论

已完成第一轮静态审查并修复：ROC/mgmt_tx 外部 cookie 与内部 ID 分离、异步回调使用外部 cookie、mgmt/ROC 错误返回传播、mgmt buffer 分配失败 IDR 回滚、key wdev/link 校验、change_beacon 事务式提交与失败回滚。验证日志为 `/tmp/qcacld-semantic-round4.log`，退出码 0，完成 `MODPOST` 与 `wlan.ko` 链接。

仍未闭合、上机前必须知悉的风险：真实 key 读取/删除/default management/beacon key 语义、control-port TX-status cookie 链路、ROC/mgmt cancel 异步 IDR 裸指针生命周期、auth mgmt_tx 特殊路径 TX-status。故当前不能声称完整 cfg80211 运行语义已验证。

## 7. 验收

在 `sky-linux/third_party/micode-sky-icnss2`：

```bash
make -f port.mk wlan > /tmp/cfg80211-linux73.log 2>&1
```

验收条件：

- 退出码 0；
- 日志无 `incompatible-pointer-types`；
- `icnss2.ko` 和 `wlan.ko` 均完成 `MODPOST`/链接；
- `modinfo -F depends wlan.ko` 仍为 `icnss2`；
- 不修改 ICNSS2 的真实 QMI/PLD 符号链路。

## 7. 禁止事项

- 不删除或伪造 cfg80211 功能来消除警告；
- 不把所有 wrapper 都强制 cast 成 `void *` 或旧函数指针；
- 不修改内核 `cfg80211.h` 迎合驱动；
- 不安装/加载模块，不刷写，不做设备端 Wi-Fi 测试；
- 不把“能链接”描述成 cfg80211 运行正确。
