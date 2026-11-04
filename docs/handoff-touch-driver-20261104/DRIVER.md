# NT36XXX 触屏驱动技术文档（sky / SM4450 / NT36672C）

本文档面向主仓库接手的维护者：结构、数据通路、初始化/电源流程、**全部修复项及其依据**、
与原厂驱动的差异、已知限制。

依据来源：`docs/touch-investigation/`（原厂 HyperOS 真机抓取 + 与设备 `.ko` 同源的原厂源码
`vendor-src/nt36xxx*.c`，`MiCode/Xiaomi_Kernel_OpenSource` 分支 `sky-t-oss`）。

---

## 1. 器件与总线

| 项 | 值 |
| --- | --- |
| IC | Novatek NT36672C（模组 Tianma M19），trim `0C 00 00 72 66 03` → **NT36675 map**、`hw_crc=2` |
| 总线 | `spi@a84000`（QUP SE1，`qcom,geni-spi`），CS0，mode 0，**4 MHz**（原厂 9.6 MHz，见 §7） |
| DMA | GPI-DMA（`qcom,enable-gsi-dma` + `dmas = <&gpi_dma1 0 1 QCOM_GPI_SPI>, <&gpi_dma1 1 1 …>`） |
| ICC | 必须保留 `interconnects` 三元组（qup-core / qup-config / **qup-memory**），否则 GENI→DDR 无带宽、SE 会卡在 `M_GENI_CMD_ACTIVE` |
| 中断 | TLMM **GPIO13**，`IRQ_TYPE_EDGE_RISING \| IRQF_ONESHOT`，线程化处理 |
| 复位 | **SPI 软复位**（无硬件复位脚参与；GPIO12 只由 pinctrl 保持上拉） |
| 供电 | 与面板共用轨，驱动不取 regulator（原厂同样没有 supply） |

### SPI 帧格式

```
页选：MOSI = [0xFF, (addr>>15)&0xFF, (addr>>7)&0xFF]
写  ：MOSI = [addr|0x80, data...]
读  ：MOSI = [addr&0x7F, 0x00 ...]（共 len+2 拍），有效数据 = MISO[2 .. ]
```

我们的后端（`nt36xxx_spi.c`）用 regmap 自定义 `read`/`write` 实现，每次访问自动页选；
`NT36XXX_READ_DUMMY = 2` 对应原厂 `memcpy(buf+1, rbuf+2, len-1)`。

关键地址（NT36675 map）：`EVENT_BUF_ADDR = 0x22D00`，
`|0x50` = HOST_CMD，`|0x51` = handshake/sub-cmd，`|0x60` = RESET_COMPLETE，
`|0x78` = FWINFO；`SWRST_N8 = 0x3F0FE`，`SPI_RD_FAST = 0x3F310`，
`CHIP_VER_TRIM = 0x3F004`，`ENG_RST = 0x7FFF80`，
FW 历史 `0x23D10 / 0x23D50`。

---

## 2. 报文格式（本固件实测）

每次中断读 **66 字节**（10 个 6 字节点；原厂超级分辨率版本读 109 字节，见 §7）：

```
byte 0        : id = b>>3 (1..10)，state = b & 7（1=按下 2=移动）
byte 1..2     : X，16 位大端，单位 1/10 像素（0..10799）
byte 3..4     : Y，16 位大端，单位 1/10 像素（0..24599）
byte 5        : 触摸强度（touch major / weight，0..255）
第 i 个点偏移 : ppos = 6 * i
```

* 上报：MT protocol B（10 slot），`ABS_MT_POSITION_X/Y` 缩放到面板像素
  （`nd36xxx_scale_coord()`，raw_max = 面板像素 ×10），
  `ABS_MT_TOUCH_MAJOR` 与 `ABS_MT_PRESSURE` 都取第 5 字节；
* `INPUT_MT_DIRECT` 因而还会输出 `ABS_X/ABS_Y/ABS_PRESSURE/BTN_TOUCH`，属正常；
* 空闲图样 **`0x77`**（`state=a3`，正常运行）；固件复位图样是全 `0xFD`/`0xFE`。

