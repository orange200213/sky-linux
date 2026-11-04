# 交接：qcacld 管理帧 TX 完成缺失（断连卡死）— 2026-11-04

> 上一轮已修掉 3 条 NAPI teardown WARNING（见 `FINDINGS-…§22`），本轮开始查"断连卡死"，
> **根因范围已收窄到管理帧 TX completion**，下一步只剩三处验证。

## 1. 本轮已完成

| 项 | 状态 |
|---|---|
| NAPI teardown 的 3 条 WARNING | ✅ 已修（`hif_deinit_rx_thread_napi()` 补 `napi_disable()`），实测 0 条 |
| 数据面 TX completion（HTT） | ✅ 正常：ping 期间 8 次 `tx_compl`，`status=0` |
| 管理帧 TX completion（HTT `MGMT_TX_COMPL_IND`） | ❌ **一次都没到达主机** |
| 断连请求到达 PE | ✅ `disconnect_req vdev=0 cm_id=0xd000002 source=4` |
| 断连完成 | ❌ 10s 后 `scheduler_watchdog` → 强制 assert FW → SSR 恢复失败(-110) |

## 2. 实验记录（可复现）

```sh
rc-service sky-wifi restart          # 连上（wpa_state=COMPLETED, 192.168.1.186）
ping -c3 1.1.1.1                     # 数据面流量
dmesg | grep -c "sky-dbg3: tx_compl" # → 8        （数据面完成事件正常）
dmesg | grep -c "sky-dbg4"           # → 0        （MGMT_TX_COMPL_IND 从未到达）
ip link set wlan0 down               # 触发断连
dmesg | grep "sky-dbg3: disconnect_req"   # → vdev=0 cm_id=0xd000002 source=4
# 10s 后：
#   scheduler_watchdog_notify: Callback cm_handle_disconnect_resp … exceeded 10s
#   scheduler_watchdog_timeout → cds_trigger_recovery_handler
#   icnss2: Received early crash indication from FW
#   remoteproc1: crash detected in wpss → recovering
#   remoteproc1: can't start rproc wpss: -110        ← SSR 恢复失败
```

## 3. 关键代码路径与**最可疑点**

```
lim_cm_handle_disconnect_req()            lim_process_sme_req_messages.c:4240
  └─ 有 session → 发 deauth（waitForAck 1）
      └─ ol_txrx_mgmt_send_ext()           core/dp/txrx/ol_tx.c:173
          tx_msdu_info.htt.action.do_tx_complete =
              pdev->tx_mgmt_cb.ota_ack_cb ? 1 : 0;        ← ★★★ 关键
      └─ 期望 firmware 回：
           HTT_T2H_MSG_TYPE_MGMT_TX_COMPL_IND   htt_t2h.c:458（本次 0 次）
           或 WMI_MGMT_TX_COMPLETION_EVENTID     wma_mgmt_tx_completion_handler() wma_mgmt.c:2831
  └─ 完成后 → eWNI_SME_DISCONNECT_DONE_IND → lim_cm_send_disconnect_rsp()
           → cm_handle_disconnect_resp()      （lim_send_sme_rsp_messages.c:761/836）
```

**假设（最可能）**：`pdev->tx_mgmt_cb.ota_ack_cb` 为 NULL ⇒ `do_tx_complete = 0`
⇒ 固件被明确告知"不要上报完成" ⇒ deauth 永远等不到 ACK 完成 ⇒ 断连卡死。
回调注册入口：

```
wma_data.c:1449   cdp_mgmt_tx_cb_set(soc, pdev_id, 0, …)   ← 是否被调用待确认
ol_tx.c:153       ol_txrx_mgmt_tx_cb_set()                 ← 真正给 tx_mgmt_cb 赋值
```

另一条可能：`ol_tx_get_is_mgmt_over_wmi_enabled()` 为真 ⇒ `htt_t2h.c:495` 直接
"Ignoring HTT_T2H_MSG_TYPE_MGMT_TX_COMPL_IND"，改等 WMI 事件；而 WMI 那条没人处理。

## 4. 下一步（三处插桩，一次编译可全查）

1. `ol_tx.c` `ol_txrx_mgmt_tx_cb_set()` —— 打印是否注册、注册了哪些回调（`ota_ack_cb`/`comp_cb`）
2. `ol_tx.c` `ol_txrx_mgmt_send_ext()` —— 打印 `vdev_id`、`type`、`do_tx_complete` 实际取值
3. `wma_mgmt.c` `wma_mgmt_tx_completion_handler()` —— 打印入口（确认 WMI 路线是否走）
4. 视结果修：
   * 若 `ota_ack_cb == NULL` → 在 `wlan_hdd_mgmt_tx`/`hdd_mgmt_ota_ack_cb` 处补齐注册
   * 若 WMI 路线 → 在 `wma_mgmt_tx_completion_handler()` 里补发 disconnect 完成
   * 若 `is_mgmt_over_wmi_enabled` 与实际不符 → 由 `wma_main.c:6063` 的 cfg 决定，需对齐

## 5. 当前设备/构建状态

```
内核         7.3.0-g78458d4eea4e-dirty (#112)，boot_a = sky-shim-boot-v4.img
rootfs       /dev/sda20 ext4（已重建；/proc /sys /tmp 目录已补齐）
模块         /root/wlan.ko = 62,486,056 B，md5 148371147b19d94849a85f65460e905a
             （含 sky-dbg3 / sky-dbg4 插桩）
             icnss2.ko = 279bc58…（含平台 IRQ 修复）
服务         rc-service sky-wifi（OpenRC，default runlevel；手动 restart 正常）
             开机那次会失败（guard/依赖阶段），原因待查；手动 restart 可恢复
固件         /lib/firmware/qcom/sm4450/xiaomi-sky/wpss.*、/lib/firmware/adrastea/*
```

**注意**：断连卡死后 WPSS 会 offline 且 SSR 恢复失败（`-110`），此时只能重启；
重启后先 `rc-service sky-wifi restart`。

## 6. 插桩位置（源码里已存在的调试打印）

| 文件 | 打印 | 作用 |
|---|---|---|
| `core/dp/txrx/ol_tx_send.c`（`ol_tx_completion_handler`） | `sky-dbg3: tx_compl #N num_msdus=.. status=..` | 数据面完成事件计数（前 8 条 + 每 512 条） |
| `core/mac/src/pe/lim/lim_process_sme_req_messages.c` | `sky-dbg3: disconnect_req vdev=.. cm_id=.. source=..` | 断连请求是否到 PE |
| `core/dp/htt/htt_t2h.c`（`MGMT_TX_COMPL_IND`） | `sky-dbg4: MGMT_TX_COMPL_IND arrived len=.. mgmt_over_wmi=..` | 管理帧完成事件是否到达 |

正式版可按这些字符串 `grep -n "sky-dbg" -r` 一次性清除。
