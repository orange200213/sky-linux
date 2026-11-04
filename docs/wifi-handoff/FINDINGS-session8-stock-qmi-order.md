# Session 8: 原厂 WLFW QMI 顺序的源码+二进制双重取证（子代理分析）

分析对象：`sky_jp_global_images_OS1.0.13.0.UMWJPXM_14.0` 原厂系统
（HyperOS 14，kernel 5.10.198）+ `backup/stock-jp-2026-11-02/{jp_icnss2.ko,jp_qca_cld3_adrastea.ko}`
+ 同版本源码 `sky-lineage/android_kernel_xiaomi_sky@lineage-23.2`。

## 1. 四条硬结论

1. 设备走 **`ADRASTEA_DEVICE_ID`(0xabcd)** 分支（`qcom,icnss` → `icnss_platform_id_table[1]`）。
   该分支**不发 `WLFW_HOST_CAP_REQ`，也不发 `WLFW_DEVICE_INFO_REQ`**（那两条只在 0x6750/0x6450 分支）。
2. **`WLAN_INI` / `WLAN_CFG` / `WLAN_MODE` 三条都严格晚于 `WLFW_FW_READY_IND`**，
   顺序固定 INI → CFG → MODE，调用方各有硬前置 `test_bit(ICNSS_FW_READY)`：
   - `icnss_set_fw_log_mode()`（INI）`main.c:3162-3171`
   - `icnss_wlan_enable()`（CFG+MODE）`main.c:3360-3388`
   ⇒ 我们在 FW_READY 之前发 CFG/MODE 收到 **90 INCOMPATIBLE_STATE** 完全符合原厂逻辑。
3. **第一次 WCSS 寄存器访问（CE init）在 `WLAN_MODE` 被 ACK 之后**：
   `FW_READY_IND` → `icnss_call_driver_probe` → qcacld probe → `hdd_wlan_start_modules`
   → `pld_set_fw_log_mode`(INI) → `hdd_hif_open` → `hif_enable` → `hif_snoc_bus_configure`
   → `hif_wlan_enable`(CFG+MODE) → **`hif_config_ce`（首次 MMIO）**。
4. **icnss2 里没有任何"解锁 APSS 访问 WCSS 寄存器"的代码**（复核：无 writel/iowrite 调用、
   无 `0x32060/0x300e0/0x30000` 常数、无 SR_APSS/remap/ATC 串）。**解锁是 WPSS 固件自己做的**，
   host 唯一的杠杆是把 QMI 走到 `WLAN_MODE` 并被 ACK。

## 2. 大发现：原厂 ADRASTEA build 启用 `ADRASTEA_SHADOW_REGISTERS`

* `ce_reg.h:541-560`：在该宏下 `CE_SRC_RING_WRITE_IDX_SET` / `CE_DEST_RING_WRITE_IDX_SET`
  被改写成写 **shadow register**（`shadow_sr_wr_ind_addr()` / `shadow_dst_wr_ind_addr()`，共 24 槽），
  而不是写 `CE_ctrl_addr + SR_WR_INDEX_ADDRESS`。
* 本 build **确实启用**：`jp_qca_cld3_adrastea.ko` 里有
  `"%s: Using Shadow Registers instead of CE Registers"`（仅存在于 `ce_main.c:4380` 的
  `#ifdef ADRASTEA_SHADOW_REGISTERS` 块），且 `hif_config_ce`(0x38fb14) 里有
  `bl __log_read_mmio` @0x38fcdc（对应 `ce_main.c:4385` 的 `A_TARGET_READ(scn, SHADOW_ADDRESS(i))`）。
* 与之配套，ADRASTEA 的 `WLAN_CFG` 必须带 **`shadow_reg`(v1)**（`icnss2/qmi.c:3217-3225`），
  而 **WCN6750 才用 `shadow_reg_v2`**（`qmi.c:3206`）、WCN6450 用 `v3`。

⇒ 这正好对上我们 session 3/4/5 的实测：SR_APSS 的 `ADDRESS_VALID=0`、`SHADOW_ADDRESS` 只读、
`SHADOW_VALUE` 可写但没被转发。**槽位是固件根据 WLAN_CFG 里的 shadow_reg 表配置的**，
我们发的 CFG 里没带这张表（或带的是 v2 变体），所以槽位永远是 0。

## 3. FW_READY 前后时间线（原厂 dmesg，含 state 位解码）

```
7.206  QMI Server Connected: state 0x480      (bit7 SSR_REG + bit10 WLFW_EXISTS)
7.331  adrastea/regdb.bin 下载
7.362  adrastea/bdwlant_jp.bin 下载
7.390  Sending cal report request, state 0x483 (+bit0 CONNECTED +bit1 POWER_ON)
        ↓ 固件自己做初始化，host 完全静默（期间零 WCSS 寄存器访问）
10.300 DMS service connected, state 0x200483 (+bit21 QMI_DMS_CONNECTED)
10.320 WLAN FW is ready: 0x200487             (+bit2 FW_READY)  ⇒ 间隔 2.93 s
10.452 pld_get_user_msi_assignment: Not supported on type 1
10.633 wlan_mac.bin → wlan0
```

## 4. 对我们移植的改动清单（对齐原厂）

