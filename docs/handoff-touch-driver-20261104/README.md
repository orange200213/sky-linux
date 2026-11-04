# Novatek NT36XXX 触屏驱动 · 交接包

交接日期：2026-11-04
目标设备：Xiaomi Redmi 12 5G（`sky` / 零售名 `river`），Qualcomm SM4450（parrot）
触屏 IC：Novatek **NT36672C**，模组 Tianma M19（trim ID `0C 00 00 72 66 03`，
走 NT36675 寄存器映射，`hw_crc = 2`），挂 `spi@a84000` CS0
本包内容：**内核驱动源码 5 个文件 + DTS 触屏节点 + 固件说明 + 技术文档 + 证据日志**

> 本包只包含"触屏驱动部分"。主仓库需要自己完成提交，本包不含 .git 操作。

---

## 1. 内容清单

```
driver/                     ← 整目录拷贝到 kernel/drivers/input/touchscreen/nt36xxx-ml/
  nt36xxx_core.c            ← 主体（2000 行）
  nt36xxx.h                 ← 常量/map/结构体
  nt36xxx_spi.c             ← SPI/regmap 后端
  Kconfig                   ← 注意：见 §3 的重复符号问题
  Makefile
dts/
  touch-node.txt            ← 触屏节点成品（可直接粘贴进板级 DTS）
  sm4450-xiaomi-sky.dts     ← 该板完整 DTS（参考用）
firmware/
  README.md                 ← 固件来源/版本/校验；文件本身不进仓库
evidence/
  boot-and-stability.log    ← 开机 + 45 分钟稳定性实测
  panel-off-on-failure.log  ← 息屏/亮屏问题的故障现场（修复前）
DRIVER.md                   ← 技术文档（结构、流程、全部修复项、限制）
COMMIT-MESSAGE.txt          ← 建议的提交信息（含 change list）
SHA256SUMS
```

文件校验：

| 文件 | sha256 | 行数 |
| --- | --- | --- |
| `nt36xxx_core.c` | `62bf354e0f0b7717767e9a387281e32e5f2e86078756fa1664160373e0d76cce` | 2098 |
| `nt36xxx.h` | `ffdce5555f3cb9159b09b19482be052de8afd2bccdf683acd98b488590f2682b` | 154 |
| `nt36xxx_spi.c` | `0061c03561b526c796733969960a9b018e98b9cac77c317ce95750fe8fae7300` | 225 |
| `Kconfig` | `710a04f76a2c4b5699bf98fdace3bcd305f12476a6df6acf8128896b7411d801` | — |
| `Makefile` | `221e386531e8e4e11a3f945aa60257e6fdbde8882c1940026a479975afffc21f` | — |

实测所用的内核 Image：`a5f13f9b75048675…`（`7.3.0`，arm64）；
镜像 `shim/out/sky-shim-boot-v4-dt-final.img` sha256 `831227eb…`。

---

## 2. 集成为 4 步

```sh
# 1) 驱动文件
cp -r driver/ <kernel>/drivers/input/touchscreen/nt36xxx-ml/

# 2) Makefile 钩子（已存在则跳过）
#    drivers/input/touchscreen/Makefile
obj-$(CONFIG_TOUCHSCREEN_NT36XXX_ML)	+= nt36xxx-ml/

# 3) Kconfig 钩子（见 §3：二选一，不要留两份）
#    drivers/input/touchscreen/Kconfig
config TOUCHSCREEN_NT36XXX_ML
	tristate "Novatek NT36XXX touchscreen (mainline port, regmap)"
	depends on SPI
	select REGMAP
	help
	  Mainline Novatek NT36XXX SPI touchscreen driver.

# 4) 板级 DTS：把 dts/touch-node.txt 整个节点放进 spi@a84000，并确认
#    a) 控制器节点含 qcom,enable-gsi-dma、dmas/dma-names、ICC 三元组；
#    b) 引用了 pinctrl 组 ts_active（gpio12/13）；
#    c) 存在 panel 节点（本板为 xiaomi,m19-36-02-0a-fhd-dsc）。
```

固件：驱动通过 `firmware-name` 加载 `novatek/nt36672c.bin`，需放进 initramfs
（`.../lib/firmware/novatek/nt36672c.bin`）或 `/lib/firmware`。**版本必须是
FW_VER `0x18`**（原厂随机器固件），详见 `firmware/README.md`。

---

## 3. 主仓库需要顺手修的两个集成问题

1. **Kconfig 符号重复定义**
   `drivers/input/touchscreen/Kconfig` 第 ~694 行已经有内联的
   `config TOUCHSCREEN_NT36XXX_ML`，而 `nt36xxx-ml/Kconfig` 里还有一份同样的定义
   （后者没有被 `source`，属于死文件）。建议：删掉子目录 Kconfig，只保留父级内联那条
   （或反过来只 `source` 子目录那份，二选一）。
2. **`.config` 里该符号是手工加的**
   `CONFIG_TOUCHSCREEN_NT36XXX_ML=y` 能生效是因为 Makefile 直接用了这个变量；
   若要出现在 `menuconfig` 里，必须完成 §2 第 3 步或第 1 条中的一种。

---

## 4. 验收标准（与实测一致）

```sh
# 开机（首次初始化按原厂延迟到 probe 后 14 s）
dmesg | grep -E "sky-fwinfo|Touch IC fw loaded|sky-report"
#   期望：sky-fwinfo: fw_ver=0x18 ~=0xE7 raw=18e710240438099c
#         Touch IC fw loaded ok  （约 14 s）
#         sky-report[1]: 777777777777 state=a3…   ← NORMAL_RUN
grep spi0.0 /proc/interrupts; sleep 5; grep spi0.0 /proc/interrupts   # ≈12 IRQ/s

# 触摸报文
dd if=/dev/input/event3 bs=24 count=100 2>&1 | od -Ax -tx1 -v
#   期望：EV_ABS(03) 0x35/0x36 落在 0..1079 / 0..2459，0x3a 压力随力度变化

# 息屏 → 亮屏（原厂 resume 重刷语义）
echo 4 > /sys/class/graphics/fb0/blank; sleep 5
echo 0 > /sys/class/graphics/fb0/blank; sleep 6
dmesg | grep -E "sky-panel|Touch IC fw loaded" | tail -6
#   期望：disabling → unpreparing → prepared → enabled → 新的 "Touch IC fw loaded ok"
#         之后 state=a3…、12 IRQ/s，无需重启
```

已知正常现象，不要当故障：
* 开机可能打印一次 `sky-recover[1]: reports all 0xFD/0xFE`（首次初始化失败后重试成功）；
* 空闲报文区内容为 `7777…`（这是本 IC 的空闲图样，**不是**固件复位）。

---

## 5. 证据日志对应关系

| 日志 | 说明 |
| --- | --- |
| `evidence/boot-and-stability.log` | 换用原厂 FW 0x18 后：CRC/固件正常、单次恢复、45 分钟无二次复位、空闲 12 IRQ/s |
| `evidence/panel-off-on-failure.log` | 息屏/亮屏故障现场：`sky-state=a0…` 卡在 INIT、CRC 读回全垃圾、208 IRQ/s 风暴（修复前的样貌） |

完整会话归档（每一步的取舍与反例）：`build/archives/handoff11…handoff23-*`，
技术细节与"不要再犯"清单见 `DRIVER.md`。
