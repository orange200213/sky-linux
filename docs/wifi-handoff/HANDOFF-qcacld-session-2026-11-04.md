# qcacld 上机调试交接（2026-11-04，本轮结束状态）

> 详细日志与逐轮记录见 `FINDINGS-qcacld-device-test-2026-11-04.md`（923+ 行）。
> 本文只讲"现在在哪、下一步怎么做"。

## 1. 已达成的里程碑

| 项 | 状态 | 证据 |
|---|---|---|
| DT icnss 节点（含 smp2p 三路、MSA 12 MiB、触屏修复） | ✅ 已合入并上机 | 活体 `/sys/firmware/fdt` sha 与本地 DTB 一致 |
| `wlan_firmware_service.ko` 加载 | ✅ rc=0 | 导出 93 个 `wlfw_*`，覆盖 icnss2 需要的 64 个 |
| **`icnss2.ko` probe 成功** | ✅ | `Platform driver probed successfully`；`QMI Server Connected: state 0x480` |
| msa_mem_info / msa_ready / cap_req | ✅ 全通 | capability 与原厂 logcat 一致（chip_id 0x4130…） |
| **REGDB + BDF 下载** | ✅ 成功 | 无 `result=1`；之前的失败是**固件文件被 USB 传成全零** |
| **cal report → `WLAN FW is ready: 0x487`** | ✅ | 与原厂状态完全相同 |
| `wlan.ko`（qcacld）加载 | ✅ rc=0，注册驱动成功 | `Registering driver, state: 0x485` |
| **qcacld probe** | ❌ 返回 **-1** | `icnss2: Driver probe failed: -1, state: 0x20487, probe_cnt: 1` |

## 2. 本轮定位的两大真因

### 2.1 icnss2 取 IRQ 用错 API（已修）
`main.c` ADRASTEA 分支原来用 `platform_get_resource(pdev, IORESOURCE_IRQ, i)`，
而我们的平台设备**没有资源表**（`num_resources == 0`）→ 必然 `Fail to get IRQ-0`。
改为 `platform_get_irq(pdev, i)`（内部回退 DT 解析）后 probe 一路通过。

### 2.2 BDF 下载被拒 = 固件文件内容全零（已修）
主机 dmesg：`usb 1-1: can't set config #1, error -71` + `USB disconnect`。
设备上 `/lib/firmware/adrastea/*.bin` **大小正确但内容是 0x00**。
换线 + 换口 + **逐文件 md5 双向校验**后，REGDB/BDF 下载与 cal report 全部成功。

## 3. 当前唯一卡点：qcacld probe 返回 -1

### 3.1 现象
```
wlan: Loading driver v2.0.9.17N +PANIC_ON_BUG
Registering driver, state: 0x485
Posting/Processing event: REGISTER_DRIVER(3)
HW Power on: state: 0x485          ← regulator 全部使能
icnss2: Driver probe failed: -1, state: 0x20487, probe_cnt: 1
HW Power off: 0x487                ← 回退关电
```
* qcacld **一行日志都没输出**（`hdd_info("probing driver")` 都看不到）
* `-1` = `-EPERM`，`probe_cnt: 1` 表示未走 `-EPROBE_DEFER` 重试
* 加 `qdf_log_dump_at_kernel_enable=1` 无变化；建好 `/data/vendor/wifi{,/wlan,/wlan_logs,/pktlog}`
  与 `/data/misc/wifi/sockets` 也无变化

### 3.2 调用链（源码位置）
`icnss2/main.c:1477` → `priv->ops->probe(&priv->pdev->dev)`
→ `qcacld-3.0/core/hdd/src/wlan_hdd_driver_ops.c:1753 wlan_hdd_pld_probe()`
→ `hdd_soc_probe()` → `__hdd_soc_probe()`，五个可能提前失败的点：

1. `hdd_init_qdf_ctx(dev, bdev, bus_type, bid)`
2. `hdd_init_dma_mask(dev, bus_type)`
3. `hdd_context_create(dev)`
4. `dp_prealloc_init(...)`（`qdf_status_to_os_return()`）
5. `hdd_wlan_startup(hdd_ctx)`

## 4. 下一步（按推荐顺序）

1. **给 qcacld 加最简打印并重编 `wlan.ko`**（源码在
   `third_party/micode-sky-t-oss/qcacld-3.0/`，构建入口 `third_party/micode-sky-icnss2/port.mk`
   的 `wlan:` 目标）。在 `__hdd_soc_probe()` 的 5 个调用点各加一行
   `pr_err("sky: step N ret=%d\n", errno)`，一次就能定位。
2. 若不想重编：给 **icnss2** 加打印（`priv->ops`、`priv->ops->probe` 指针、
   `pld_bus_type` 实参），确认传参正确（`wlan_hdd_pld_probe` 第一步会
   `to_bus_type(pld_bus_type)`，若得到 `QDF_BUS_TYPE_NONE` 会直接 `-EINVAL`）。
3. 检查 `qdf_status_to_os_return()` 的映射（qcacld 里对 `QDF_STATUS_E_FAILURE`
   可能就返回 `-1`），确认 -1 来源。

## 5. 运维经验（务必沿用）

| 场景 | 做法 |
|---|---|
| 重启 | `sync; sync; sleep 1; echo b > /proc/sysrq-trigger`；**不要用 `reboot`**（会挂住） |
| 落进 fastboot | `fastboot flash boot_a sky-mainline/shim/out/sky-shim-boot-v4.img`（内嵌 DTB sha `b1b0d1d7…`）+ `fastboot reboot` |
| 传文件 | 每个文件 **md5 双向校验**（USB 链路不稳时内容会变全零，大小却是对的） |
| 卸载模块 | 不可能：`CONFIG_MODULE_UNLOAD=n`，`rmmod` 报 `Function not implemented` ⇒ **一次开机只能 insmod 一次** |
| 模块存放 | 推 `/root` 并 `sync`，不要放 `/tmp` |

## 6. 一次性加载命令（每次重启后）

```sh
insmod /root/wlan_firmware_service.ko
insmod /root/icnss2.ko                 # 等 ~8s，期望 "WLAN FW is ready: 0x487"
insmod /root/wlan.ko [qdf_log_dump_at_kernel_enable=1]
```

## 7. 文件清单

```
模块      /root/wlan_firmware_service.ko (134,920)  /root/icnss2.ko (809,992, 含编码 dump)  /root/wlan.ko (62,481,352)
固件      /lib/firmware/adrastea/{regdb.bin 19348, bdwlant_cn.bin 26328, bdwlant_jp.bin 26328, wpss.mdt 4684}
配置      /lib/firmware/wlan/qca_cld/adrastea/WCNSS_qcom_cfg.ini (13,913, 取自原厂 vendor)
源码      sky-linux/third_party/micode-sky-icnss2/（icnss2 + 构建入口 port.mk）
          sky-linux/third_party/micode-sky-t-oss/qcacld-3.0/（qcacld）
DTS       sky-mainline/dts/sm4450-xiaomi-sky.dts（attic/ 存两份历史版）
镜像      sky-mainline/shim/out/sky-shim-boot-v4.img (120,086,528, DTB sha b1b0d1d7…)
备份      wlan_firmware_service_v01.{c,h}.bak-20261104（本轮未改动 WLFW 头文件）
```
