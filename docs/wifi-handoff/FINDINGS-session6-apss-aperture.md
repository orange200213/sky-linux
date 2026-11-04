# Session 6: 谁在给 WCSS 寄存器解码（APSS 侧的实测结论）

设备：Xiaomi Redmi 12 5G（sky），SM4450 ravelin，WCN6750 adrastea（SNOC）
内核：#90 `7.3.0-g78458d4eea4e-dirty`（k91 镜像）
结论均为硬件实测 + 原厂源码对照，标注了来源。

## 1. 原厂代码路径（决定性，来自原厂源码树）

源码：`sky-lineage/android_kernel_xiaomi_sm8450-modules@lineage-23.2`
（`qcom/opensource/wlan/{qcacld-3.0,qca-wifi-host-cmn,fw-api}` 三个目录，非 submodule）

| 事实 | 出处 |
|---|---|
| SNOC 的 target type 是 `TARGET_TYPE_ADRASTEA`（19），不是 `TARGET_TYPE_QCA6750` | `hif/src/snoc/if_snoc.c:219-222` |
| `hal_target_based_configure()` 的 switch **没有** `TARGET_TYPE_ADRASTEA` 分支，只有 QCA6750 会置 `use_register_windowing/static_window_map` | `hal/wifi3.0/hal_srng.c:401-491` |
| `hal->dev_base_addr = hif_get_dev_ba()`，即 `scn->mem` = icnss ioremap 的 membase；`hal->dev_base_addr_ce = hif_get_dev_ba_ce()` | `hal_srng.c:1074-1075` |
| SNOC 的 `hif_write32_mb()` 在 `HIF_REG_WINDOW_SUPPORT && HIF_PCI` 之外时是**裸 `iowrite32`**，无窗口 | `hif/src/hif_io32.h:47-64` |
| WCN6750 的 UMAC/WBM/REO 是芯片偏移 0xa00000/0xa34000/0xa38000 | `fw-api/hw/qca6750/v1/wcss_seq_hwiobase.h:191,200,201` |
| 原厂 CE 基址（adrastea）就在 `A_WCSS_HM...`: CE0 `0x240000`…CE11、CE wrapper `0x24C000` | `hif/src/adrastea_reg_def.h`（0x240000 起的 19 条） |
| 原厂 adrastea 主机寄存器表最大只到 `0x2F1000`，**没有** UMAC 0xa00000 区 | 同上（块直方图） |

⇒ 原厂 SNOC 路径若真的跑 DP ring setup，写的就是 `membase + 0xa34874`
（= 0x23234874），这在本机上是**未解码**地址（见 §2）。要么原厂 ROM 用的不是
adrastea profile，要么原厂还缺一步我们没复现的使能。**这是当前最大未知。**

## 2. APSS 侧寄存器空间实测（本轮新增工具：无需刷机的 mmap 探针）

工具：`wifi-backup/regtool/`（`skyreg.ko` + `skyprobe`，见 §5）。

### 2.1 membase(0x22800000, DT 8 MiB) 内，4 KiB 页扫描结果

只有 4 个页有解码：

```
0x22820000  (SR 类块，0x00-0x7C 有寄存器，0x84+ DEAD)
0x22822000  (SR 类块第二例，0x60=0x1, 0x68=PM-类值, 0x74=0x82ac1008)
0x22830000  SR_APSS 控制组
0x22832000  SR_APSS 值组
```

`0x22824000`（CE0 SR_BA_LOW）**DEAD**，`0x228B4874`（窗口 1）**DEAD**，
`0x22880000/0x228100000/0x228180000`（窗口基址）**DEAD**。

### 2.2 时间维度：bring-up 全程都没有出现

把探针挂在 `sky-wifi-up.sh`（bind 之前）里，以 100 ms 采样 24 s，覆盖
WPSS start → QMI → MSA → BDF → cal report → DP：

```
   0 ms 0x22830008 = 0            (WPSS 还没起)
 256 ms 0x22830008 = 0x8          (WCSS core awake；之后无变化)
   全程 0x22824000 / 0x228B4874 / 0x23234000 / 0x22880000 都是 DEAD
```

⇒ CE/UMAC 不是"某时刻才可见"，而是始终没有被解码。

### 2.3 0x20000000-0x30000000 全页扫描（65536 页）

20 个 live 页：`0x221c8000`(qfprom)、`0x22820000`、`0x22822000`、`0x22830000`、
`0x22832000`、`0x22a40000…0x22a4e000`。`0x22a40000` 那片是**RAM**（填充
`0xdeadc0de`、含 0x82a3xxxx 指针，15360/16384 word 可读），不是寄存器。

