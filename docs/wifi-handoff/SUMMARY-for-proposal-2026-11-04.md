# WiFi 现状小结（给提案分析用）— 2026-11-04

设备：Xiaomi Redmi 12 5G（`sky`，SM4450 ravelin）+ WCN6750 `adrastea`（SNOC），
内核 **mainline Linux 7.3**（`7.3.0-g78458d4eea4e-dirty`），非 Android 用户空间（Alpine 3.21 rootfs）。

---

## 一、路线结论（已定，不再讨论）

mainline **ath11k 在本机不可行**：APSS 只能访问 SR_APSS 段（`membase+0x20000..0x52000`）
和最多 24 个固件配置的影子槽（`SHADOW_VALUE[n]`，实测 `ADDRESS_VALID=0xf860b9` 12 槽，
只覆盖 CE 写指针 0x3C/0x40）；CE ring base/size、UMAC/WBM/REO/TCL 全部不可达；
零 MMIO 实测证明固件不会替主机建 CE 环。

⇒ 采用 **qcacld-3.0（adrastea profile）+ icnss2** 并主线化到 Linux 7.3。

## 二、已经跑通的功能（实测）

```
icnss2 probe OK → QMI(msa_mem_info/msa_ready/cap) OK
REGDB + BDF 下载 OK → cal report OK → "WLAN FW is ready: 0x487"
wlan.ko 加载 → wlan0 (managed) 出现，MAC 00:0a:f5:a0:f6:c7
wpa_supplicant 2.10(-D nl80211)：WPA2-PSK 关联成功 (TP-Link_FD895G)
udhcpc：192.168.1.186/24；ping 1.1.1.1：0% 丢包，4–16 ms
```

## 三、已修复的 5 个真实问题

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `icnss2: Fail to get IRQ-0` | ADRASTEA 分支用 `platform_get_resource(pdev, IORESOURCE_IRQ, i)`，平台设备资源表为空 | 改 `platform_get_irq(pdev, i)` |
| 2 | probe 返回 `-1` 且无任何日志 | `hdd_context_create()` → `cfg_parse("wlan/qca_cld/WCNSS_qcom_cfg.ini")` 失败（status=16）；ini 只在 `.../qca_cld/adrastea/` | ini 放到 `/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini`（+ `/vendor/firmware/...`） |
| 3 | probe 在 `netif_napi_add_weight_locked` oops | `qca_napi_info` 内嵌 `net_device` 从未初始化，7.3 会操作 `dev->napi_list`；`init_dummy_netdev()` 未导出 | 手工 `INIT_LIST_HEAD(&dev->napi_list)` + `reg_state = NETREG_UNREGISTERED` |
| 4 | 3 条 `__netif_napi_del_locked` WARNING | 7.3 语义：`NAPI_STATE_SCHED` 置位=已 disable，删除时若仍 enabled 就告警；`hif_deinit_rx_thread_napi()` 没先 `napi_disable()` | 补 `if (!test_bit(NAPI_STATE_SCHED, …)) napi_disable(…);` 实测 0 条 |
| 5 | BDF/REGDB 下载 `result=1, err=1` | **不是代码问题**：USB 链路不稳导致固件文件"大小正确、内容全零" | 换线 + 逐文件 md5 双向校验；脚本里加非空检查 |

## 四、当前唯一功能性阻塞：断连卡死 → SSR 恢复失败

### 4.1 复现

```sh
rc-service sky-wifi start        # 或 /usr/local/sbin/sky-wifi-load.sh
wpa_cli -i wlan0 status          # wpa_state=COMPLETED
ip link set wlan0 down           # = 用户态 disconnect
```

### 4.2 日志（实测）

