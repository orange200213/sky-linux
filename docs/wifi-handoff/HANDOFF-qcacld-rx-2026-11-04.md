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

---

## 9. 提案结论（已采纳）：采用 C2，拆分 CM 内部完成与 OSIF/用户态通知

> 本节是提案方给出的最终决策，实现时以此为准。**§8 里"换投递队列"的想法降级为
> 实现细节，不是根修复**；§4.4/§7 里关于 `pMlmDeauthReq` 的猜测已被实测否证。

### 9.1 修正后的因果链（实测 + 源码）

```
WMA_DEAUTH_TX_COMP
 → lim_send_deauth_cnf()
 → lim_cm_send_disconnect_rsp()           (已发出，实测 #0 vdev=0)
 → cm_handle_disconnect_resp()            (已进入)
 → wlan_cm_disconnect_rsp() → cm_disconnect_rsp()
 → cm_sm_deliver_event(WLAN_CM_SM_EV_DISCONNECT_DONE)      ★ 持有 cm_sm_lock
 → cm_disconnect_complete()
 → cm_notify_disconnect_complete()
 → mlme_cm_osif_disconnect_complete() → osif_disconnect_handler()   ★ 阻塞
 → (之后才会) cm_remove_cmd() / qdf_event_set(&cm_ctx->disconnect_complete)
```

### 9.2 两个结构性缺陷

1. **OSIF/HDD 通知发生在 CM 状态机锁仍被持有时**
   ```c
   /* cm_sm_deliver_event() */
   cm_lock_acquire(cm_ctx);
   status = cm_sm_deliver_event_sync(cm_ctx, event, data_len, data);
   cm_lock_release(cm_ctx);
   ```
   而 `cm_disconnect_complete()` 在锁内先调 `cm_notify_disconnect_complete()`
   （内含 `mlme_cm_osif_disconnect_complete()`）。

2. **serialization command 的移除晚于 OSIF 通知**
   `cm_disconnect_complete()` 的执行顺序是：
   `cm_notify_disconnect_complete()` → `cm_flush_pending_request()` →
   `cm_remove_cmd()`（内部调 `wlan_serialization_remove_cmd()`）→
   `qdf_event_set(&cm_ctx->disconnect_complete)`。
   OSIF/HDD 若等待 CM 状态离开 DISCONNECTING、等待 serialization cmd 被移除、
   或等待 `disconnect_complete` 事件，就形成**自锁**（10 s watchdog 完全吻合）。

⇒ 根因**不是** CM 响应丢失，也**不是** deauth 请求保存方式；是 **completion 的后处理上下文与顺序**。

### 9.3 C2 实现要点（给实现 prompt）

目标形态：

```
cm_disconnect_complete()
  → 更新 CM 内部状态
  → cm_flush_pending_request() / cm_remove_cmd()   ← 内部清理提前
  → qdf_event_set(&cm_ctx->disconnect_complete)
  → 复制 wlan_cm_discon_rsp + 取 vdev ref
  → 异步入队 OSIF 通知（不要在此处同步调用）
  → 返回（cm_lock 由 cm_sm_deliver_event 释放）

异步 worker / 独立队列
  → osif_disconnect_handler()
  → cfg80211_disconnected()
  → HDD post-user cleanup
  → 释放 response + vdev ref
```

具体改动点：

| 文件 | 函数 | 改动 |
|---|---|---|
| `qca-wifi-host-cmn/umac/mlme/connection_mgr/core/src/wlan_cm_disconnect.c:552` | `cm_disconnect_complete()` | **重排顺序**：先 `cm_flush_pending_request()` / `cm_remove_cmd()` / `qdf_event_set()`，再入队 OSIF 通知 |
| 同文件 `:533` | `cm_notify_disconnect_complete()` | 拆分：内部通知（`mlme_cm_disconnect_complete_ind`/`mlo_sta_link_disconn_notify`/`cm_if_mgr_inform_disconnect_complete`/`cm_inform_blm_disconnect_complete`/`cm_clear_vdev_mlo_cap`）保持同步；`mlme_cm_osif_disconnect_complete()` 改为**异步**（复制 resp + 持有 vdev ref） |
| `wlan_cm_disconnect.c` | 新增 | 异步回调（worker 或专用 scheduler 队列消费），负责 OSIF/HDD 通知并释放资源 |
| `qcacld-3.0/core/mac/src/pe/lim/lim_send_sme_rsp_messages.c:761` | `lim_cm_send_disconnect_rsp()` | 保持现状（**不要**靠改 `QDF_MODULE_ID_OS_IF` 目标来规避） |