`0x0-0x80000000` 的全空间扫描把机器**挂死**了（无 panic 记录，事后落到 fastboot），
所以盲扫要分块、且避开危险区域。

## 3. SR_APSS 权限实测

| 寄存器 | 写是否生效 | 说明 |
|---|---|---|
| `0x32064 SW_SCRATCH` | ✅ 生效（0xcafe0001 回读一致） | 探针写通路的验证点 |
| `0x32000 SHADOW_VALUE0` | ✅ 生效，并置 `DIRTY` bit0、`SR_INTERRUPT_STATUS` bit0 | |
| `0x30020 SHADOW_ADDRESS0` | ❌ 回读仍 0 | 与原厂头文件 `___RWC QCSR_REG_RO` 一致 |
| `0x300e0 ADDRESS_VALID` | ❌ 回读仍 0 | 同上；开机即 0，24 slot 全未配置 |
| `0x30000 SR_CONTROL` | ❌ 回读仍 0 | **`ENABLE_APSS_FULL_ACCESS`=bit2、POR=0，APSS 写不进** |
| `0x32060 FORCE_WAKE` | ✅ 写入 1，并引发 `SR_INTERRUPT_STATUS=0x10a`、`SR_INTERRUPT_ENABLE` 由 0x7ff 变 0x6fc | FORCE_WAKE 不能把 UMAC 变可见 |

`SR_CONTROL` 位定义（`adrastea_reg_def.h`）：bit0 SOFT_RESET、bit1
CLOCK_GATE_DISABLE、**bit2 ENABLE_APSS_FULL_ACCESS**、bit3 SR_PLL_REF_MUX_SEL、
bit4 SR_RF_XO_MUX_SEL，mask 0x1F。

⇒ 影子端口是"主机只能写 SHADOW_VALUE、地址/有效位只读"的设计；而
`ENABLE_APSS_FULL_ACCESS` 只能由 WCSS/安全侧置位，本机实测为 0。

## 4. 重要纠错：之前"CE0..CE11 ring init 成功"的说法不成立

`ath11k_core_qmi_firmware_ready()` 里 `ath11k_ce_init_pipes()` 先于
`ath11k_dp_alloc()`；本机日志显示第一个 abort 就发生在 `ath11k_dp_srng_setup()`
（`ath11k_ahb_window_write32_snoc`），CE 阶段没有任何 abort。而 0x22824000 实测
DEAD，所以 CE 阶段其实**没有发生任何寄存器访问**（CE pipe 未建立/ring 为空）。
结论：截至目前，mainline ath11k 在本机除 SR_APSS 外**没有成功访问过任何 WCSS 寄存器**，
所有"能跑通"的里程碑（QMI/MSA/BDF/cal report）都是 SMEM/QMI 路径，不经寄存器。

## 5. 新增工具（保留在仓库，无需刷机即可迭代）

```
wifi-backup/regtool/skyreg.ko    misc 设备 /dev/skyreg，mmap 任意物理区间
                                 (pgprot_device, VM_PFNMAP)，insmod 参数
                                 phys_size = 单次映射上限（默认 64 MiB）
wifi-backup/regtool/skyprobe     静态 aarch64 工具：
                                 rd / wr / rmw / scan / watch
                                 总线 abort 在 EL0 变成 SIGBUS 并被捕获，
                                 所以读未解码地址不会拖死内核
```
构建：`make -C <kernel> M=<regtool> modules` + `aarch64-linux-gnu-gcc -static`。
注意 `CONFIG_MODULE_UNLOAD` 未开：**一次开机只能 insmod 一次**（不能 rmmod）。

⚠️ `copy_from_kernel_nofault()` 在 arm64 上**不能**兜住 external abort：内核里
读未解码地址会 oops 掉调用它的任务（v1 探针就是这样把 shell 打死并留死锁 mutex）。
探针务必走 EL0（用户态）映射。

## 6. 本轮踩到的坑（已恢复）

