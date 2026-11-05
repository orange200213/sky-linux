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

---

## 7. 2026-11-04 晚更新：断连链路已逐步验证，卡点最终锁定在 `lim_send_deauth_cnf()`

### 7.1 结论：整条"帧→完成事件→回调"链路是好的，缺的是最后一步 CM 响应

用插桩逐段验证（都是实测 on device）：

```text
[162.395] [PE] Deauth TX: vdev 0 seq_num 2051 reason 3 waitForAck 1
[162.411] sky-dbg7: mgmt_txrx_compl desc=63 status=0 tx_cb=0 ota_cb=1 ctx=1
[162.414] sky-dbg9: deauth_tx_complete_cnf_handler tx_success=0 buf=yes
[162.420] sky-dbg9: lim_deauth_tx_complete_cnf tx_success=0
[162.455] OSIF: wlan0(vdevid-0): locally-generated disconnect … cm_id 0xd000002
[179.432] scheduler_watchdog_notify: Callback cm_handle_disconnect_resp exceeded 10s   ← 仍然卡
[185.316] cm_disconnect_start_req_sync: Disconnect timeout with status 15
```

已确认为**正常**的环节（不再是怀疑对象）：

| 环节 | 实测 |
|---|---|
| 管理帧发送 | 走 WMI 路线，不是 HTT（`MGMT_TX_COMPL_IND` 从未出现属正常） |
| WMI 完成事件 | `WMI mgmt_tx_compl desc_id=63 status=0`，帧内容确认是 `fc=0x00c0` deauth |
| ota 回调注册 | `tx_cb=0 ota_cb=1`（deauth 走 ota 完成回调） |
| deauth 完成回调 | `lim_deauth_tx_complete_cnf_handler(tx_success=0)` → `lim_deauth_tx_complete_cnf` |
| `desc_id` 恒为 63 | 三个帧（auth/assoc/deauth）都是 63，但帧内容各不相同，属于固件侧重用，不是 bug |

### 7.2 卡点：`lim_send_deauth_cnf()` 早退

```c
QDF_STATUS lim_send_deauth_cnf(struct mac_context *mac_ctx)
{
	deauth_req = mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq;
	if (deauth_req) {        /* ← 为空则整段跳过：不停 timer、不发 cnf */
		…
	}
	…
}
```

而 `pMlmDeauthReq` **只在 legacy 路径** `lim_send_management_frames.c:3523`
（`lim_process_mlm_deauth_req()`）里被赋值。CM 路径
（`lim_process_nb_disconnect_req()` → `lim_prepare_and_send_deauth()` →
`lim_process_disconnect_sta()` → …）是否走到那个赋值点，是下一个要确认的问题。

若确实没赋值，则 `lim_send_deauth_cnf()` 什么都不做 →
`LIM_MLM_DEAUTH_CNF` 不发 → `cm_handle_disconnect_resp` 永不被调用 → 10s watchdog。

### 7.3 下一轮（两小步即可收口）

1. 在 `lim_send_deauth_cnf()` 里加一行：打印 `deauth_req` 是否为 NULL、`sessionId`、结果
   （若 NULL 即为确诊）；同时在 `lim_process_mlm_deauth_req()` 里打印是否被调用。
2. 修复方案（二者选一，建议 A）：
   * **A**：在 CM 断连完成路径里直接调 `lim_cm_send_disconnect_rsp(mac_ctx, vdev_id)`
     （`lim_send_sme_rsp_messages.c:761` 已有该函数），即 CM 断连不再依赖 legacy 的
     `pMlmDeauthReq`/`pMlmDisassocReq` 机制。`vdev_id` 在
     `lim_prepare_and_send_deauth()` 里可从 `req->req.vdev_id` 拿到，可存到 session 或
     新增一个小字段。
   * **B**：在 `lim_prepare_and_send_deauth()` 里把对应请求塞进
     `mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq`，让 legacy cnf 流程照常跑完。

### 7.4 本轮其它产出

* `/etc/init.d/sky-wifi` 的 `start()` 里加了 `sleep 10`（备份 `sky-wifi.bak-20261104`），
  服务实测 24.5s 起完，`COMPLETED` + DHCP + ping 正常
* 插桩一览（用 `grep -rn sky-dbg core/` 可一次清掉）：
  `sky-dbg3` ol_tx_send.c（数据面完成计数）/ lim_process_sme_req_messages.c（断连请求）；
  `sky-dbg4` htt_t2h.c（MGMT_TX_COMPL_IND 是否到达）；
  `sky-dbg5` ol_tx.c（mgmt cb 注册 / mgmt send）+ wma_mgmt.c（WMI 完成事件）；
  `sky-dbg6` wma_mgmt.c（完成处理 + 帧类型）；
  `sky-dbg7` wlan_mgmt_txrx_tgt_api.c（tx_cb/ota_cb 是否存在）；
  `sky-dbg8` lim_send_management_frames.c（disassoc 路径，实测不触发）；
  `sky-dbg9` lim_send_management_frames.c（**deauth 路径，实测会触发**）
* 当前设备镜像：`/root/wlan.ko` = md5 `1f9660d89f3c00f1ec36812ab553875c`（含 dbg3~dbg9）
* 每次改模块都要重启（`CONFIG_MODULE_UNLOAD=n`）；断连卡死后 WPSS 会 offline 且 SSR 起不来
  （`-110`），只能重启；偶发落 fastboot 时用
  `fastboot flash boot_a sky-mainline/shim/out/sky-shim-boot-v4.img`

