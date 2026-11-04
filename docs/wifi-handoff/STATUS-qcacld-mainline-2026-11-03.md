# STATUS：qcacld-3.0 → mainline 7.3 移植进度（更新至 cfg80211 round16）

本文是当日唯一的状态快照。历史论证仍在各 FINDINGS/HANDOFF 中，本文件只写
**现在到哪一步、证据是什么、下一步做什么**。

## 0. 一句话

平台层（ICNSS2）编译+modpost 已打通；qcacld 的 Linux 7.3 `cfg80211_ops` ABI 适配已完成，严格构建 round16、默认构建 round17 均已通过；`cast-function-type` / `discarded-qualifiers` / `enum-int-mismatch` /
`implicit-fallthrough` 四类严格告警已清零。

round16 证据：`/tmp/qcacld-cfg80211-round16.log` 退出 0，完成 `LD [M] wlan.o`、`MODPOST Module.symvers`、`LD [M] wlan.ko`；`modinfo -F depends wlan.ko` 返回 `icnss2`。旧的 round10 失败日志仍保留为历史证据，不再代表当前源码状态。

代码全部为源码级改动，**未加载模块、未刷机、未安装固件、无设备端运行验证**。

## 1. 四层状态

| 层 | 状态 | 证据 |
|---|---|---|
| 源码基线 | ✅ 锁定 | MiCode `vendor_qcom_opensource_wlan:sky-t-oss` @ `02e10575`（qcacld/host-cmn/fw-api 同批） |
| 平台层 ICNSS2 | ✅ 编译 + modpost 通过 | `/tmp/ssr-adrastea-round9b-platform.log`，退出 0 |
| qcacld 联编 | ✅ 严格与默认均产出当前 `wlan.ko` | `/tmp/qcacld-cfg80211-round16.log`、`/tmp/qcacld-default-round17.log` 均退出 0，完成 MODPOST/链接 |
| cfg80211 ABI | ✅ Linux 7.3 签名适配完成 | station/wdev、cookie、link/radio index、change_beacon、TDLS/control-port adapters 已接入 |
| 设备树 | ✅ 静态通过 | `dts/sm4450-xiaomi-sky.dts` `wifi@22800000`，`cpp`+`dtc` 离线OK |
| 设备运行 | ⛔ 未开始 | — |

旗标生效性说明（本轮实测）：

```text
-Werror=incompatible-pointer-types -Wno-error   -> 仍然 error
-Wno-error=incompatible-pointer-types           -> warning（唯一有效写法）
```

内核 `KBUILD_CFLAGS` 自带该专用 `-Werror`，qcacld 的通用 `-Wno-error` 无法覆盖。

## 2. 基线锁定

```text
wlan 源码  MiCode/vendor_qcom_opensource_wlan : sky-t-oss
           commit 02e10575f1357db03d74b9d59a1edd3a88614181（2023-07-31）
           Qualcomm 基线 LA.VENDOR.1.0.r1-21200-WAIPIO.QSSI13.0-1
           路径 third_party/micode-sky-t-oss/{qcacld-3.0,qca-wifi-host-cmn,fw-api}

平台源码   MiCode/Xiaomi_Kernel_OpenSource : sky-t-oss
           commit 9adb4dca796fbc465bca5731feed21641f87bd2b
           路径 third_party/micode-sky-icnss2/

目标内核   sky-mainline/kernel/linux-7.3（arm64, aarch64-linux-gnu-）
```

已被淘汰：Sony `aosp/LA.UM.7.3.r1`（`third_party/qcacld-3.0-sony-la73/`，只读对照，
不要在其上继续推进）。

## 3. 当天时间线