**禁止事项**（提案明确）：
* 不要改成"deauth 发出即完成"（会绕过 cleanup 顺序、导致重复完成/早于内部清理的
  `cfg80211_disconnected`、后续 connect 被拒、deauth 重试路径重复完成、SSR 并发重复响应）
* 不要复用 legacy `pMlmDeauthReq`
* 不要只换 scheduler 目标队列当作根修复

### 9.4 实现后必须验证的验收点

```
cm_sm_deliver_event(DISCONNECT_DONE) 返回（不再 10s 不返回）
cm_remove_cmd 完成
disconnect_complete event 置位
cfg80211_disconnected 返回
无 scheduler watchdog
无 "Disconnect timeout with status 15"
无重复 disconnect response（lim_cm_send_disconnect_rsp 计数应 = 1 次/请求）
SSR 不再被触发（WPSS 保持 running）
```

### 9.5 下一步探针（若要先用证据确认顺序假设，可选）

在 `cm_disconnect_complete()` 内：
`before cm_notify_disconnect_complete` / `before cm_remove_cmd` / `after cm_remove_cmd` /
`before qdf_event_set` / `after qdf_event_set`；
在 `osif_disconnect_handler()` 内：`entry` / `osif_cm_disconnect_comp_ind PRE` /
`cfg80211_disconnected` 前后 / `POST` 后。
若阻塞发生在 `cm_remove_cmd` 之前 → 直接证明顺序问题；若在 HDD 内，
再在 `hdd_cm_disconnect_complete_pre_user_update()` / `post_user_update()` 边界细分。

---

## 10. C2 已实现并上机验证：CM 层死锁已解决，暴露出下一层阻塞（HDD pre_user_update）

### 10.1 实现（`qca-wifi-host-cmn/umac/mlme/connection_mgr/core/src/wlan_cm_disconnect.c`）

```c
struct cm_osif_disconn_notify {                 /* 新增 */
	struct wlan_objmgr_vdev *vdev;
	struct wlan_cm_discon_rsp resp;         /* 复制，脱离调用者栈 */
};

static QDF_STATUS cm_osif_disconn_notify_cb(struct scheduler_msg *msg)   /* 异步消费 */
{
	mlme_cm_osif_disconnect_complete(ctx->vdev, &ctx->resp);
	wlan_objmgr_vdev_release_ref(ctx->vdev, WLAN_MLME_CM_ID);
	qdf_mem_free(ctx);
}

static void cm_queue_osif_disconnect_notify(vdev, resp)
{
	ctx = qdf_mem_malloc(...); qdf_mem_copy(&ctx->resp, resp, ...);
	wlan_objmgr_vdev_try_get_ref(vdev, WLAN_MLME_CM_ID);
	msg.callback = cm_osif_disconn_notify_cb;
	scheduler_post_message(QDF_MODULE_ID_OS_IF, QDF_MODULE_ID_OS_IF,
			       QDF_MODULE_ID_OS_IF, &msg);        ← 关键：投到 OS_IF 队列
}

/* cm_notify_disconnect_complete() 里： */
-	mlme_cm_osif_disconnect_complete(cm_ctx->vdev, resp);
+	cm_queue_osif_disconnect_notify(cm_ctx->vdev, resp);
```

要点：
* `QDF_MODULE_ID_MLME` 队列在本 build（MCL）**不可用**，`scheduler_post_message` 直接失败
  （实测 `sky-c2: failed to queue ...`）→ 必须用 `QDF_MODULE_ID_OS_IF`
* 复制 `wlan_cm_discon_rsp` + 持 vdev ref，保证异步消费时数据有效
* 内部状态清理（`cm_remove_cmd` / `qdf_event_set`）仍在同步路径里先完成