---

## 8. 2026-11-04 深夜：因果链全程打通，真正的卡点是 `cm_sm_deliver_event()` 阻塞（不是响应缺失）

按提案要求逐节点取证（`sky-dbgA/B/C` 三组探针），**实测**结果：

```text
[162.416] dbgA: deauth_compl_handler tx_success=0 pMlmDeauthReq=set session=found retry=2
[162.419] dbgA: posted WMA_DEAUTH_TX_COMP ret=0 bodyval=0
[162.425] dbgA: LIM got WMA_DEAUTH_TX_COMP bodyval=0 bodyptr=ffff800088333c68
[162.431] dbgA: lim_send_deauth_cnf entry pMlmDeauthReq=set
[162.454] dbgA: lim_cm_send_disconnect_rsp #0 vdev=0          ← CM 响应确实发出
[162.457] dbgB: cm_handle_disconnect_resp ENTER vdev=0
[162.460] dbgB: cm_fill_disconnect_resp status=0
[162.463] dbgB: calling wlan_cm_disconnect_rsp
[162.528] dbgC: cm_disconnect_rsp active_cm_id=0xd000002 resp_cm_id=0xd000002 prefix=0xd000000
[162.530] dbgC: -> cm_sm_deliver_event(DISCONNECT_DONE)
（之后 10 s 无任何输出）
[172.x  ] scheduler_watchdog: cm_handle_disconnect_resp exceeded 10s
```

### 8.1 结论：之前两个假设都被推翻

| 假设 | 实测 |
|---|---|
| `mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq` 为 NULL 导致 `lim_send_deauth_cnf()` 早退 | **错**：`pMlmDeauthReq=set`，`lim_send_deauth_cnf()` 正常执行 |
| CM 响应没有发出（`cm_handle_disconnect_resp` 从未被调用） | **错**：`lim_cm_send_disconnect_rsp #0 vdev=0` 已发出，`cm_handle_disconnect_resp` 已进入 |

真正阻塞点（按调用栈）：

```
cm_handle_disconnect_resp()                        wlan_cm_vdev_disconnect.c:363
  cm_fill_disconnect_resp()            OK (status=0)
  wlan_cm_disconnect_rsp()                          qca-wifi-host-cmn/.../wlan_cm_api.c:104
    cm_disconnect_rsp()                             .../wlan_cm_disconnect.c:782
      cm_id 校验通过 (active 0xd000002 == resp 0xd000002)
      cm_sm_deliver_event(vdev, WLAN_CM_SM_EV_DISCONNECT_DONE, ...)   ★ 永不返回
        → wlan_cm_sm.c 的 DISCONNECTING 状态处理（:464 / :618 / :718 分支）
        → cm_disconnect_complete() → cm_notify_disconnect_complete()
        → mlme_cm_osif_disconnect_complete()   ← 疑似在 OS_IF 回调上下文里做同步/等待
```

而 `cm_handle_disconnect_resp()` 本身是 `lim_cm_send_disconnect_rsp()` 投递到
**`QDF_MODULE_ID_OS_IF` 队列**的回调 ⇒ SM 处理里若再对同一队列/同一 serialization
命令做「投递并等待」，就是**自死锁**（10s 后 scheduler watchdog 判定超时，
进而 `cds_trigger_recovery_handler` 强制 assert FW，SSR 又起不来 → 只能重启）。

### 8.2 修复方向（下一轮实现，需与提案确认）

1. **首选**：让 CM 断连响应处理**不在 OS_IF scheduler 回调里跑**：
   * 把 `lim_cm_send_disconnect_rsp()` 的目标模块从 `QDF_MODULE_ID_OS_IF` 改成
     MAC/CM 模块队列（或 `scheduler_post_message` 到独立队列），
   * 或在 `mlme_cm_osif_disconnect_complete()` 路径上改为**异步**（workqueue/消息），
     彻底消除"回调内同步完成"。
2. 备选：确认 serialization 是否在响应到达前就应释放 disconnect 命令
   （`wlan_serialization_remove_cmd` 的时序），若是，则在 `lim_send_deauth_cnf()`
   完成前释放，避免回调里再操作同一命令。
3. **不要**采用"发出 deauth 就立刻响应 CM"的做法（提案已指出会破坏 completion 语义）。

### 8.3 探针清单（本轮新增）

| 标签 | 位置 | 作用 |
|---|---|---|
| `sky-dbgA` | `lim_send_management_frames.c`（deauth 完成 handler ×2、`lim_send_deauth_cnf` 入口）、`lim_process_message_queue.c`（`WMA_DEAUTH_TX_COMP` 到达）、`lim_send_sme_rsp_messages.c`（`lim_cm_send_disconnect_rsp` 计数） | 证明 legacy 链与 CM 响应投递 |
| `sky-dbgB` | `wlan_cm_vdev_disconnect.c`（`cm_handle_disconnect_resp` 入口 / `cm_fill_disconnect_resp` 结果 / 调 `wlan_cm_disconnect_rsp` 前后） | 定位到 `wlan_cm_disconnect_rsp` 不返回 |
| `sky-dbgC` | `wlan_cm_disconnect.c`（cm_id 校验、`cm_sm_deliver_event` 前后、`cm_disconnect_complete` 前后） | 定位到 `cm_sm_deliver_event` 阻塞 |