```text
[162.395] [PE]      Deauth TX: vdev 0 seq_num 2051 reason 3 waitForAck 1 to 78:8c:b5:82:fd:88
[162.411] sky-dbg7  mgmt_txrx_compl desc=63 status=0 tx_cb=0 ota_cb=1 ctx=1
[162.414] sky-dbg9  deauth_tx_complete_cnf_handler tx_success=0 buf=yes
[162.420] sky-dbg9  lim_deauth_tx_complete_cnf tx_success=0
[162.455] OSIF      wlan0(vdevid-0) locally-generated disconnect cm_id 0xd000002
[179.432] SCH       scheduler_watchdog_notify: Callback cm_handle_disconnect_resp exceeded 10s
[179.44 ] SCH       scheduler_watchdog_timeout: Triggering self recovery
[179.45 ] QDF       cds_trigger_recovery_handler: critical host timeout → fw recovery (reason 31)
[179.46 ] icnss2    Received early crash indication from FW
[181.25 ] remoteproc1 crash detected in wpss: type watchdog → recovering wpss
[181.26 ] icnss2    WPSS went down, crashed: 1 → PD_SERVICE_DOWN → SERVER_EXIT
[186.75 ] qcom_q6v5_pas start timed out / remoteproc1: can't start rproc wpss: -110
[192.85 ] CMN_MLME  cm_disconnect_start_req_sync: Disconnect timeout with status 15
[244.05 ] icnss2    Timeout waiting for FW Ready
```

### 4.3 已排除的怀疑（都有实测证据）

| 怀疑 | 证据 |
|---|---|
| `HTT_T2H_MSG_TYPE_MGMT_TX_COMPL_IND` 丢失 | 该平台 mgmt 完成走 **WMI**：`WMI mgmt_tx_compl desc_id=63 status=0`，帧内容 `fc=0x00c0`=deauth。HTT 那条 0 次属正常 |
| `pdev->tx_mgmt_cb.ota_ack_cb` 为 NULL | `sky-dbg5: mgmt_tx_cb_set type=0 download_cb=1 ota_ack_cb=1`；且 deauth 完成时 `tx_cb=0 ota_cb=1` |
| `desc_id` 恒为 63 = 描述符表错乱 | auth(0x00b0)/assoc(0x0000)/deauth(0x00c0) 三帧内容都正确，属固件复用 ID |
| 数据面 RX/TX 完成链路 | ping 期间 `sky-dbg3: tx_compl #0..#7 status=0`，DHCP/EAPOL 收发正常 |

### 4.4 卡点定位（下一步只剩确认 + 改一行）

链路已逐段验证到：
`lim_deauth_tx_complete_cnf_handler` → `lim_deauth_tx_complete_cnf()` → **`lim_send_deauth_cnf()`**

```c
QDF_STATUS lim_send_deauth_cnf(struct mac_context *mac_ctx)
{
	deauth_req = mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq;
	if (deauth_req) {          /* ← 为 NULL 则整段跳过：不停 timer、不发 cnf */
		…
	}
	…
}
```

`pMlmDeauthReq` **只在 legacy** `lim_process_mlm_deauth_req()`（`lim_send_management_frames.c:3523`）里赋值。
CM 路径为：
`lim_cm_handle_disconnect_req()` → `lim_process_nb_disconnect_req()` →
`lim_prepare_and_send_deauth()` → `lim_process_disconnect_sta(&msg)`，
**是否会走到 `lim_process_mlm_deauth_req()` 尚未确认**。

若没走到 ⇒ `lim_send_deauth_cnf()` 什么都不做 ⇒ `cm_handle_disconnect_resp()` 永不被调用
（该函数只有两个来源：`lim_process_sme_req_messages.c:4257` 的"无 session 快速路径"，
和 `lim_send_sme_rsp_messages.c:836` 的 `eWNI_SME_DISCONNECT_DONE_IND` 处理）

## 五、待提案决策的问题

**Q1（主）** CM 断连的完成响应应该由谁发出？三个候选：
* A. 在 CM 路径直接调 `lim_cm_send_disconnect_rsp(mac_ctx, vdev_id)`
  （函数已存在于 `lim_send_sme_rsp_messages.c:761`；vdev_id 在
  `lim_prepare_and_send_deauth()` 里可得）——即 CM 断连不再依赖 legacy 的
  `pMlmDeauthReq`/`pMlmDisassocReq` 机制
* B. 在 `lim_prepare_and_send_deauth()` 里把请求塞进
  `mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq`，让 legacy cnf 流程照常跑完
* C. 其它（例如在 `lim_process_disconnect_sta()` 里补齐）

**Q2** `waitForAck` 语义是否还要保留？现在 ACK 完成回调拿到 `tx_success=0` 后只发 legacy cnf，
CM 侧无感知。是否应改成"deauth 发出即完成 CM 断连，ACK 只用于统计"？