---

## 3. 初始化流程（每次 init 都走）

```
nt36xxx_sky_recover_reset()  ← 仅"重试"时：0xAA(sw reset idle,15ms) → 0x5A(eng reset) → 0x55(sw reset,10ms)
nt36xxx_chip_version_init()  ← 0x69(bootloader reset, 5/35ms) + 读 0x3F004 识别 map
nt36xxx_download_firmware_hw_crc()
   0x69 bootloader reset
   nt36xxx_set_bld_hw_crc()          ← ILM/DLM 的 DES+LENGTH+golden CRC，**在写固件之前**（原厂顺序）
   逐分区写 SRAM（16 KiB 分块，每分区 size+1 字节）
   逐分区读回校验 + 坏块重写（4 轮，最多 32 块/轮）   ← 原厂没有，见 §7
   BLD_CRC_EN |= bit7
   EVENT_BUF|0x60 = 0（6 字节）；|0x50 = 0xAE
   BOOT_RDY = 1，等待 10 ms
   check_fw_reset_state(INIT)：轮询 EVENT_BUF|0x60，50×10 ms
   读 FWINFO（16 B，校验 fw_ver + ~fw_ver == 0xFF；0x18 + 0xE7）
   check_fw_reset_state(REK)：等到 ≥0xA1（50×10 ms）
   wait_first_report()：轮询报文区直到不再是 0xFD/0xFE（20×5 ms）
```

之后 worker 打开 IRQ。**首次初始化在 probe 后 14 s**（原厂取值，见 §5）。

---

## 4. 中断、故障检测与恢复

```
IRQ(线程化) → nt36xxx_report()
    mutex_lock(&ts->lock)              ← 所有 SPI 使用者共用一把锁（原厂同样全程持锁）
    读 66 B 报文
    若前 6 字节全 0xFD/0xFE → fw_reset_cnt++，连续 10 次 → 请求恢复
    解析并上报 MT-B
    mutex_unlock
```

恢复策略（`nt36xxx_sky_schedule_recover()`）：

| 触发源 | 条件 |
| --- | --- |
| 报文路径 | 连续 10 次全 `0xFD`/`0xFE`（原厂 `nvt_wdt_fw_recovery` 阈值） |
| 健康轮询 | 每 2 s 读 `EVENT_BUF|0x60`；读到 `0xFD/0xFE/0xFF` **或 < 0xA1(REK)** 连续 2 次 |

* 节奏：前 5 次每 2 s，之后每 60 s，**永不永久放弃**；
* 前后 3 次仍失败 → 进入 park：只打一行 `sky: IC will not recover (N attempts), parking until panel cycle/reboot`，
  报文路径直接返回（避免 200 IRQ/s 风暴每次都发 SPI）；面板亮屏或重启给新预算；
* 每次恢复前打印 `sky-fwhist[0/1]`（IC 自己的崩溃记录区）；**只在 IC 活着时有意义**，复位态读出全是 0xFE；
* `0x77` 永远不触发恢复（它是空闲图样；照抄原厂"0x77=复位"会导致每个空闲帧都重刷）。

---

## 5. 电源状态机（DRM panel follower）

板级 DTS 必须有 `panel = <&panel>;`，否则 `drm_is_panel_follower()` 返回 false，
**四个回调全部不会发生**（本项目曾因此让亮屏永远不恢复触摸）。

| 回调 | 时机（m19 驱动） | 我们的行为 |
| --- | --- | --- |
| `panel_prepared` | `m19_prepare()`：电轨使能后 | 若处于 SUSPEND 则 `enable_irq()`；仅状态簿记，不排程 |
| `panel_enabled` | `m19_enable()`：DSI 初始化/PPS 之后 | 清重试预算 → `mod_delayed_work(40 ms)` 重刷固件（原厂"每次 resume 重刷"） |
| `panel_disabling` | `m19_disable()` 之前 | 置 SUSPEND + 下发 `0x11` 深睡 + 取消 work |
| `panel_unpreparing` | `m19_unprepare()`：断电轨前 | 置 SUSPEND + `disable_irq()` + 深睡 |