| 项 | 原厂 | 我们现在 | 动作 |
|---|---|---|---|
| HOST_CAP | **不发** | `snoc_host_cap=1` 发 | 关掉（默认 0） |
| DEVICE_INFO | **不发** | `snoc_device_info` 实验开关 | 关掉（已实测 94） |
| BDF 时机 | `msa_ready` 之后紧接 | 等 `MSA_READY` 事件才发 | 改为 server_arrive 里紧跟 |
| INI/CFG/MODE | FW_READY 之后（`firmware_start`） | 已在 `firmware_start`（正确），但额外有 pre/post-bdf 实验开关 | 删掉 pre/post 实验开关 |
| CFG 内容 | `shadow_reg`(v1) + 24 槽表 | `shadow_reg_valid=0`，且用 `shadow_reg_v2` | **改成 v1 并填 `target_shadow_reg_cfg` 表** |
| 首次 CE MMIO | MODE ACK 之后 | CFG/MODE 失败后仍继续到 CE/DP | 失败即 abort，不要吞错 |
| FW_INIT_DONE | 原厂固件**不发**该 indication | 有 `ATH11K_QMI_EVENT_FW_INIT_DONE → firmware_ready` 分支 | 不要依赖 |
| 超时 | cal report→FW_READY ≈2.93 s | - | 等待 FW_READY 超时 ≥5 s |

`target_shadow_reg_cfg` 表（`qca-wifi-host-cmn/hif/src/ce/ce_main.c:510-533`）：

```
CE 源 ring: 0→0, 3→3, 4→4, 5→5, 7→7         （偏移 0x3C = SR_WR_INDEX）
CE 目的 ring: 1→13, 2→14, 5→17, 7→19,
              8→20, 9→21, 10→22, 11→23      （偏移 0x40 = DST_WR_INDEX）
```

## 5. 下一步实验（k95），顺序很重要

1. 关掉 `snoc_host_cap` / `snoc_device_info` / `snoc_pre_bdf` / `snoc_post_bdf` 全部实验开关。
2. `ath11k_qmi_wlanfw_wlan_cfg_send()` 改为填 **`shadow_reg`(v1)**：`shadow_reg_valid=1`、
   `shadow_reg_len=12`、表内容用上面 12 条 `{ce_id, offset}`。
3. 保留现状（FW_READY → firmware_start → INI/CFG/MODE → CE）不变，**但在 CE/DP 之前加日志**，
   并在 `ath11k_qmi_firmware_start` 返回非 0 时**立即停止**（不要继续 CE/DP）。
4. 用 `skyprobe` 在系统起来后读 `0x228300e0`（ADDRESS_VALID）：
   **若变为非 0，说明固件按 shadow 表配置了槽位 → 机制被证实**，
   之后 CE 的 write-index 就会走 SR 影子窗口，而不再碰 0x240000。
5. DP（WBM/REO/TCL）另论：原厂 adrastea 驱动**没有 HAL、不编程 DP ring**，
   所以 mainline ath11k 的 `ath11k_dp_alloc()/ath11k_dp_srng_setup()` 在这颗芯片上
   必须禁用或彻底改造。这一步要等 CE 打通之后再评估。

---

## 6. 现在有两种解释，k95 的探针可以区分它们

背景：我们在主线内核上做过时间采样（session 6 §2.2），bring-up 全程
`0x22824000 / 0x228B4874 / 0x23234000` 都是 DEAD；而且 `0x22800000..0x22820000`
整段（SI 0x84000 / RTC 0x45000 / SOC_CORE 0x113000 / CE wrapper 0x24C000）全 DEAD，
**只有 SR_APSS（0x22830000/0x22832000）可读**。

另外：在安全配置下我们的 `ath11k_qmi_firmware_start()`（INI→CFG→MODE）**是成功返回的**
（否则 `ath11k_core_qmi_firmware_ready` 会在 CE/DP 之前就退出，而我们观察到 abort 发生在
`ath11k_dp_alloc`）。也就是说"MODE 已 ACK"这个条件我们其实满足过。

### 解释 A：固件在 WLAN_MODE 之后才把 A_WCSS_HM 区开放给 APSS
那么好时刻之后 SI/CE 页应该可读。但我们没观察到（采样覆盖了 abort 前的时间点）。
仍可能是采样窗口/时刻问题（例如解锁发生在 CE init 之后、abort 之前的一瞬间）。

### 解释 B（新，可能更对）：主机本来就不该访问 A_WCSS_HM 区的绝大部分
原厂 `ADRASTEA_SHADOW_REGISTERS` 把 CE 的 **write-index** 改写成写 SR 影子槽，
说明"主机能碰的寄存器"被刻意限制在 **SR_APSS 窗口**里（这正好是我们唯一能读的块）。
CE 的 ring base/size 等仍走 `CE_ctrl_addr`，但那部分可能被固件预先配置好，
主机只写 shadow 槽即可驱动 CE。

### 判别实验（k95，成本低）

1. 按 §4 的 12 条表把 `WLAN_CFG` 改成发 `shadow_reg`(v1)；
2. 起来后立刻 `skyprobe rd 0x228300e0`：
   * **ADDRESS_VALID 变非 0** → 解释 B 成立：固件按表配置了槽位，
     CE 走影子窗口，A_WCSS_HM 区对主机确实不可用（这是设计，不是缺使能）。
     此时把 CE 的 write-index 操作改到 `SHADOW_VALUE[n]`（0x32000+4n，实测可写）
     就能推进到 HTC；
   * **仍是 0** → 说明 CFG 没被固件接受/表格式不对，回到解释 A 继续查解锁时机。
3. 只有 CE 通了才谈 DP：原厂**完全不编程 DP ring**，mainline 的
   `ath11k_dp_alloc()/ath11k_dp_srng_setup()` 在这颗芯片上要么禁用、要么彻底改造。
