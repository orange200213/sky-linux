# Wi-Fi 移植记录：Adrastea SR_APSS shadow 端口（session 3）

日期：2026-11-01  
设备：Xiaomi Redmi 12 5G（sky），SM4450/ravelin，WCN6750/adrastea  
目标：mainline `ath11k` SNOC，不使用 downstream `qcacld-3.0`。

## 1. 本轮结论

本轮拿到并分析了 Qualcomm/Adrastea 的公开寄存器定义：

- 参考文件：[`adrastea_reg_def.h`](https://github.com/SpaceExplorationTechnologies/starlink-wifi/blob/1d585ee8909bf7bab4391c0c52d8a9d44137b3bb/qca/src/qca-wifi/cmn_dev/hif/src/adrastea_reg_def.h)
- 文件明确描述了 SR_APSS shadow 端口，而不是 ath11k HAL 的 DP shadow ring 区域。
- 之前实验代码读取 `0x300e4` 作为 status 是错误的：该地址不在文件定义的 SR_APSS 状态寄存器中，k88 已实测在该读取处产生 synchronous external abort。
- k89 的 mode 5 仍不是有效实现；之后设备通过 `/etc/sky-wifi-params` 使用 `snoc_reg_mode=4` 恢复到安全路径。
- 当前设备仍能启动、SSH 可用，但 Wi-Fi 在 `ath11k_hal_srng_setup()` 写入 UMAC offset `0xa34874` 时外部 abort，WPSS 报 `_CE_misc_handler:2227`。

## 2. 已实测的工作链路

以下步骤在设备上成功：

```text
WPSS remoteproc running
WLFW QMI instance 0
host_cap + ddr_range
MSA mem_info / msa_ready
chip_id 0x4130, chip_family 0x4007
regdb + BDF download
cal report
CE0..CE11 direct register area initialization
```

代表性日志：

```text
ath11k 22800000.wifi: snoc mode: membase registers, per-CE interrupts
ath11k 22800000.wifi: host cap ddr_range: 0+100000000, 82a00000+300000
ath11k 22800000.wifi: msa 0x82a00000+0x300000, 2 region(s)
ath11k 22800000.wifi: chip_id 0x4130 chip_family 0x4007 board_id 0xff soc_id 0x406a0000
ath11k 22800000.wifi: cal report sent
```

阻塞：

```text
ath11k_hal_srng_setup()
  -> ath11k_ahb_window_write32_snoc()
     offset 0xa34874
Internal error: synchronous external abort: 96000010
WPSS: cmnos_thread.c:4003: Asserted in copy_engine.c:_CE_misc_handler:2227
```

## 3. 重要区分：两种 shadow 不是同一个东西

### 3.1 ath11k HAL shadow ring

mainline ath11k 的：

```c
hal_shadow_base_addr
HAL_SHADOW_REG(ab, x)
shadow_reg_v2[]
ath11k_hal_srng_shadow_config()
```

用于 CE/DP ring 的 head/tail shadow 配置，并通过 QMI WLAN CFG 发送给固件。WCN6750 的 `hal_shadow_base_addr` 是 `0x504`。这不是把任意 UMAC MMIO 写请求转发给 WCSS 的硬件窗口。

### 3.2 Adrastea SR_APSS shadow MMIO port

`adrastea_reg_def.h` 中的 SR_APSS 是 APSS 主机访问 WCSS 的硬件 register-port，包含 24 个 address/value slot、valid 位、dirty 位、状态和错误寄存器。当前 SNOC 移植真正缺少的是这一层。

## 4. Adrastea SR_APSS 寄存器定义

根据参考文件，关键地址如下：

| 寄存器 | 地址 | 说明 |
|---|---:|---|
| `SR_CONTROL` | `0x30000` | reset/clock/full APSS access 等控制位 |
| `SHADOWREG_STATUS` | `0x30008` | shadow port 状态，低 4 位 state，bit 3 为 WCSS core wake/sleep |
| `SR_INTERRUPT_STATUS` | `0x3000c` | dirty/error/invalid/timeout 等状态 |
| `SR_INTERRUPT_ENABLE` | 参考定义 | dirty/force-wake/error/timeout 中断使能 |
| `SR_INTERRUPT_CLEAR` | `0x30014` | 清除 dirty、force wake、error、invalid 等状态 |
| `SHADOW_ADDRESS0..23` | `0x30020 + 4*n` | 22-bit WCSS 地址，mask `0x003fffff` |
| `DIRTY` | `0x30080` | 24-bit dirty 位图，表示 shadow value 更新/脏状态 |
| `ADDRESS_VALID` | `0x300e0` | 24-bit slot valid 位图 |
| `SHADOW_VALUE0..23` | `0x32000 + 4*n` | 24 个 32-bit shadow value |
| `FORCE_WAKE` | `0x32060` | force wake bit 0 |
| `SW_SCRATCH` | `0x32064` | 软件 scratch |
| `WCSSAON_SR_LSB` | `0x32070` | WCSS AON SR 状态/地址低位定义 |
| `WCSSAON_SR_MSB` | `0x32074` | WCSS AON SR 状态/地址高位定义 |
| `INVALID_ADDR_ACCESS` | `0x32078` | invalid address + read/write access 信息 |

关键字段（来自参考头文件）：

```text
SHADOW_ADDRESSn mask                 0x003fffff
ADDRESS_VALID mask                   0x00ffffff
DIRTY mask                           0x00ffffff
SHADOWREG_STATUS mask                0x0000000f
SR_INTERRUPT_STATUS.DIRTY_BIT_SET   bit 0
SR_INTERRUPT_STATUS.ERR_RESP        bit 2
SR_INTERRUPT_STATUS.ECAHB_TIMEOUT   bit 4
SR_INTERRUPT_STATUS.INVALID_ADDR    bit 5
SR_INTERRUPT_STATUS.VALUE_INVALID   bit 5（具体命名以生成头文件为准）
INVALID_ADDR_ACCESS.WRITE_ACCESS    bit 17
```

注意：`0x300e4` 没有出现在参考头文件的 SR_APSS 寄存器地址定义中，不能作为 status 读取地址。

## 5. 当前代码状态

修改位置：

```text
sky-mainline/kernel/linux-7.3/drivers/net/wireless/ath/ath11k/ahb.c
sky-mainline/kernel/linux-7.3/drivers/net/wireless/ath/ath11k/ahb.h
```

已经完成并验证：

- SNOC 模式、instance 0、MSA、BDF、cal report 等前置 bring-up；
- 异步 probe，避免从 PID 1 bind 路径访问坏 MMIO；
- CE direct register map `0x240000 + n*0x1000`；
- `snoc_probe_base` 单基址参数；
- mode 5 的实验性函数目前只打印 `unsupported` warning，不执行危险 shadow MMIO；
- mode 4 是当前安全运行配置。

当前设备参数：

```text
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
```

设备内核运行状态：

```text
7.3.0-g78458d4eea4e-dirty #88
```

## 6. 下一步实现要求

不能再猜测单个 slot 的 `ADDRESS_VALID` 握手。应从参考文件中恢复 stock host-common 的完整顺序：

1. 确认 `SR_CONTROL` 是否需要设置 `ENABLE_APSS_FULL_ACCESS`（bit 2），以及 clock gate/soft reset 的状态。
2. 用 `SHADOWREG_STATUS`（`0x30008`）确认 WCSS core wake/sleep 和 state，而不是读取 `0x300e4`。
3. 选择一个 slot（优先 slot 0）：
   - 写 `SHADOW_ADDRESS0 = target & 0x003fffff`；
   - 写 `SHADOW_VALUE0 = value`；
   - 设置 `ADDRESS_VALID` 对应 bit；
   - 读取 `DIRTY`、`SR_INTERRUPT_STATUS`、`INVALID_ADDR_ACCESS`，只访问参考文件定义的 APSS 本地地址。
4. 先用已知无害 scratch/version 地址做读回验证；不要先写 `0xa34874`。
5. 对 read path 必须确认 stock 实现如何区分 `SHADOW_VALUE` 的读回值和普通写值；当前“写 value=0 再读 value”实现无效，已删除/禁用。
6. 只有 harmless read/write 验证成功后，才将 `ath11k_window_write32_snoc()` 的 UMAC/WBM/REO/TCL 写请求切换到 shadow port。
7. 另行处理 SNOC 的 DP extended interrupts；当前 `ath11k_ahb_config_irq()` 仍明确跳过 ext IRQ，不能把“越过 ring setup”误认为 Wi-Fi 已完成。

## 7. 失败实验记录

### k84

probe 在 bind/PID 1 路径执行，访问坏寄存器导致 panic。结论：probe 必须移到 workqueue。

### k85

错误使用 `wifi-fdt.dtb`，启动成混合节点 `17210040.wifi`，QMI DEVICE_INFO 失败。结论：必须使用 `wifi-snoc-fdt.dtb`。

### k86/k87

正确 SNOC DTB；QMI/MSA/BDF/cal report 全成功；直接静态窗口访问 UMAC 在 `0xa34874` abort。

### k88

mode 5 实验读取错误的 `0x300e4` status，worker 在读取该地址时 abort。

### k89

删除 `0x300e4` 读取后的实验版本；设备曾使用 mode 5 反复重启。随后用 `snoc_reg_mode=4` 恢复，确认安全路径仍是原始 UMAC abort。

## 8. 交接给下一轮

当前最有价值的新事实不是“继续试几个物理 base”，而是：

- Adrastea SR_APSS 公开寄存器定义已经找到；
- `SHADOWREG_STATUS` 是 `0x30008`；
- `SR_INTERRUPT_STATUS` 是 `0x3000c`；
- `SR_INTERRUPT_CLEAR` 是 `0x30014`；
- `DIRTY` 是 `0x30080`；
- `ADDRESS_VALID` 是 `0x300e0`；
- `INVALID_ADDR_ACCESS` 是 `0x32078`；
- `SHADOW_VALUE0` 是 `0x32000`；
- `0x300e4` 不应访问；
- 目前设备保持 `snoc_reg_mode=4`，SSH 可用；
- 下次刷写前必须先完成静态代码验证和编译；任何新镜像都要先通知用户进入 fastboot。
