# Wi-Fi 移植记录：session 4（stock Adrastea 源码对照）

## 新证据

从用户提供的 Starlink/QCA 源码树中下载了 stock HIF/CE 文件：

- `qca/src/qca-wifi/cmn_dev/hif/src/adrastea_reg_def.h`
- `qca/src/qca-wifi/cmn_dev/hif/src/ce/ce_reg.h`
- `qca/src/qca-wifi/cmn_dev/hif/src/ce/ce_main.c`
- `qca/src/qca-wifi/cmn_dev/hif/src/regtable.c`
- `qca/src/qca-wifi/cmn_dev/hif/src/dispatcher/multibus_snoc.c`

参考提交：

<https://github.com/SpaceExplorationTechnologies/starlink-wifi/tree/1d585ee8909bf7bab4391c0c52d8a9d44137b3bb/qca/src/qca-wifi/cmn_dev/hif/src>

## 关键发现

stock Adrastea 代码没有把所有 CE 控制寄存器改为 SR_APSS shadow transport。它只在编译宏 `ADRASTEA_SHADOW_REGISTERS` 下，将 CE 的 write-index 写入改为预先映射的 `SHADOW_VALUE` 槽：

```c
CE_SRC_RING_WRITE_IDX_SET -> A_TARGET_WRITE(shadow_sr_wr_ind_addr(...), n)
CE_DEST_RING_WRITE_IDX_SET -> A_TARGET_WRITE(shadow_dst_wr_ind_addr(...), n)
```

映射表：

```text
CE source: 0 -> slot 0
           3 -> slot 3
           4 -> slot 4
           5 -> slot 5
           7 -> slot 7

CE dest:   1 -> slot 13
           2 -> slot 14
           5 -> slot 17
           7 -> slot 19
           8 -> slot 20
           9 -> slot 21
           10 -> slot 22
           11 -> slot 23
```

stock CE 初始化只读取并打印：

```c
A_TARGET_READ(scn, SHADOW_ADDRESS(i)) << 2
```

这说明 Adrastea shadow address 寄存器保存的单位可能是 word address，主机读取后左移 2 得到 byte offset。

## 寄存器地址（确认）

```text
SR_CONTROL             0x30000
SHADOWREG_STATUS       0x30008
SR_INTERRUPT_STATUS    0x3000c
SR_INTERRUPT_ENABLE    0x30010
SR_INTERRUPT_CLEAR     0x30014
SHADOW_ADDRESS[n]      0x30020 + 4*n
DIRTY                  0x30080
ADDRESS_VALID          0x300e0
SHADOW_VALUE[n]        0x32000 + 4*n
FORCE_WAKE             0x32060
SW_SCRATCH             0x32064
PMM_SR_LSB/MSB         0x32068/0x3206c
WCSSAON_SR_LSB/MSB     0x32070/0x32074
INVALID_ADDR_ACCESS    0x32078
```

`SHADOW_ADDRESS` 的字段 mask 是 `0x003fffff`。`ADDRESS_VALID` 和 `DIRTY` 均为 24 slot bitmap。

## 重要解释

这进一步说明之前把所有 `ath11k_window_write32_snoc()` 的 UMAC writes 直接重定向到一个 guessed slot 的设计不可靠。

stock Adrastea 的 shadow 机制至少包括：

1. firmware/硬件先配置各 slot 的 `SHADOW_ADDRESS[n]`；
2. `ADDRESS_VALID` 表示哪些 slot 已经有效；
3. CE 写 index 直接写相应的 `SHADOW_VALUE[n]`；
4. shadow slot 的 address 单位需要转换（stock log 读取后 `<< 2`）；
5. 未确认 stock 如何完成 address/value/valid 的初始化，因此暂不启用实验性 mode 5。

## 与当前 ath11k 的对应关系

mainline ath11k 已通过 QMI `shadow_reg_v2[]` 发送 DP ring shadow 地址列表，但当前 SNOC 传输层仍把 DP/U MAC ring 配置写入普通 `ab->mem` 静态窗口。需要新增的不是普通 ath11k HAL shadow ring，而是：

- 将 QMI/CE shadow 配置映射到 Adrastea 的 `SHADOW_ADDRESS`/`SHADOW_VALUE` 硬件 slot；或
- 只对 Adrastea stock 支持的 CE write-index 寄存器使用 shadow value slot；
- 对 REO/TCL/WBM 的 `0xa34874` 等初始化寄存器，仍需确认是否存在另一个 SR_APSS access 方法。

当前 crash 的 `0xa34874` 是 DP/UMAC ring setup 写入，不等价于 CE write-index。不能直接套用 stock CE slot 映射。

## 当前设备状态

设备保持可 SSH，当前配置：

```text
bdf_total_full=y
snoc_reg_mode=4
snoc_probe=n
```

当前仍能完成 QMI/MSA/BDF/cal report，然后：

```text
ath11k_hal_srng_setup() -> offset 0xa34874
synchronous external abort
WPSS copy_engine.c:_CE_misc_handler:2227
```

## 下一步建议

1. 把 stock `ADRASTEA_SHADOW_REGISTERS` 的 CE slot 映射作为独立实验，不要把所有 UMAC offset 送入 slot 0。
2. 先通过 ath11k QMI shadow list 记录实际 ring register offsets，并比对 `SHADOW_ADDRESS[n]` 的 word-address 语义。
3. 需要找到 stock 对 `SHADOW_ADDRESS`、`ADDRESS_VALID` 的初始化代码或 firmware 自动配置证据；当前 Starlink 源码树只显示运行时使用 `SHADOW_VALUE`，未显示初始化实现。
4. 继续搜索 vendor `icnss`/`hif` 或固件启动日志，确认 slot 初始化是在 WLFW QMI、WPSS 固件还是 host CE init 中完成。
5. 未得到以上证据前，不再要求用户刷写新的危险 mode 5 镜像。