首次初始化**不受面板回调提前拉入**，保持 probe 后 14 s
（原厂注释 "after display reset (RESX) sequence"；实测在 ~2.6 s 的 DSI 初始化窗口里初始化必失败）。

---

## 6. 全部修复项（按重要性）

| # | 现象 | 根因 | 修复 | 证据 |
| --- | --- | --- | --- | --- |
| 1 | 触摸全无、GPIO13 200–250 IRQ/s、报文恒 `0xFE` | IRQ 在 IC 尚处于 ReK 前就被使能；且没有"固件复位"恢复 | INIT 后等 ReK + 等首帧再开 IRQ；新增 `0xFD/0xFE` 连续 10 次的恢复路径 | handoff11/12 |
| 2 | 运行期掉固件（9.5 分钟后） | **刷的是错固件**（FW 0x12，机器原厂是 **0x18**） | 固件换成原厂 `novatek_ts_fw.bin`(0x18) | handoff14/15，45 分钟无复位 |
| 3 | 掉固件后只能重启恢复 | 恢复只发 `0x5A`；原厂 WDT 路径是 `0xAA` 优先 | 重试路径用原厂阶梯 `0xAA/0x5A/0x55` | handoff15 |
| 4 | —— | 全驱动没有互斥，健康轮询与报文读取会在 SPI 上交错 | `ts->lock` 覆盖所有 SPI 使用者 | handoff13 |
| 5 | 息屏/亮屏后触摸永久失效 | 驱动只实现 `panel_prepared/unpreparing`；m19 的 DSI 初始化在 `enable`；且 DTS 缺 `panel` 属性导致 follower 从未注册 | 新增 `panel_enabled/panel_disabling`；DTS 加 `panel = <&panel>` | handoff19/21/22 |
| 6 | 卡在 INIT 无人发现 | 健康轮询只认 `0xFD/0xFE/0xFF` | 增加 `< 0xA1(REK)` 判据 | handoff19 |
| 7 | 失败后 200 IRQ/s 风暴持续折腾 SPI | 恢复无限重试 | park 策略（见 §4） | handoff20 |
| 8 | 开机首次初始化必失败 | 首次初始化落在 DSI 初始化窗口（~2.6 s） | 改为 probe 后 14 s（原厂值） | handoff20，三次重启各 1 次恢复 |
| 9 | 下载顺序与原厂不同 | BLD CRC 描述符组写在固件之后 | 移到写固件之前（原厂顺序） | handoff20 |
| 10 | 输入设备缺 `ABS_MT_TOUCH_MAJOR` | 未上报 | 新增，与 strength 字节一致 | handoff17 |
| 11 | FWINFO 坏掉时仍报 "fw loaded ok" | 5 次失败后落穿 | 失败即 `release_fw_buf`，由外层重试 | handoff11 |
| 12 | 挂起时把 `0x11` 写进报文区 | 地址少了 `\|0x50` | 写到 `EVENT_BUF_ADDR \| EVENT_MAP_HOST_CMD` | handoff11 |

---

## 7. 与原厂驱动的差异（均为刻意，附理由）