### 10.2 效果（实测，正面对比）

| 指标 | C2 之前 | C2 之后 |
|---|---|---|
| `cm_sm_deliver_event(DISCONNECT_DONE)` | **永不返回**（10 s 后 watchdog） | `<- … status=0` ✅ 立即返回 |
| `Disconnect timeout with status 15` | 有 | **0 次** ✅ |
| `scheduler_watchdog` 名称 | `cm_handle_disconnect_resp` | 变为 `cm_osif_disconn_notify_cb`（**新位置**） |
| OSIF 通知 | 卡在 CM 锁内 | `sky-c2: queued …` → `sky-c2: osif disconnect complete (deferred)` ✅ 送达 |

⇒ **提案 C2 的判断正确**：解除 CM 锁内同步调用后，原来的自死锁消失。

### 10.3 新暴露的阻塞点：HDD `pre_user_update`

```
[156.993] sky-c2b: -> HDD pre_user_update
（不再返回）→ watchdog: cm_osif_disconn_notify_cb
wpss: offline（watchdog → cds_trigger_recovery → 强制 assert）
Disconnect timeout = 0（CM 完成计数正常）
```

调用栈：`osif_disconnect_handler()` → `osif_cm_disconnect_comp_ind(vdev, rsp, OSIF_PRE_USERSPACE_UPDATE)`
→ `hdd_cm_disconnect_complete_pre_user_update()`（`core/hdd/src/wlan_hdd_cm_disconnect.c:307`）
其中依次调用（**下一个要定位的候选**）：

```c
hdd_conn_set_authenticated(adapter, false);
hdd_napi_serialize(0);
hdd_disable_and_flush_mc_addr_list(adapter, pmo_peer_disconnect);   ← ★ 最可疑（同步等 WMI PMO 响应）
__hdd_cm_disconnect_handler_pre_user_update(adapter);               ← 内含 hdd_stop_tsf_sync / ipa evt / place_marker
hdd_handle_disassociation_event(adapter, &rsp->req.req.bssid);
wlan_rec_conn_info(...);
hdd_ipa_set_tx_flow_info();
```

### 10.4 下一轮定位方法（一行插桩即可）

在 `hdd_cm_disconnect_complete_pre_user_update()` 的上述 6 个调用前后各加
`pr_err("sky-c2c: N before/after ...")`，重编重测，即可确认是 `hdd_disable_and_flush_mc_addr_list()`
（PMO mc-list flush 同步等固件响应）还是 `hdd_handle_disassociation_event()`。

⚠️ 注意与 C2 区分：**这是 HDD/PMO 层的新问题**，不是 C2 引入的回归；
C2 修复本身应保留。

### 10.5 本轮产物

```
wlan.ko（C2 + sky-c2b 边界插桩）  md5 f756cad75b141e56669747058e06929c
源码  wlan_cm_disconnect.c（C2）、osif_cm_disconnect_rsp.c（sky-c2b）
备份  wlan_cm_disconnect.c.bak-c2-20261104
```

---

## 11. 第二轮定位（按提案要求：成对探针 + 内核栈）：阻塞在 `qdf_periodic_work_stop_sync()`

### 11.1 六处成对探针结果（`sky-c2c`）

```
[178.135] [1/6] -> hdd_conn_set_authenticated      [178.136] <- 
[178.138] [2/6] -> hdd_napi_serialize              [178.140] <- 
[178.142] [3/6] -> hdd_disable_and_flush_mc_addr_list [178.143] <- 
[178.145] [4/6] -> __hdd_cm_disconnect_handler_pre_user_update [178.147] <-
[178.149] [5/6] -> hdd_handle_disassociation_event     ← 无出口 ★
（[6/6] hdd_ipa_set_tx_flow_info 从未执行）
```

⇒ 提案要求的"只在第一个有入口无出口的函数内加第二层探针"用得上：
**第 5 项 `hdd_handle_disassociation_event()` 就是阻塞点**。
（PMO/NAPI 清理均已正常返回，提案关于"不要预设 PMO"的判断正确。）

### 11.2 内核栈（`sysrq-w` 阻塞任务 + `sysrq-l` NMI 背trace，实测）