| 时间 | 事件 | 产物/日志 |
|---|---|---|
| 上午 | ath11k 线关闭，转 qcacld；q1 先试 Sony LA.UM.7.3 | `FINDINGS-qcacld-q1-source-baseline.md` |
| 下午 | 分支搜索 → 锁定 MiCode `sky-t-oss`，改为主基线 | `FINDINGS-qcacld-branches-github.md` §1–3 |
| 16:24–16:25 | 平台依赖编出 | `cnss_utils.ko`、`wlan_firmware_service.ko` |
| 18:24 | cfg80211 首轮结构迁移 + PLD/SNOC 契约、生命周期、ABI 断言 | `micode-sky-cfg4.log`、`.../branches-github.md` §7–§16 |
| 18:45 | `wlan.ko` 产出，28 个 `icnss_*` 由真实平台符号解析（**旗标变更后该产物已过期**） | `micode-icnss2-wlan-round8-final.log` |
| 19:46–21:47 | SSR/recovery 修复 + 严格类型清理（9a–9r 轮） | `/tmp/qcacld-strict-round9*.log` |
| 21:53 | 平台模块 modpost 通过，SSR handoff 落盘 | `ssr-adrastea-round9b-platform.log` |
| 22:xx | 本轮复核：严格构建 17 条，位置与 9r 完全一致；**默认构建同样失败**（`-Wno-error` 压不住内核自带的 `-Werror=incompatible-pointer-types`） | `qcacld-strict-round10.log`、`qcacld-default-round10.log` |

## 4. 已落地的源码改动

`third_party/micode-sky-t-oss` 相对上游共 **50 个文件、+326 / −104**（`git diff --stat`）。

**QDF / host-cmn**

- tasklet：新增 `struct __qdf_bh { tasklet_struct; func; arg; }` 与
  `__qdf_bh_callback()` 上下文转发，删除 `void (*)(void *)` → tasklet callback 的强转
  （`qdf/linux/src/i_qdf_defer.h`）。
- scheduler timer：`struct scheduler_timer` 增加独立 `qdf_mc_timer_callback_t timer_callback`
  字段，scheduler callback 与 timer callback 不再互转；serialization 侧新增 message wrapper。
- `from_timer`/`del_timer` → 7.3 `timer_delete` 等名差；`in_irq()` → `in_hardirq()`;
  `netif_rx_ni()` → `netif_rx()`；`PDE_DATA()` → `pde_data()`；`strlcpy` 系列；
  AES-CMAC 走 `aes_prepareenckey()/aes_encrypt()`（`QCA_MAINLINE_AES_KEY_API`）。
- NAPI 不再访问 Android 私有 `napi_struct.rx_count/rx_list`；`netif_napi_add_weight()` 保持预算。

**qcacld**

- `dev_addr` const 契约：改用 `dev_addr_set()`（5 处）；
- enum/int 原型统一：SAP ACS band、MLME roam reason、CSR TX power、CP stats rate flags、
  WMA beacon rate、WMA roam notification、serialization match type、`qdf_hang_reason`；
- STA/P2P 有意的控制流贯穿补 `fallthrough;`；
- `cfg80211_ch_switch_notify()` 补 `link_id`；random MAC 走 `eth_hw_addr_set()`；
- monitor-channel 回调补 `struct net_device *dev`。

**ICNSS2**

- WPSS / modem SSR notifier 拒绝空 `qcom_ssr_notify_data`，返回 `-EINVAL`；
- `QCOM_SSR_AFTER_SHUTDOWN` 清 `ICNSS_FW_READY`，避免恢复流程用 stale ready；
- 固定 MSA 走 `devm_add_action_or_reset()` 回收；QMI lookup 失败释放 handle；
  DMS 先于 WLFW 初始化；sysfs/debugfs/probe 失败回滚；SSR notifier/timer/workqueue/
  WPSS loader 注销顺序；`rproc_boot()` 失败清脏指针；remove 后释放 rproc 引用。
- **不等价限制保留**：mainline 只有普通 WPSS SSR notifier，原厂 early-SSR 的
  `FW_DOWN` / QMI timeout 抑制被并到 `BEFORE_SHUTDOWN` 开头，未伪造 early 回调。

## 5. 历史硬卡点：17 条 cfg80211 回调（已解决）

问题全部集中在 `core/hdd/src/wlan_hdd_cfg80211.c` 的 `.cfg80211_ops` 初始化
（24621–24713）。完整表格 + 修法约束见
**[HANDOFF-cfg80211-api-linux73.md](HANDOFF-cfg80211-api-linux73.md) §4/§5**。