**Q3（次）** SSR 恢复失败：`remoteproc1: can't start rproc wpss: -110`（超时）。
icnss2 的 `PD_SERVICE_DOWN(5)` 处理里是否缺少完整的 WPSS 断电→重新 `rproc_boot` 流程？
（现在只能重启恢复；mainline 上 `qcom_q6v5_pas` 的 `start` 超时 5s）

**Q4** 开机自启失败：`sky-wifi` 服务在 boot 时 openrc 报 `failed to start`，
但 `/log/wifi-load.log` 里**没有该次运行的记录**，`/var/lib/sky-wifi/` 也空 ⇒
失败发生在脚本之前（guard 检查/依赖阶段）。手动 `rc-service sky-wifi restart` 正常。
已在 `start()` 里加 `sleep 10`，但**尚未验证重启后是否解决**。

**Q5** 调试插桩清理策略：目前源码里有 7 组 `sky-dbg3..9`（`grep -rn sky-dbg core/` 可定位），
提案时可考虑改成编译开关或 `dynamic_debug`。

## 六、环境与运维事实（影响方案可行性）

| 项 | 事实 |
|---|---|
| 模块卸载 | `CONFIG_MODULE_UNLOAD=n`，`rmmod` 报 `Function not implemented` ⇒ **一次开机只能加载一次**，改模块必须重启 |
| 重启方式 | `reboot` 会挂；用 `sync; sync; sleep 1; echo b > /proc/sysrq-trigger` |
| 偶发落 fastboot | `fastboot flash boot_a sky-mainline/shim/out/sky-shim-boot-v4.img`（120,086,528 B，内嵌 DTB sha `b1b0d1d7…`）+ `fastboot reboot` |
| USB 传输 | 链路不稳时文件"大小对、内容全零"，**任何推送都要 md5 双向校验** |
| 时钟 | RTC 从 1970 开始，`apk` 会因 TLS 失败（表现为 Permission denied）⇒ 先 `date -u -s` |
| WPSS 固件 | `wpss` remoteproc 在 1.9 s 被内核 probe（此时 `/lib/firmware` 未挂载）→ `wpss.mbn` ENOENT 失败；加载脚本现在会**主动** `echo start > /sys/class/remoteproc/*/state` |
| rootfs | `/dev/sda20` ext4（106 GiB）。**注意**：内核无 F2FS，TWRP 格式化时必须选 ext4 |
| 备份 | `sky-rootfs-20261104-v2.tar`(补全 proc/sys/tmp 目录) + `-v2.tar.zst`，944→948 条目/356 symlink |

## 七、代码与产物清单

```
源码  third_party/micode-sky-icnss2/           icnss2 + wlan_firmware_service_v01.{c,h} + port.mk
      third_party/micode-sky-t-oss/qcacld-3.0/ qcacld（含 7 组 sky-dbg 插桩）
      third_party/micode-sky-t-oss/qca-wifi-host-cmn/
产物  icnss2.ko 809,992 / wlan_firmware_service.ko 134,920 / cnss_utils.ko 79,840
      wlan.ko 62,486,7xx（当前 md5 1f9660d89f3c00f1ec36812ab553875c，含 dbg3~dbg9）
DT     dts/sm4450-xiaomi-sky.dts（icnss 节点+smp2p 三路+MSA 12MiB+触屏修复）
仓库  github.com/orange200213/sky-linux（docs/wifi-handoff/*）
      github.com/orange200213/sky-qcacld-3.0-mainline（源码+模块+文档备份）
文档  docs/wifi-handoff/FINDINGS-qcacld-device-test-2026-11-04.md（§1–22）
      docs/wifi-handoff/HANDOFF-qcacld-rx-2026-11-04.md（断连问题专题，173 行）
      docs/wifi-handoff/SUMMARY-for-proposal-2026-11-04.md（本文）
```

## 八、复现/验证命令

```sh
# 上机（每次重启后）
rc-service sky-wifi start            # 内含 sleep 10 + WPSS 拉起 + 三个 insmod + wpa + dhcp
tail -12 /log/wifi-load.log
wpa_cli -i wlan0 status | grep -E 'wpa_state|ssid'
ping -c3 1.1.1.1

# 复现断连卡死
ip link set wlan0 down && sleep 13 && dmesg | grep -E "scheduler_watchdog|sky-dbg9|Disconnect timeout"

# 检查 NAPI 告警（应为 0）
dmesg | grep -c netif_napi_del_locked
```