**A. 我们的调用线程**（`scheduler_threa`，OS_IF 调度线程，state:D）：

```
qdf_periodic_work_stop_sync+0x2c/0x44            [wlan]   ← 卡在这里
__hdd_bus_bw_compute_timer_stop+0x38/0x14c
hdd_bus_bw_compute_timer_try_stop+0x70/0x74
hdd_handle_disassociation_event+0x8c/0x1bc               ← [5/6]
hdd_cm_disconnect_complete_pre_user_update+0x128/0x1e4
hdd_cm_disconnect_complete+0x50/0xc0
osif_cm_disconnect_comp_ind+0x28/0x40
osif_disconnect_handler+0x194/0x294
osif_cm_disconnect_complete_cb+0x10/0x1c
mlme_cm_osif_disconnect_complete+0x28/0x40
cm_osif_disconn_notify_cb+0x40/0x80              ← C2 的异步回调
scheduler_os_if_mq_handler / scheduler_thread
```

**B. 被等待的那次 periodic work**（NMI 采样，仍在执行中）：

```
pc : hdd_bus_bw_work_handler+0x3e0/0x618 [wlan]   (P)
  __qdf_periodic_work_handler+0x1c/0x50 [wlan]
```

**C. 另一条同点阻塞**（SSR 路径，`irq/17-smp2p-wp` state:D）：

```
qdf_periodic_work_stop_sync → __hdd_bus_bw_compute_timer_stop
 → hdd_bus_bw_compute_timer_stop → hdd_soc_recovery_cleanup → wlan_hdd_pld_uevent
```

### 11.3 机理分析

`qdf_periodic_work_stop_sync()`（`qca-wifi-host-cmn/qdf/linux/src/qdf_periodic_work.c:136`）：

```c
	pwork->msec = 0;
	while (cancel_delayed_work_sync(&pwork->dwork))
		;
```

而 `__qdf_periodic_work_handler()` 每次执行完会按 `pwork->msec` **自重新排队**：

```c
	pwork->callback(pwork->context);
	msec = pwork->msec;
	if (msec)
		schedule_delayed_work(&pwork->dwork, msecs_to_jiffies(msec));
```

⇒ 若 `hdd_bus_bw_work_handler()` 内部**重新启动定时器**（例如结束处再次
`qdf_periodic_work_start()`/`hdd_bus_bw_compute_timer_start()` 把 `msec` 写回非 0），
`cancel_delayed_work_sync()` 会不停返回 true → 死循环；
若 handler 本身长时间不返回（NMI 显示它仍在 `hdd_bus_bw_work_handler+0x3e0`），
`cancel_delayed_work_sync()` 就一直等它 → 两条栈同时出现。

两者的共同点：**在 OSIF 回调上下文里做同步等待**（本项目第 4 次遇到同类问题）。

### 11.4 下一轮要做的判定（两选一，各一轮）

1. **判定 handler 是否自重启**：在 `hdd_bus_bw_work_handler()` 入口/出口打印
   `hdd_ctx->bus_bw_work.msec`，并在 `qdf_periodic_work_start()` 里计数。
   若出口前 `msec` 又变成非 0 ⇒ 自重启循环确认。
2. **判定 handler 是否长时间不返回**：在 `__hdd_bus_bw_work_handler()` 内部按段打点
   （`wlan_hdd_validate_context` / 各 adapter 统计 / `hdd_pld_request_bus_bandwidth` /
   `cdp_*`），找出它停在哪一段（NMI 的 `+0x3e0` 偏移可对照反汇编）。

### 11.5 候选修复（待判定后选）

| 方案 | 说明 | 风险 |
|---|---|---|
| **A**（推荐先试） | 断连路径改用 `qdf_periodic_work_stop_async()`（`wlan_hdd_cm_disconnect.c:80` 的 `hdd_bus_bw_compute_timer_try_stop` 调用点），不在 OSIF 回调里同步等 | 定时器可能稍后再跑一次，需确认无副作用 |
| **B** | 给 `struct qdf_periodic_work` 加 `stopping` 标志，handler 在 stopping 时不再自重启，`stop_sync` 结束后清标志 | 改动 QDF 公共层，需回归其他 periodic work |
| **C** | 把 `hdd_handle_disassociation_event()`（或其中 timer stop）移出 OSIF 回调上下文（与 C2 同思路，异步化） | 改动面更大，但语义最干净 |