分类：wdev/netdev 5 条、cookie 值化 3 条、`link_id` 2 条、chain index 2 条、
`idx` 参数 2 条、单点参数变化 3 条。

`add_key` / `get_key` / `del_key` / `set_default_mgmt_key` **已不报错**，不要重复修。

严格复验命令见 `HANDOFF-cfg80211-api-linux73.md §2`（用 `KCFLAGS` 注入
`-Werror=incompatible-pointer-types` 等五项，不修改 Kbuild）。

## 6. 复现构建

```bash
cd sky-linux/third_party/micode-sky-icnss2
make -f port.mk dependencies   # cnss_utils.ko, wlan_firmware_service.ko
make -f port.mk platform       # icnss2.ko（依赖 wlan_firmware_service）
make -f port.mk wlan           # 平台 + wlan.ko（depends icnss2）
```

前两步当前通过；第三步已在 round16/round17 分别以严格/默认配置通过并链接 `wlan.ko`。

依赖核对：`icnss2.ko -> wlan_firmware_service`，`wlan.ko -> icnss2`，
ICNSS2 OF alias = `qcom,icnss`。

## 7. 仍未验证 / 已知风险

1. **cfg80211 运行语义**：即使签名修齐，key / `mgmt_tx` cookie / station 路径的实际行为未验证。
2. **SSR 时序不等价**：firmware-down 标志、QMI timeout 抑制、qcacld 收发 SSR 事件的先后未在硬件核对。
3. **并发**：全局 `wpss_ssr_work` / recovery timer / `rproc` 引用在重复 SSR、多实例下的语义只做了静态防护。
4. **compat 临时实现**：`hrtimer_callback_running`、dummy netdev 等为推进编译的临时路径，可加载前必须复审。
5. **板级供电**：`vdd-3.3-ch1` 未确认；DTS 里仅 ch0 有 supply，ch1 留了注释。
6. **DTS 不可再生**：`tools/mksky_dts.py` 仍生成旧 ath11k 节点，已加防覆盖门禁，但与目标 DTS 有 223 行差异。
7. **`cnss_utils.ko`** 能编，但不在 `icnss2` 依赖链里，不要当成必装模块。
8. **固件路径**：`ADRASTEA_PATH_PREFIX "adrastea/"` 与 remoteproc `firmware-name` 的一致性尚未审查。

## 8. 下一步顺序

1. ✅ 完成 §5 的 17 条 wrapper 适配（`link_id==0`、`wdev->netdev` 判空、value-cookie 适配）。
2. ✅ 严格与默认构建均通过并完成 MODPOST/链接；默认构建仍有历史非致命 warning，但无 ABI error。
3. cfg80211 运行语义审查（key/mgmt_tx/station）。
4. 里程碑 M1：设备侧 `QMI Server Connected` + BDF/cal report；M2：HTC 连接；M3：`wlan0`/扫描。
5. 到 M1 之前不要刷机；实验参数一律走 `/etc/sky-wifi-params.try`。

## 9. 文档地图

| 文件 | 定位 |
|---|---|
| **本文件** | 当前状态唯一入口 |
| `HANDOFF-qcacld-mainline.md` | 路线结论、资产清单、q1–q5 总体计划 |
| `HANDOFF-cfg80211-api-linux73.md` | **当前任务**：17 条 ABI 适配 + 严格复验命令 |
| `HANDOFF-ssr-recovery-adrastea-linux73.md` | SSR 修复与不等价边界 |
| `FINDINGS-qcacld-branches-github.md` | 选型过程 + 当日各轮构建流水（§7–§20） |
| `FINDINGS-dt-references-wcn6750-adrastea.md` | DT 五参考交叉验证（结论已并入 HANDOFF §8） |
| `FINDINGS-session8/9-*.md` | ath11k 线硬件实测（qcacld 侧配置必须与之一致） |
| `FINDINGS-qcacld-q1-source-baseline.md`、`NEXT-PROMPT-qcacld-session1.md` | **历史**，已被取代 |