| 差异 | 原厂 | 我们 | 理由 |
| --- | --- | --- | --- |
| SPI 时钟 | 9.6 MHz | **4 MHz** | 1 MHz 固件下载会卡死；4 MHz 下写入+回读校验稳定（9.6 MHz 未在大传输上验证） |
| 固件分块 | 63 KiB | 16 KiB | 主线的 GPI/SPI 路径在 64 KiB 级传输上会挂；16 KiB 已验证 |
| 写后校验 | 无（依赖 IC 侧 BLD CRC） | **读回比对 + 坏块重写** | 曾实测到 SRAM 写入损坏；重写后 `bad=0` 收敛 |
| resume 重刷 | 每次 | 每次（`panel_enabled` 后 40 ms） | 与之一致 |
| 报文长度 | 109 B（超级分辨率） | **66 B** | 本固件第 98 字节起是 `0x22D60` RESET_COMPLETE 寄存器（地址算术：0x22D00+96），其"压力区"其实是寄存器内容；实测该区内容 = 状态寄存器 |
| 压力来源 | `point_data[99+i]` | 点的第 5 字节 | 同上，原厂那片区不是压力 |
| 0x77 图样 | 当固件复位→重刷 | **忽略** | 本 IC 空闲就是 0x77；照抄会每个空闲帧重刷 |
| 硬件复位 | 无（`NVT_TOUCH_SUPPORT_HW_RST=0`） | 无 | 本板 GPIO12 在零售态 unclaimed |
| 手势/双击唤醒 | 有（host cmd 0x13 + `enable_irq_wake`） | **未实现** | 需要 `wakeup-source` + input event 钩子，见 §8 |
| 手写笔 / 产测(mp) | 有 | 未实现 | 与本机基础触控无关 |
| Xiaomi HAL（`/dev/xiaomi-touch`、touch mode） | 有 | 未实现 | 主线用 evdev/libinput 即可 |

---

## 8. 已知限制 / 后续可做

1. **pinctrl sleep 态**：DTS 只用了 `default`（`ts_active`）；驱动没有
   `pinctrl_select_state()`，如需 `ts_int_suspend`/`ts_reset_suspend` 要补驱动调用。
2. **无手势唤醒**：要支持双击亮屏需 `wakeup-source;` + `input_dev->event` 钩子 +
   suspend 时 host cmd `0x13` 与 `enable_irq_wake()`（TLMM13 需可唤醒）。
3. **开机首次初始化仍可能失败 1 次**：14 s 延迟后仍有概率落到复位态，
   由恢复路径接住（三次重启实测各 1 次 `sky-recover[1]`，随后稳定）。若要在驱动层降低概率，
   可试"14 s 后再等首帧就绪 + 失败立刻用阶梯重试"，但别把阶梯放到第一次初始化上
   （实测会变成每次初始化都失败的死循环）。
4. **四角坐标校准**未在最终镜像上复测（报文范围已验证 0..1079 / 0..2459）。
5. **固件分发**：`novatek/nt36672c.bin`（FW 0x18）来自原厂
   `/vendor/firmware/novatek_nt_ts_fw.bin`，版权归 Novatek/小米，是否入库需自行决定；
   驱动只用 `request_firmware()`，路径由 DTS `firmware-name` 指定。
6. **Kconfig 重复符号**（见 README §3）。

---

## 9. 排障速查

| 症状 | 先看 | 结论 |
| --- | --- | --- |
| 完全无触摸，IRQ 200+/s | `sky-report` 前 6 字节是否全 `fe` | 固件未运行；看 `sky-recover` 与 `sky-fwhist` |
| 空闲 IRQ 远高于 ~12/s | 同上 | 同上；正常空闲约 12 IRQ/s |
| 亮屏后不恢复 | 有没有 `sky-panel: enabled` | 没有 → DTS 缺 `panel` 属性 / follower 未注册 |
| 有 `sky-panel: enabled` 但无新 `loaded ok` | `sky: IC will not recover` / `give_up` | 已 park；亮屏或重启给新预算 |
| `sky-fwinfo` 不是 `0x18` | 固件文件 | 刷错了固件（0x12 是别的树带来的） |
| `sky-crc` golden/result 不一致 | 供电/面板状态 | IC 半供电（例如面板刚关）；不是 SPI 问题 |
| 坐标偏小 10 倍 / 16 步跳变 | 报文解析 | 有人退回 12 位 nibble 解析或改了 `NT36XXX_RAW_UNITS_PER_PX` |

调试口：`/sys/bus/spi/devices/spi0.0/sky_reinit` 写 1 = 重放一次"resume 重刷"，
用于在没有真实息屏/亮屏条件时验证恢复路径。