⚠️ 不建议：跳过 bus-bw 清理、延长 watchdog、或用 `stop_async` 后不处理后续状态。

### 11.6 本轮产物

```
wlan.ko（C2 + sky-c2b + sky-c2c）  md5 8d78453c14292aa1ce6ddfdbd6b70929
备份  wlan_hdd_cm_disconnect.c.bak-c2c-20261104
```

---

## 12. ✅ 根因确认并修复：`uint8_t` 计数器溢出 `NUM_CPUS`(=512) 导致 bus-bw worker 死循环

### 12.1 符号化结论（提案要求的第一步）

用本次构建的 `wlan.ko`（未剥离，带调试信息）反汇编 `hdd_bus_bw_work_handler+0x3e0`：

```asm
; wlan_hdd_main.c:11147  if (!hdd_ctx->config->mscs_pkt_threshold) return;
546a8: ldr  x0, [x19, #104]        ; hdd_ctx->config
546ac: ldr  w0, [x0, #10224]       ; config->mscs_pkt_threshold
546b0: cbz  w0, 546b8              ; ==0 -> 跳过
; wlan_hdd_main.c:11150  for (cpu = 0; cpu < NUM_CPUS; cpu++)
546b4: b    546b4                  ; ★ 自跳转 = 编译器证明的无限循环
```

源码（`hdd_send_mscs_action_frame()`）：

```c
uint8_t cpu;                                    /* ← 0..255 */
for (cpu = 0; cpu < NUM_CPUS; cpu++)            /* NUM_CPUS = NR_CPUS = 512 */
	tx_vo_pkts += stats->per_cpu[cpu].tx_classified_ac[SME_AC_VO];
```

`CONFIG_NR_CPUS=512`（本机内核 `.config`）+ `#define NUM_CPUS NR_CPUS`（`wlan_hdd_main.h:250`）
⇒ `uint8_t` 在 255 回绕，`cpu < 512` **永真** ⇒ 编译器生成 `b .`（无限自旋）。

`mscs_pkt_threshold` 默认 **1200**（非 0，`hdd_dp_cfg.h:373`），所以**每个已连接 STA 的
bus-bw worker 都会进入该死循环**——这也解释了为什么设备一直有 ~1.0 的 load average，
以及为什么 `qdf_periodic_work_stop_sync()`（`cancel_delayed_work_sync` 等活跃 handler 退出）
永远等不到。

### 12.2 修复

```diff
--- a/core/hdd/src/wlan_hdd_main.c   (hdd_send_mscs_action_frame)
-	uint8_t cpu;
+	/* sky: uint8_t wraps at 256 while NUM_CPUS == NR_CPUS (512) -> infinite loop */
+	int cpu;

--- a/core/hdd/src/wlan_hdd_wmm.c     (两处 cpu < NUM_CPUS 循环)
-	uint8_t cpu;
+	int cpu;
```

（`grep -n "NUM_CPUS" core/hdd/src/*.c` 只有这三处循环；全部已修。）

### 12.3 上机验证（实测，全部通过）

```
[5/6] -> hdd_handle_disassociation_event
[5/6] <- hdd_handle_disassociation_event      ← 16 ms 返回（修复前永不返回）
[6/6] -> / <- hdd_ipa_set_tx_flow_info

scheduler_watchdog               : 0
"Disconnect timeout with status 15": 0
wpss state                       : running（不再崩溃、不再 SSR）
load average（修复前 ~1.0）      : 0.14
重新 up + reassociate            : wpa_state=COMPLETED, inet 192.168.1.186/24
ping 1.1.1.1                     : 3/3, 0% loss, 4.5–5.7 ms
两次断连的 CM 响应计数            : #0, #1 —— 每次请求恰好 1 次，无重复响应
```

### 12.4 最终修复清单（本次 WiFi 主线化全过程）