`snoc_pre_bdf=5`（WLAN_INI + WLAN_CFG + WLAN_MODE(NORMAL) 在 BDF 之前发送）会让
内核**整机挂死**（无 console 输出），随后引导器进入 fastboot。恢复方法：
用 DT 里把 wifi 节点 `status="disabled"` 的镜像启动（内核不变，仅 DTB 改一行），
进 Linux 改回 `/etc/sky-wifi-params`，再刷回 k91。
镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`。

安全参数（请勿删除）：
```
bdf_total_full=y
snoc_reg_mode=4
snoc_membase_mib=0
snoc_probe=n
snoc_shadow_probe=y
```

## 7. 下一步（按价值排序）

1. **拿到原厂 `qca_cld3_*.ko`**（HyperOS 的 `vendor_dlkm.img` / `super.img`）并反汇编：
   确认编的是 adrastea 还是 qca6750 profile、`hal_srng_setup` 实际写哪个地址、
   有没有 SR_APSS/window 初始化代码。这能直接终结"UMAC 基址未知"。
2. 若 (1) 证明存在一个使能步骤：查原厂 `icnss2`（`power.c` 管 `cxo_ref_clk_pin` 等 clock）
   与 WCSS AHB 时钟/电源域；用 mainline 的 clk/debugfs 检查这些 clock 是否 on。
3. 若能保持固件存活（例如给 ath11k 加"跳过 DP"参数），再在用户态反复试
   `SR_CONTROL`/`FORCE_WAKE`/窗口组合，观察 UMAC 是否变可见。
4. 只有在 (1)/(2) 都排除后，才考虑"通过影子端口做 UMAC 访问"的方案——但实测
   `SHADOW_ADDRESS`/`ADDRESS_VALID` 对 APSS 只读，需要 WCSS 侧配合。

---

## 8. 突破：寄存器基址不是 DT 的 membase，而是固件给的 2 MiB BAR

### 8.1 原厂二进制证据（从 `super.img` 的 vendor_dlkm 取出）

`super.img` 是 Android **sparse** 镜像；LP metadata 里 `vendor_dlkm_a` = extent 5
（32.1 MiB @ raw 0x146500000），文件系统是 **EROFS**（用 `fsck.erofs --extract=` 解出）。
里面有：

```
icnss2.ko             429 KB
qca_cld3_adrastea.ko  13.9 MB
qca_cld3_qca6750.ko   15.8 MB
```

符号对比（决定性）：

| 符号/字符串 | adrastea.ko | qca6750.ko |
|---|---|---|
| `hal_attach` | ✗ | ✓ |
| `hal_srng_setup` | ✗ | ✓ |
| `hw_srng_table_6750` | ✗ | ✓ |
| `hal_qca6750_attach` | ✗ | ✓ |
| `"Accessing Wrong register"`（hal_get_window_address_6750 的报错） | ✗ | ✓ |
| `0x310c`（WINDOW_REG_ADDRESS）立即数 | – | 28 处 |

⇒ 本机真正跑的是 **`qca_cld3_qca6750.ko`（QCA6750 profile，带寄存器窗口）**，
`adrastea.ko` 里连 HAL 都没有。（`modules.load` 同时列了两个，但只有 6750 能工作。）

### 8.2 原厂基础 DTB 里是两个不同节点

`vendor_boot.img` 的**基础** DTB 里有：

```
qcom,wcn6750 {
    compatible = "qcom,wcn6750";            /* → WCN6750_DEVICE_ID */
    reg = <... 0xb0000000 0x10000>;
    qcom,smem-state-names = "wlan-smp2p-out\0wlan-soc-wake-smp2p-out\0wlan-ep-powersave-smp2p-out";
    ...
};
```
而 `dtbo.img` 的 overlay 里才是 `qcom,icnss@22800000`（compatible `qcom,icnss`
→ ADRASTEA_DEVICE_ID）。

`icnss2/main.c` 的分支区别（`icnss_dt_match`）：

* `compatible = "qcom,wcn6750"` → `WCN6750_DEVICE_ID`：
  先发 **WLFW device_info**，然后用固件返回的 **BAR 地址/大小** 做 ioremap
  （`priv->mem_base_pa/size`，日志 "Non-Secured Bar Address"）——**DT 的 membase 不参与**。
* `compatible = "qcom,icnss"` → `ADRASTEA_DEVICE_ID`：只用 DT `membase`（8 MiB）。

### 8.3 mainline ath11k 本来就是这么设计的

`ath11k/qmi.c:2582 ath11k_qmi_request_device_info()`：

```c
if (!ab->hw_params.hybrid_bus_type)  return 0;      /* ← 我们这里被关掉 */
send QMI_WLANFW_DEVICE_INFO_REQ …
必须 resp.bar_addr && resp.bar_size == ATH11K_QMI_DEVICE_BAR_SIZE (0x200000)
bar_addr_va = devm_ioremap(ab->dev, resp.bar_addr, resp.bar_size);
ab->mem = bar_addr_va;  ab->mem_len = resp.bar_size;
```

调用点在 `ath11k_qmi_event_load_bdf()`，紧跟 `request_target_cap()` 之后；
WCN6750 的 `hw_params.hybrid_bus_type = true`（`core.c:652`），但
**patch 0003 在 SNOC 分支把它改成了 false（`ahb.c:1526`）**，所以 device_info 从未发过。

### 8.4 于是整个地址模型就通了

BAR 是 **2 MiB**，正好放三个 512 KiB 静态窗口
（这也正是原厂注释 "Static window map is enabled for qcn9000 to use 2mb bar size
and use multiple windows to write into registers" 的含义）：

| 窗口 | BAR 内偏移 | 对应芯片偏移 | 用途 |
|---|---|---|---|
| 0 | 0x000000-0x07FFFF | 直接（0x0-0x7FFFF） | SR_APSS / CE wrapper 等 |
| 1 | 0x080000-0x0FFFFF | 0x0A0000-0xA7FFFF | **UMAC**（WBM 0xA34000 / REO 0xA38000 / TCL 0xA44000） |
| 2 | 0x100000-0x17FFFF | 0x1B80000+ | **CE0..CE11** |
| 3 | 0x180000-0x1FFFFF | CMEM | CMEM |

也就是说：`0xa34874` → BAR+`0x80000+0x34874` = **BAR+0xB4874**；
CE0 → BAR+`0x100000+0x18000` = **BAR+0x118000**。
这正是 `snoc_reg_mode=3/4` 的算法（地址公式我们是对的），
**唯一错的是基址：用 DT membase 而不是固件给的 BAR**。

### 8.5 阶段结论与下一次实验

* `dtb` 里的 `membase`（0x22800000, 8 MiB）是 MHI/MSI/侧带区，不是寄存器区；
  实测只有 SR_APSS 类页有解码，与"寄存器在别处"一致。
* 下一步（一次 flash）：在 SNOC 分支保留 `hybrid_bus_type` 的 device_info 路径，
  即加一个 `snoc_device_info=Y` 开关，让它：
  1. 照常发 `QMI_WLANFW_DEVICE_INFO_REQ`（0x0021/0x0022 见 qmi.h），
  2. 打印 `bar_addr` / `bar_size`（先不要硬性要求 0x200000，超范围也要打出来），
  3. 拿到地址后 `devm_ioremap` 并作为 `ab->mem`，配合 `snoc_reg_mode=3`
     （纯静态窗口映射）访问 UMAC/CE。
* 如果固件回 `device_info` 失败（旧记录里出现过 94 NOT_SUPPORTED），
  那就要按 ADRASTEA(icnss overlay) 那条路走，用 DT membase 并另找使能；
  但本机 `modules.load` 里 `qca_cld3_qca6750.ko` + `qcom,wcn6750` 基础节点
  的存在，强烈指向 BAR 路径。

---

## 9. k92 / k93 实测：device_info 路线被固件否决

为验证 §8 的 BAR 假设做了两个镜像（DTB 与 k91 逐字节相同，只改内核）：

| 镜像 | 内核 | 改动 | 结果 |
|---|---|---|---|
| `sky-shim-boot-v4-wifi-k92-snoc-device-info.img` | #91 | 新增 `snoc_device_info` 开关（`ath11k` 模块参数，`EXPORT_SYMBOL_GPL` 给 ahb 用）；`ath11k_qmi_request_device_info()` 在该开关下绕过 hybrid 检查；关闭时保留原厂 `0x1b80000` CE 偏移 | 开关生效（`snoc mode: device_info BAR + static window map`），但**一条 QMI 都没发出**——发现 SNOC 启动分支在 `request_target_cap()` 之后直接 `return`，从不进入 `ath11k_qmi_event_load_bdf()` |
| `sky-shim-boot-v4-wifi-k93-snoc-device-info2.img` | #92 | 在 SNOC 分支 `request_target_cap()` 之后补发 device_info | **固件拒绝**：`qmi device info request failed: 1 94`（94 = QMI_ERR_NOT_SUPPORTED）→ `snoc: device info request failed: -22` |

⇒ 该固件（CI_WLAN.HL.3.4.3-00346 … SNOC 变体）**不支持 WLFW DEVICE_INFO**，
所以 mainline 的 "hybrid BAR" 路线在本机不可用。§8 的 BAR 假设就此关闭。

同时补测了窗口选择寄存器：

```text
0x2280310C  ← 0x40000014   REFUSED (dead)      ; WINDOW_REG / PCIE_REMAP_1M_BAR_CTRL 不存在
0x228B4874                  DEAD                ; 写入后仍是 DEAD
```

⇒ 连 `static_window_map` 依赖的窗口选择寄存器都没有解码。

### 9.1 新发现：0x22820000/0x22822000 是 SR_APSS 的别名

把这两个"活页"完整 dump 后与 `0x22830000/0x22832000` 对比：

```text
0x22822060..7c : 0x0  0x11530000  0x33002002  0x50c1f000  0x10000c  0x94130008  0x18031  0x0
0x22832060..7c : 0x0  0x11530000  0x3e01f000  ...         0x10200c  0x1b8f808  0x0      0x0
```

`SW_SCRATCH` 完全一致（`0x11530000`），`PMM_SR_LSB` 前缀相同，`WCSSAON_SR_MSB` 是活动计数器所以两次不同。
⇒ 两个地址是**同一组 SR_APSS 寄存器的别名**（控制窗口 0x200000↔0x30000，值窗口 0x220000↔0x32000，
不是常量偏移，说明地址译码有折叠）。这一点下一轮用作"如何在不破坏其它块的前提下探测别名"的参考。

### 9.2 现在的状态

```text
镜像 k93（内核 #92）  bdf_total_full=y  snoc_reg_mode=4  snoc_membase_mib=0
snoc_probe=n  snoc_shadow_probe=y  snoc_device_info=n
```
设备可 SSH，WiFi 仍在 `ath11k_ce_init_ring() -> ath11k_hal_srng_setup()` 处 abort。

### 9.3 结论：三条路线已排除，剩下的方向

已排除（都是硬件实测）：
1. 用 DT membase 直接访问 UMAC/CE（0xa34874 / 0x240000）—— DEAD；
2. 用静态窗口映射（0x80000+ / 0x100000+ / 0x180000+）—— DEAD，且 0x310C 选择器也不存在；
3. 用固件 device_info 拿 2 MiB BAR —— 固件返回 94 NOT_SUPPORTED。

剩下最值得做的：
1. **反汇编原厂 `qca_cld3_qca6750.ko` 的 `hal_target_based_configure` / `hal_attach` / DP 路径**，
   确认它在本平台真的写 `0x80000+` 还是另有一套 IPCI-PLD 提供的基址
   （icnss2 的 IPCI PLD flag `PLD_IPCI_ICNSS_FLAG` 可能给出与 DT membase 不同的 `soc_info.v_addr`；
   见 `drivers/soc/qcom/icnss2/power.c`、`main.c` 的 `icnss_get_soc_info`/PLD ops 表）。
2. 查 `icnss2` 是否在 `wlan_enable` 前后对 WCSS 做**SoC 级 remap/ATC/AON** 操作
   （反汇编 `icnss2.ko` 找除 0x80000 之外的写寄存器常量）。
3. 若能拿到原厂串口日志（stock ROM 启动时的 dmesg），可以直接看到 icnss2 打印的
   `MEM_BASE pa/va`、`Non-Secured Bar Address pa/va`，一眼定位基址。

### 9.4 k92/k93 的代码改动清单（内核树 `drivers/net/wireless/ath/ath11k/`）

* `core.c`：新增全局 `bool ath11k_snoc_device_info` + `EXPORT_SYMBOL_GPL`
  + `module_param_named(snoc_device_info, …, bool, 0644)`。
* `core.h`：`extern bool ath11k_snoc_device_info;`
* `qmi.c`：
  - `ath11k_qmi_request_device_info()`：`if (!hybrid && !snoc_device_info) return 0;`；
    打印 `device info: bar %#llx size %#x (valid %d/%d)`；仅在非 SNOC 时才要求
    `bar_size == 0x200000`；成功后打印 `using device_info BAR …`；
    `ab->mem_ce = ab->mem`（SNOC 强制）。
  - SNOC 启动分支（`ath11k_qmi_request_target_cap()` 之后）补发
    `ath11k_qmi_request_device_info()` 并打印结果（k93 的关键）。
* `ahb.c`：
  - `snoc_device_info` 打开时**不再**覆盖 CE 寄存器基址（保留原厂 `0x1b80000`，
    静态窗口映射后为 BAR+0x118000）；关闭时保持 0x240000 布局。
  - `snoc_reg_mode >= 3` 的窗口路径：offset < 0x80000 直接访问（窗口 0）且不再告警；
    超出 `ab->mem_len` 的访问改成"跳过并告警"，避免实验期整机 abort。

镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k92-…img`、`…-k93-…img`（均未改 DTB）。
