# Next prompt: WCN6750 adrastea SNOC 端口继续（session 6 之后）

先读：

1. `docs/wifi-handoff/HANDOFF-snoc-mainline.md`（含 §13、§14）
2. `docs/wifi-handoff/FINDINGS-session6-apss-aperture.md`（本轮决定性实测）
3. `docs/wifi-handoff/FINDINGS-session4-stock-adrastea.md`

设备：Xiaomi Redmi 12 5G（sky），SSH `root@192.168.7.1` / `sky`。
不要通过设备用 adb/fastboot；需要 fastboot 或 dmesg 时先问用户。

当前设备状态（已恢复安全配置）：

```text
镜像 k93（内核 #92）  bdf_total_full=y  snoc_reg_mode=4  snoc_membase_mib=0
snoc_probe=n  snoc_shadow_probe=y  snoc_device_info=n
```

安全网：`wifi-backup/out/sky-shim-boot-v4-wifi-nofdt-recover.img`
（DT 里 wifi `status="disabled"`，内核相同）——参数写坏导致挂死/进 fastboot 时用它救回。

不放手的结论：

```text
membase 8 MiB 内只有 0x22820000/0x22822000/0x22830000/0x22832000 有解码
CE(0x240000) 与全部 UMAC 窗口/直连地址都未解码，整个 bring-up 期间都是 DEAD
SR_CONTROL(ENABLE_APSS_FULL_ACCESS) / SHADOW_ADDRESS / ADDRESS_VALID 对 APSS 只读
原厂 SNOC = TARGET_TYPE_ADRASTEA → 不做寄存器窗口重映射 → 会写 0xa34874
```

下一步只有两条有意义的路：

1. 反汇编原厂 `qca_cld3_*.ko`（`vendor_dlkm.img`），确认它到底写哪个地址、
   有没有窗口/SR 初始化、编的是 adrastea 还是 qca6750 profile。
2. 查 WCSS AHB 时钟/电源域（原厂 `icnss2/power.c` 管 `cxo_ref_clk_pin` 等），
   看是不是 mainline 少了一个 clock/power vote 导致桥接不解码。

禁止项：不要再盲改 `snoc_reg_mode`/窗口猜测；不要发 pre-BDF 的 WLAN_MODE
（`snoc_pre_bdf=5` 会整机挂死）；不要在内核态读未解码地址。

---

## device_info 路线已实测否决（k92/k93）

`snoc_device_info=Y` 下补发 `QMI_WLANFW_DEVICE_INFO_REQ_V01`，固件返回
`qmi device info request failed: 1 94`（94 = NOT_SUPPORTED）。该固件不支持 device_info，
mainline 的 hybrid BAR 路线在本机不可用。`0x2280310C`（窗口选择寄存器）也不解码。
详见 `FINDINGS-session6-apss-aperture.md` §9。

## 下一步备选（原来 device_info 那条作废）

下面保留的是当初的设想，供参考；现在应优先走 §9.3 的 1/2/3：反汇编原厂
`qca_cld3_qca6750.ko` 的 HAL attach/DP 路径、反汇编 `icnss2.ko` 找 SoC 级 remap，
或拿到原厂串口日志看 `MEM_BASE pa/va`。

（历史设想）寄存器基址本想来自 WLFW `device_info` 返回的 2 MiB BAR：

1. `qca-wifi-host-cmn/qmi.c` 里 `ath11k_qmi_request_device_info()` 已有完整实现
   （`devm_ioremap(resp.bar_addr, resp.bar_size)` → `ab->mem`），
   但它在 `!hybrid_bus_type` 时直接 return；`patch 0003` 在 `ahb.c:1526`
   把 SNOC 的 `hybrid_bus_type` 置成了 false。
2. 新开一个开关 `snoc_device_info=Y`（或让 SNOC 分支保留 hybrid 标记但不走
   `ath11k_ahb_setup_msi_resources()`）：
   - 先发 `QMI_WLANFW_DEVICE_INFO_REQ_V01`，**打印 `bar_addr`/`bar_size`，
     不要先做 `bar_size == 0x200000` 的硬校验**；
   - 拿到后用该 BAR 做 `ab->mem`，`snoc_reg_mode=3`（纯静态窗口映射）跑 DP。
3. 环境里已有 `skyreg.ko` + `skyprobe`（`wifi-backup/regtool/`），
   拿到 bar_addr 后可以先在用户态把 `BAR+0xB4874`（UMAC WBM）和
   `BAR+0x118000`（CE0）读一遍，确认解码再动内核路径。