| # | 问题 | 修复 |
|---|---|---|
| 1 | icnss2 `Fail to get IRQ-0` | `platform_get_resource` → `platform_get_irq` |
| 2 | qcacld `cfg_parse` 失败 → probe -1 | ini 放 `/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini` |
| 3 | NAPI 注册时 oops | `INIT_LIST_HEAD(&dev->napi_list)` + `reg_state` |
| 4 | 3 条 NAPI teardown WARNING | `napi_disable()` before `netif_napi_del()` |
| 5 | BDF/REGDB 下载失败 | 固件文件被 USB 传成全零（非代码问题）→ md5 校验 |
| 6 | **断连卡死 → watchdog → FW assert → SSR -110** | **C2：OSIF 通知移出 CM SM 锁（异步）** |
| 7 | **bus-bw worker 死循环（本次根因）** | **`uint8_t cpu` → `int cpu`（3 处）** |

> 说明：C2 与 #7 是两个独立缺陷。#7 是断连路径"永不返回"的直接原因；
> C2 修的是"OSIF 通知在 CM 状态机锁内同步执行"这一结构性隐患（提案结论），
> 两者都保留。

### 12.5 产物

```
wlan.ko  62,487,1xx B   md5 92649de62b63f9d9de76293ebaf0ef47（含 C2 + NR_CPUS 修复 + 探针）
备份     wlan_hdd_main.c.bak-nrcpus-20261104 / wlan_hdd_wmm.c.bak-nrcpus-20261104
         wlan_hdd_cm_disconnect.c.bak-c2c-20261104 / wlan_cm_disconnect.c.bak-c2-20261104
```

---

## 13. ✅ 固化：开机零干预自启达成（含 `sleep 20`、`&&` 串行、guard 自愈）

### 13.1 改动

| 文件 | 改动 |
|---|---|
| `/etc/init.d/sky-wifi` | `start()` 里 `sleep 10` → **`sleep 20`**；`start_pre()` 的 guard 改为**只在 pstore 有真实崩溃证据时才跳过**（`Internal error`/`Unable to handle kernel`/`kernel BUG at`/`Kernel panic`），否则清 marker 后**本次开机直接重试**；`start()` 结束时清 marker |
| `/usr/local/sbin/sky-wifi-load.sh` | 三个 `insmod` 改为**严格 `&&` 串行**：`load_mod(wlan_firmware_service) && load_mod(icnss2) && wait_fw_ready && load_mod(wlan)`；任一环节失败整链中止并记日志；新增 `ensure_default_route()`（链路抖动后自动补回 `default via <gw> dev wlan0`） |

两份文件的当前版本已归档到本目录 `install/sky-wifi-load.sh`、`install/sky-wifi.initd`。

### 13.2 验证（重启后零干预，实测）

```
$ rc-service sky-wifi status
 * wlan0 is up (192.168.1.186/24)

$ tail -10 /log/wifi-load.log
sky-wifi-load: wlan_firmware_service loaded
sky-wifi-load: icnss2 loaded
sky-wifi-load: WLAN FW is ready: 0x487
sky-wifi-load: wlan loaded
sky-wifi-load: wlan0 up
sky-wifi-load: power_save off
sky-wifi-load: wpa_supplicant started
sky-wifi-load: wpa_state=COMPLETED
sky-wifi-load: dhcp: 192.168.1.186/24
sky-wifi-load: === done ===

wpa_state=COMPLETED   wlan0 192.168.1.186/24   default via 192.168.1.1
ping 1.1.1.1: 2/2, 0% loss, 4.1-5.9 ms
```

从冷启动到可上网全自动，无需人工 `rc-service` 干预。

### 13.3 行为说明

* `sleep 20`：给 WPSS remoteproc / udev / 其它 init 服务留出稳定时间后再 insmod
* `&&` 串行：`wlan_firmware_service` → `icnss2` →（等 `WLAN FW is ready`）→ `wlan`，
  前一步失败绝不继续，避免半加载状态
* guard：真的崩溃（pstore 有证据）→ 跳过本次、下次再试；只是被手动重启打断 → 同次开机重试
* 备用恢复通道：`usb0 = 192.168.7.1/24` 始终可用（WiFi 挂了也能 SSH 进去）
