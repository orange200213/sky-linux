# qcacld 首次上机测试报告（2026-11-04）

测试对象：`artifacts/sky-wifi-test-artifacts-2026-11-04.tar.gz`
（sha256 `78a53ffd…`，包内 sha256sums 全部校验通过）
测试设备：Redmi 12 5G / sky（SM4450 + WCN6750 adrastea），内核 `7.3.0-g78458d4eea4e-dirty #112`
镜像：`shim/out/sky-shim-boot-v4-dt-final.img`（触屏修复版，#112，2026-11-04 12:15）

---

## 1. 结论（一句话）

**模块本身构建正确，但当前内核/DT 缺少两个必要条件，无法完成最小加载：**

1. **符号未导出**：`icnss2.ko` 引用 64 个 `wlfw_*_msg_v01_ei`，内核只有 37 个（在 ath10k 的
   `qmi_wlfw_v01.c` 里，且 **没有 `EXPORT_SYMBOL`**）；**完全缺失 27 个**。
2. **设备树无 `qcom,icnss` 节点**：当前 DTS 只有 ath11k 的 `wifi@22800000`
   （`qcom,wcn6750-wifi`），`icnss2` 即使符号齐全也无处 probe。

⇒ 任务从这里进入"补符号 + 补 DT"阶段，不需要改模块代码。

---

## 2. 阶段 A：静态核对（全部通过的部分）

| 检查 | 结果 |
|---|---|
| 压缩包 sha256 | ✅ `78a53ffdbf74a0fdf737c1a2ba59a1d353694fe574ccb21a7e1202a3e10a79f5` |
| 包内 `metadata/sha256sums.txt` | ✅ 全部"成功" |
| `icnss2.ko` | 780,680 B，sha256 前缀 `558983e2…` |
| `wlan.ko` | 62,481,352 B，sha256 前缀 `6fcbd84d…` |
| vermagic（两个） | ✅ `7.3.0-g78458d4eea4e-dirty SMP preempt aarch64` |
| 运行内核 | ✅ 同版本 `7.3.0-g78458d4eea4e-dirty #112`（ABI 一致） |
| `wlan.ko` 依赖方向 | ✅ 依赖 `icnss2`（396 个未定义符号，含 `icnss_*` / `pld_*`） |
| `wlan.ko` 是否携带 `wlfw_*` | ❌ 0 个（既不定也不引 ⇒ 不是它提供） |

## 3. 阶段 B：最小加载（失败，已定位根因）

### 3.1 操作

```sh
scp modules/icnss2.ko modules/wlan.ko root@192.168.7.1:/tmp/
insmod /tmp/icnss2.ko
```

### 3.2 结果

```text
insmod icnss2 rc=2
insmod: can't insert '/tmp/icnss2.ko': unknown symbol in module, or unknown parameter
[  232.852461] icnss2: Unknown symbol wlfw_qdss_trace_mode_resp_msg_v01_ei (err -2)
[  232.853296] icnss2: Unknown symbol wlfw_vbatt_req_msg_v01_ei (err -2)
[  232.854120] icnss2: Unknown symbol wlfw_msa_info_req_msg_v01_ei (err -2)
…（连续 12 条以上同类）
```

`wlan.ko` 未尝试加载（`icnss2` 未成功，按检查表顺序停止）。

### 3.3 符号级核对（在主机上做，`nm`）

```
icnss2.ko 未定义符号总数             : 239
  其中 wlfw_*_msg_v01_ei           : 64
内核 vmlinux 已定义 wlfw_*          : 37（在 drivers/net/wireless/ath/ath10k/qmi_wlfw_v01.c）
内核 Module.symvers 中导出的 wlfw_* : 0     ← 关键
内核完全缺失的 wlfw_*              : 27
```

**已定义但未导出（37 个）**：加 `EXPORT_SYMBOL` 即可解决，例如
`wlfw_bdf_download_req_msg_v01_ei`、`wlfw_cal_report_req_msg_v01_ei`、`wlfw_wlan_mode_req_msg_v01_ei`、
`wlfw_msa_info_req_msg_v01_ei`、`wlfw_athdiag_read_req_msg_v01_ei` 等（完整清单见 §6）。

**完全缺失（27 个，必须补生成）**：

```
wlfw_cal_done_ind_msg_v01_ei
wlfw_device_info_req_msg_v01_ei          wlfw_device_info_resp_msg_v01_ei
wlfw_get_info_req_msg_v01_ei             wlfw_get_info_resp_msg_v01_ei
wlfw_m3_dump_upload_done_req_msg_v01_ei  wlfw_m3_dump_upload_done_resp_msg_v01_ei
wlfw_m3_dump_upload_segments_req_ind_msg_v01_ei
wlfw_power_save_req_msg_v01_ei
wlfw_qdss_trace_config_download_req_msg_v01_ei   wlfw_qdss_trace_config_download_resp_msg_v01_ei
wlfw_qdss_trace_data_req_msg_v01_ei              wlfw_qdss_trace_data_resp_msg_v01_ei
wlfw_qdss_trace_free_ind_msg_v01_ei
wlfw_qdss_trace_mem_info_req_msg_v01_ei          wlfw_qdss_trace_mem_info_resp_msg_v01_ei
wlfw_qdss_trace_mode_req_msg_v01_ei              wlfw_qdss_trace_mode_resp_msg_v01_ei
wlfw_qdss_trace_req_mem_ind_msg_v01_ei
wlfw_qdss_trace_save_ind_msg_v01_ei
wlfw_respond_get_info_ind_msg_v01_ei
wlfw_shutdown_req_msg_v01_ei             wlfw_shutdown_resp_msg_v01_ei
wlfw_soc_wake_req_msg_v01_ei             wlfw_soc_wake_resp_msg_v01_ei
wlfw_subsys_restart_level_req_msg_v01_ei wlfw_subsys_restart_level_resp_msg_v01_ei
```

## 4. 阶段 C/D/E：未执行

`icnss2` 未加载 ⇒ 无 wiphy、无扫描、无 ROC/mgmt_tx、无 SSR。按检查表要求**保留失败现场**：
两个 `.ko` 仍在设备 `/tmp/`（`icnss2.ko` `ee72b016…`、`wlan.ko` `9dcafa02…`），
未卸载、未清理任何日志。

## 5. 修复建议（按顺序，都很小）

### 5.1 补 WLFW 服务符号（阻塞点 1）

两条路，推荐 **(b)**：

**(a) 在 ath10k 里导出（快，但只解决 37 个）**

```c
/* drivers/net/wireless/ath/ath10k/qmi_wlfw_v01.c 末尾 */
EXPORT_SYMBOL(wlfw_bdf_download_req_msg_v01_ei);
/* …对其余 36 个同样处理… */
```
并在 `qmi_wlfw_v01.c` 里补上缺失的 27 个表（等于把 vendor 的
`wlan_firmware_service_v01.c` 增量移植进来）。

**(b) 用完整的 WLFW 服务模块（推荐）**

从 vendor 源取 `wlan_firmware_service_v01.c/.h`（icnss2 同版本），做成独立模块：

```make
obj-m += wlan_firmware_service.o
wlan_firmware_service-objs := wlan_firmware_service_v01.o
```
并给全部 `wlfw_*_ei` 加 `EXPORT_SYMBOL`，**先 insmod 它，再 insmod icnss2**。
好处：64 个符号一次补齐、与 icnss2 版本严格对应、不需要动 ath10k。

> 参考：Android 原厂 ROM 里就是独立模块 `/vendor/lib/modules/wlan_firmware_service.ko`（73 KB），
> 它是 `icnss2.ko` 的前置依赖（原厂 `lsmod` 里 `wlan_firmware_service` refcount=2：cnss2 + icnss2）。

### 5.2 补 `qcom,icnss` 设备树节点（阻塞点 2）

当前 `dts/sm4450-xiaomi-sky.dts` 只有 `wifi@22800000`（ath11k）。需要新增 icnss 节点，
**模板见 `FINDINGS-dt-references-wcn6750-adrastea.md` §8**（含 12 管 CE 中断、`0x4c0` SMMU、
`bdf-download-support`/`wpss-support-enable`/`qcom,fw-prefix`、ch0/ch1 稳压器）。
注意两点：

* `wlan_msa_mem` 目前是 **3 MiB**，原厂是 **12 MiB**（`0xc00000`）；
* 需要 `smp2p_wlan_1_out` / `smp2p_wlan_1_in` 标签（`qcom,smp2p-wpss` 下 `wlan` 通道）。

### 5.3 固件

已就位：`/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.*` = 原厂 2024 版（`ccf4f902…`）；
BDF 走 QMI 下载（`bdf-download-support`），需确认 `/lib/firmware` 下有 `regdb.bin` /
`bdwlant_jp.bin`（可从 `NON-HLOS.bin` 的 `image/adrastea/bdwlan.*` 取得）。

## 6. 附：37 个"已定义未导出"清单

见本次测试保存的 `/tmp/wlfw_present.txt`（主机侧），如需在报告中完整列出可追加。

## 7. 复现命令

```sh
# 主机
sha256sum -c metadata/sha256sums.txt
nm -u modules/icnss2.ko | grep wlfw_ | wc -l      # → 64
# 设备
insmod /tmp/icnss2.ko                              # → unknown symbol
dmesg | grep -c "Unknown symbol"                   # → 实测 12+ 条
```

---

# 追加：第二轮上机（2026-11-04 晚）—— icnss2 已能加载

## 1. 关键修复：**镜像打包方式用错了**

之前用 `wifi-backup/repack-boot.py` 打包 → shim 报
`payload overlaps the kernel or DTB` / `HALT payload did not start Linux`，原因：
该脚本按**固定偏移**覆写（假定 Image = 119,929,344 B），而新内核 Image 更大，压到 DTB 区。

**正确做法**（`sky-mainline/shim/` 内）：

```sh
./make-m1.sh ../kernel/linux-7.3/arch/arm64/boot/Image /path/to/dtb
# 内部：tools/mklinuxpayload.py（Image+DTB 打包） + build-sky.sh（套 shim 头）
# 产物 out/sky-shim-boot-v4.img，大小应与已知可用镜像一致：120,086,528 B
```
另外**不能**把旧 boot 镜像 `4096..kernel_size` 里的内容当内核 Image 用 —— 那是 shim payload，
不是裸 Image（会导致同样的 HALT）。

## 2. DT 合并与验证（已完成）

* 合并版：`sky-mainline/dts/sm4450-xiaomi-sky.dts`（84 KB，`attic/` 保留两份旧版）
* 新增 `interrupt-parent = <&intc>;`（本轮追加）
* DTB 63,612 B；**运行中 `/sys/firmware/fdt` sha256 与本地 DTB 一致** ✅
* 活体节点 `/proc/device-tree/soc@0/wifi@22800000`：
  `compatible=qcom,icnss`、`status=okay`、`interrupts`(12)、`interrupt-parent`、
  `iommus`、`qcom,wlan`、`bdf-download-support`、`wpss-support-enable`、`qcom,fw-prefix` 全在

## 3. 模块加载：**成功了**

```
insmod /tmp/wlan_firmware_service.ko   → rc=0   （lsmod: 69632）
insmod /tmp/icnss2.ko                  → rc=0   （lsmod: 131072）
dmesg:
  wlan_firmware_service: loading out-of-tree module taints kernel.
  Unable to create log context ×5        ← ipc_log_context_create 失败，仅日志功能缺失
  icnss2: Recursive recovery allowed for WLAN
  icnss2: Fail to get IRQ-0              ← 当前阻塞点
```

**要点**：`wlan_firmware_service.ko` 不在 artifacts 包里，但在主机
`sky-linux/third_party/micode-sky-icnss2/wlan_firmware_service.ko`（vermagic 一致，
导出 93 个 `wlfw_*` 符号，完整覆盖 icnss2 需要的 64 个）。
它自身 init 会检查 DT 是否有 `qcom,wlan` 节点 —— 所以**必须先把 icnss 节点刷进 DT**，
否则 insmod 直接 `-ENODEV`（这就是上一轮 rc=19 的原因）。

## 4. 下一个待解：`Fail to get IRQ-0`

已知事实：节点存在、`interrupts` 12 条、`interrupt-parent` 已加、`status=okay`。
可能原因（按可能性）：

1. `interrupt-parent` phandle 指向的不是 GIC（`&intc` 在本 DTS 里可能被别的节点占用）；
2. icnss2 用的是 `platform_get_irq()`，而 DT 里 GIC 的 `#interrupt-cells` 与实际
   `<GIC_SPI n FLAGS>` 三元组不匹配（需要确认父节点）；
3. 驱动初始化顺序问题：需要在 GIC/irqchip 就绪后再 probe（可试 `insmod` 前
   `sleep` 或改用内置驱动）；
4. 厂商驱动可能有额外的 IRQ 名称要求（如 `qcom,wlan-msi-*`）。

建议下一步：核对 `interrupt-parent` 的 phandle 与 GIC 节点是否一致；
若不一致，把节点的 `interrupt-parent` 指向真正的 GIC 标签；再重打包/重刷重测。

## 5. 第三轮：`wlan.ko` 也试了，确认阻塞链条

```
insmod /tmp/wlan.ko   → rc=22 (EINVAL)
dmesg: wlan: Loading driver v2.0.9.17N +PANIC_ON_BUG
       wlan_pld:pld_register_driver:354:: Fail to register snoc driver
```

⇒ `wlan.ko` 依赖 `pld_register_driver(snoc)`，而 snoc 驱动由 **icnss2 的 probe 成功**才注册。
所以链条是：

```
DT 节点 ✅ → wlan_firmware_service.ko ✅ → icnss2.ko ✅ → icnss2 probe ❌(IRQ-0)
                                                        └→ pld snoc 未注册 → wlan.ko ❌(rc=22)
```

## 6. IRQ 卡点的分析结论

1. **`icnss2.ko` 未定义符号里没有 `platform_get_irq`**，但有 **`of_irq_get_byname`**；
   后者的三个字符串参数已从 `.rodata` 还原：
   ```
   .rodata+0x1b60 = "qcom,smp2p-force-fatal-error"     ← qcom,smp2p_map_wlan_1_in
   .rodata+0x1c48 = "qcom,smp2p-early-crash-ind"       ← qcom,smp2p_map_wlan_1_in
   .rodata+0x1a58 = "qcom,smp2p-soc-wake-ack"          ← qcom,smp2p_map_wlan_2_in
   ```
2. **我们 DT 缺 `qcom,smp2p_map_wlan_2_in` 子节点**（活体检查：`grep -c smp2p_map_wlan_2_in` = 0），
   而原厂 DTBO 有：
   ```dts
   qcom,smp2p_map_wlan_2_in {
           interrupts-extended = <&smp2p_wlan_2_in 0 0>;
           interrupt-names = "qcom,smp2p-soc-wake-ack";
   };
   ```
   —— 这会导致 `of_irq_get_byname(..., "qcom,smp2p-soc-wake-ack")` 返回负值。
3. 而 `Fail to get IRQ-0` 的打印里 `%d` 是**索引**，索引 0 对应的是 `priv->irq[0]`
   （CE IRQ 数组）。该数组在 probe 早期由设备资源/DT 填充；当前失败说明
   **取 CE IRQ 的那一步没有拿到值**。下一步需要把 `icnss_probe` 里那段循环反汇编出来
   （用 `.rodata+0x2000` 页偏移匹配字符串，再回溯 `bl`），确认它到底走
   `platform_get_irq` / `pdev->resource[]` / `of_irq_get` 哪条路径。

## 7. 下一步（建议按顺序）

1. **DT 补 `qcom,smp2p_map_wlan_2_in`**（以及对应 `smp2p_wlan_2_in/out` 标签，
   原厂 `qcom,smp2p-wpss` 里的 `wlan_soc_wake` 通道）→ 重打包重测；
2. 反汇编 `icnss_probe` 的 IRQ 循环，确定取 IRQ 的 API；若确为 `platform_get_irq`，
   则问题在「节点没有生成 IRQ 资源」，需要检查 `soc@0` 是否为 `simple-bus`、
   节点是否被正确 populate；
3. 若 CE IRQ 必须靠 `interrupt-names`，在节点上补 12 条 CE 名字（从原厂
   `qca_cld3_adrastea.ko` 的 `ce_irq_list`/`irq_entry` 字符串或 icnss2 源码确认）。

## 8. 复现/迭代流程（本轮验证过）

```sh
# 1) 改 DTS
vim sky-mainline/dts/sm4450-xiaomi-sky.dts
# 2) 编译 DTB（不要用 tools/mksky_dts.py，它会覆盖 DTS）
K=sky-mainline/kernel/linux-7.3
cpp -nostdinc -I $K/include -I $K/arch/arm64/boot/dts/qcom -I sky-mainline/dts \
    -undef -x assembler-with-cpp sky-mainline/dts/sm4450-xiaomi-sky.dts -o /tmp/sky.pre.dts
dtc -I dts -O dtb -o /tmp/sky.dtb /tmp/sky.pre.dts
# 3) 打包（必须用这个；repack-boot.py 是固定偏移，会 overlap）
cd sky-mainline/shim && ./make-m1.sh ../kernel/linux-7.3/arch/arm64/boot/Image /tmp/sky.dtb
#    产物 out/sky-shim-boot-v4.img，正确大小应为 120,086,528 B
# 4) 刷入（设备在线时）或 fastboot
scp out/sky-shim-boot-v4.img root@192.168.7.1:/tmp/g.img
ssh root@192.168.7.1 'dd if=/tmp/g.img of=/dev/sde13 bs=4096 && sync && echo b > /proc/sysrq-trigger'
# 5) 验证 DT 生效
ssh root@192.168.7.1 'sha256sum /sys/firmware/fdt'   # 应与 /tmp/sky.dtb 一致
# 6) 模块链
insmod /tmp/wlan_firmware_service.ko && insmod /tmp/icnss2.ko && insmod /tmp/wlan.ko
```

## 9. 第四轮：补 smp2p 节点后仍失败（已排除该假设）

### 9.1 本轮 DT 改动

```dts
/* smp2p-wpss 新增三路（对齐原厂） */
wlan_smp2p_2_out: wlan-soc-wake-ap-to-wpss   { qcom,entry-name = "wlan_soc_wake"; #qcom,smem-state-cells = <1>; };
wlan_smp2p_2_in:  wlan-soc-wake-wpss-to-ap   { qcom,entry-name = "wlan_soc_wake"; interrupt-controller; #interrupt-cells = <2>; };
wlan_smp2p_3_out: wlan-ep-powersave-ap-to-wpss { qcom,entry-name = "wlan_ep_power_save"; #qcom,smem-state-cells = <1>; };

/* icnss 节点新增/补齐 */
qcom,smp2p_map_wlan_2_in {
        interrupts-extended = <&wlan_smp2p_2_in 0 0>;
        interrupt-names = "qcom,smp2p-soc-wake-ack";
};
qcom,smem-states = <&wlan_smp2p_out 0>, <&wlan_smp2p_2_out 0>, <&wlan_smp2p_3_out 0>;
qcom,smem-state-names = "wlan-smp2p-out", "wlan-soc-wake-smp2p-out", "wlan-ep-powersave-smp2p-out";
```

DTB 64,084 B；刷入后活体 `/sys/firmware/fdt` sha256 = `b1b0d1d778f1da020fcf1374`，
与本地 `/tmp/sky2.dtb` **一致**（DT 确实生效）。

### 9.2 结果：仍然 `Fail to get IRQ-0`

```
insmod /tmp/wlan_firmware_service.ko  → rc=0
insmod /tmp/icnss2.ko                 → rc=0
dmesg: icnss2: Recursive recovery allowed for WLAN
       icnss2: Fail to get IRQ-0
```

⇒ **缺 `qcom,smp2p_map_wlan_2_in` 不是 IRQ-0 的原因**（该假设已排除）。

### 9.3 反汇编定位未成功，下一步建议换方法

尝试用 `ADRP .rodata+0x2000` + `ADD #0x1ca`（字符串 `.rodata+0x21ca`）匹配
objdump 输出，**0 命中** —— 说明该字符串不是通过标准 PG_HI21/LO12 对引用的
（可能走了别的 reloc 形式或经 GOT）。

替代方法（任选其一）：

1. `readelf -r icnss2.ko | grep -i "21ca\|0x21c"` —— 直接找引用该 rodata 偏移的
   重定位项，再按其所在节偏移定位函数；
2. 用 **原厂 `jp_icnss2.ko`**（5.10，有 kallsyms 重定位）反汇编同一字符串附近的代码，
   看它调用的是 `platform_get_irq` 还是别的（函数名会显示在 reloc 里，比 mainline
   版更容易读）；
3. 直接在本机重编 `icnss2.ko` 时给 `icnss_get_irq()` 加一行打印（需要 icnss2 源码，
   目前本机**没有** `icnss2/` 源码树，只有 `third_party/micode-sky-icnss2/` 里的
   wlan_firmware_service 部分 —— 这点需要先补齐）。

### 9.4 本轮确认的依赖链

```
wl.ko ──依赖──> pld_register_driver(snoc) ──由──> icnss2 probe 成功
                                                    └─ 目前卡在 IRQ-0
```

## 10. 第五轮：找到 `Fail to get IRQ-0` 的真正机制（源码 + 设备侧证据）

### 10.1 源码（`third_party/micode-sky-icnss2/drivers/soc/qcom/icnss2/main.c`）

ADRASTEA 分支在 probe 里**按平台资源表**取 12 条 CE IRQ：

```c
/* main.c:4114 - 4123 */
for (i = 0; i < ICNSS_MAX_IRQ_REGISTRATIONS; i++) {
        res = platform_get_resource(priv->pdev, IORESOURCE_IRQ, i);
        if (!res) {
                icnss_pr_err("Fail to get IRQ-%d\n", i);   /* main.c:4117 */
                ret = -ENODEV;
                goto put_clk;
        }
        priv->ce_irqs[i] = res->start;
}
/* main.c:3490 icnss_get_irq() 只是返回 priv->ce_irqs[ce_id] */
```

⇒ 打印 `Fail to get IRQ-0` 表示 **`platform_get_resource(pdev, IORESOURCE_IRQ, 0)` 返回 NULL**，
也就是**平台设备根本没有 IRQ 资源**（不是 DT 属性缺失）。

### 10.2 设备侧实况（关键证据）

```
/sys/devices/platform/soc@0/22800000.wifi/
  of_node -> ../../../../firmware/devicetree/base/soc@0/wifi@22800000
  无 resource 文件                      ← pdev->num_resources == 0
  无 driver 符号链接                    ← 驱动 probe 失败后未绑定
  supplier:platform:15000000.iommu / 17a00000.rsc:regulators-0,1 / smp2p-wpss
uevent: OF_COMPATIBLE_0=qcom,icnss  MODALIAS=of:NwifiT(null)Cqcom,icnss
/sys/bus/platform/drivers/icnss2        ← 驱动已注册（名字是 "icnss2"）
/sys/kernel/debug/devices_deferred      ← 空（不是 deferred probe）
```

DT 侧编码是对的：

```
soc@0: #address-cells=2  #size-cells=2  compatible="simple-bus"  ranges 存在
wifi@22800000/reg        = 16 字节 = 2+2 cells   (<0x0 0x22800000 0x0 0x800000>)
wifi@22800000/interrupts = 144 字节 = 12×3 cells
```

⇒ 结论：**节点属性都对，但平台设备没有被创建出资源**。
`platform_get_resource()` 依赖 `pdev->resource[]`，而它是 `of_device_alloc()` 在
创建设备时用 `of_address_to_resource()` / `of_irq_to_resource_table()` 填的。
num_resources = 0 说明这两步都没有产出（注意 ath11k 的同址节点之前是能拿到
`membase` 并 ioremap 成功的，说明 `ranges` 翻译本身没问题）。

### 10.3 下一步（明确的三条）

1. **检查设备是否被创建了两次/被覆盖**：`of_platform_populate` 与 `platform_device_register`
   混用时资源可能为空；可 grep 内核日志 `dmesg | grep -i "22800000"` 看创建时的报错。
2. **确认驱动匹配路径**：驱动名是 `icnss2`，modalias 是 `of:NwifiT(null)Cqcom,icnss`；
   若 `.of_match_table` 里的 compatible 是 `"qcom,icnss"`，匹配正常，问题只在资源。
3. **绕过资源表**：在 `icnss2/main.c` 的 ADRASTEA 分支把
   `platform_get_resource(pdev, IORESOURCE_IRQ, i)` 换成
   `of_irq_get(dev->of_node, i)`（或 `platform_get_irq(pdev, i)`，它会自动回退到 DT 解析），
   重编 `icnss2.ko` 再测 —— 这是**最快能验证"DT 是对的、只是资源没建"**的改法。
   （源码树、内核树都在，`port.mk` 可重编。）

## 11. 第六轮：**icnss2 probe 成功**（IRQ 修复已验证），阻塞点推进到 QMI BDF 下载

### 11.1 修复（已生效）

`third_party/micode-sky-icnss2/drivers/soc/qcom/icnss2/main.c` ADRASTEA 分支：

```c
/* 原：platform_get_resource(pdev, IORESOURCE_IRQ, i) —— 平台设备无资源表 → 必失败 */
ret = platform_get_irq(priv->pdev, i);      /* 改：内部回退到 DT 解析 */
if (ret < 0) { icnss_pr_err("Fail to get IRQ-%d (rc %d)\n", i, ret); ret = -ENODEV; goto put_clk; }
priv->ce_irqs[i] = ret;
```

重编：`make -f third_party/micode-sky-icnss2/port.mk platform` → `icnss2.ko` 781,048 B。

> 注意：模块必须推到 **/root** 而不是 /tmp —— 本轮发现 /tmp 下的副本会变成 0 字节/ELF magic 丢失
> （`Invalid ELF header magic: != ELF`，insmod rc=8）。推完务必 `md5sum` + `head -c4 | xxd` 校验。

### 11.2 结果：probe 成功、QMI 连上

```
insmod /tmp/wlan_firmware_service.ko   → rc=0
insmod /root/icnss2-new.ko             → rc=0
icnss2: Recursive recovery allowed for WLAN
icnss2: SMMU IOVA IPA not found
icnss2: Platform driver probed successfully        ← ★ probe 通过
icnss2_qmi: QMI Server Connected: state: 0x400 / 0x480
icnss2_qmi: Invalid Hang Data details, Offset:0x0, Length:0x0
```

### 11.3 新阻塞点：固件缺文件 → 补上后被固件拒绝

先是因为 `/lib/firmware/adrastea/` 里没有文件（最初 scp 静默失败产生 0 字节文件），
补齐后（`regdb.bin` 19,348 B、`bdwlant_cn.bin`/`bdwlant_jp.bin` 各 26,328 B、`wpss.mdt` 4,684 B）：

```
icnss2_qmi: get_board_id_from_dtb:device project-info node not exist.   ← DT 缺 project-info
icnss2_qmi: REGDB download request failed, result: 1, err: 1            ← QMI 通用失败
icnss2_qmi: BDF download request failed, result: 1, err: 1
```

即 firmware 的 QMI 服务端已经起来（能连），但**拒绝 BDF/REGDB 下载请求**（result=1/err=1）。

### 11.4 下一步（按可能性）

1. **DT 补 `project-info` 节点**（决定 BDF 变体：原厂日版用 `bdwlant_jp.bin`）；
   当前因为缺它，icnss2 选择了 `bdwlant_cn.bin`。
2. **核对 WLFW 握手顺序**：stock 在 BDF 下载前有 `msa_mem_info` → `msa_ready` → `cap_req`；
   我们 DT 的 `qcom,wlan-msa-fixed-region` 指向 12 MiB 的 `wlan-msa@82a00000`（已对齐原厂），
   需要确认 icnss2 是否真的把这些请求发出去了（可在 `qmi.c` 加打印或看 `state`）。
3. 检查固件版本匹配：`/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.mbn` = 2024 原厂版
   （md5 见 §8），与 `adrastea/` 下的 BDF/regdb 同批（都来自日版 NON-HLOS.bin）。
4. 若 `result=1/err=1` 持续，需对比原厂 `qmi.c` 在 ADRASTEA 分支发送 `bdf_download` 的
   **字段内容**（如 `bdf_type`、`end`、`seg_id`、`file_id`）是否与 mainline 版本一致。

## 12. 第七轮：打开 icnss2 调试打印 —— **握手序列完全正确，只有 BDF 下载被拒**

### 12.1 怎么打开打印

`icnss_pr_dbg` 走 `pr_debug`，但内核**没有** `CONFIG_DYNAMIC_DEBUG`（无
`/sys/kernel/debug/dynamic_debug/control`）。做法：改 `debug.h` 把
`icnss_pr_dbg` 提升为 `printk(KERN_INFO ...)`，用 `port.mk` 重编（`icnss2.ko` 808,064 B）。

> 另一个坑：`/root` 下推送的模块偶尔"消失"（scp 看似成功但重启后不存在）；
> 每次都要 `ls -l` + `md5sum` 双重确认再 `insmod`。

### 12.2 完整序列（与原厂逐条对齐）

```text
icnss2: Recursive recovery allowed for WLAN
MSA pa: 0x0000000082a00000, MSA Memory Size: 0xc00000        ← 12 MiB（与原厂一致）
icnss2: SMMU IOVA IPA not found
WLFW server arrive: node 13 port 9
icnss2: Platform driver probed successfully
icnss2_qmi: QMI Server Connected: state: 0x480
Sending MSA mem info, state: 0x481
Sending MSA ready request message, state: 0x481
Received MSA Ready Indication
icnss2_qmi: Invalid Hang Data details, Offset:0x0, Length:0x0    ← hang data 为空，正常
Sending target capability message, state: 0x481
Capability, chip_id: 0x4130, chip_family: 0x4007, board_id: 0xff, soc_id: 0x406a0000
        ↑ 与原厂 logcat 的 wlfw_send_cap_req 输出**完全一致**
File added with prefix: adrastea/regdb.bin
Downloading REGDB: adrastea/regdb.bin, size: 19348
icnss2_qmi: REGDB download request failed, result: 1, err: 1      ✗ 固件拒绝
Sending BDF download message, state: 0x483, type: 0
icnss2_qmi: get_board_id_from_dtb:device project-info node not exist.
File added with prefix: adrastea/bdwlant_cn.bin
Downloading BDF: adrastea/bdwlant_cn.bin, size: 26328
icnss2_qmi: BDF download request failed, result: 1, err: 1        ✗ 固件拒绝
```

### 12.3 已排除的原因

| 假设 | 结论 |
|---|---|
| IRQ 资源表缺失 | ✅ 已修复（`platform_get_irq` 回退）—— probe 成功 |
| smp2p 节点缺失 | ❌ 已排除（补齐后无变化） |
| MSA 太小 | ❌ 已排除（12 MiB，与原厂一致） |
| 握手顺序错 | ❌ 已排除（ind→msa_mem_info→msa_ready→cap 全对） |
| board_id 异常 | ❌ 已排除（0xff，与原厂一致） |
| `seg_id` 未自增 | ❌ 已排除（`qmi.c:1345` 有 `req->seg_id++`） |

### 12.4 剩余怀疑（按可能性）

1. **WLFW TLV 结构版本不匹配**：`wlan_firmware_service_v01.h`（我们构建用的版本）与
   固件期望的 TLV 布局不同 → 固件解析出无效字段，返回通用 `result=1/err=1`。
   验证方法：把 ROM 里 5.10 的 `wlan_firmware_service.ko`（或 vendor 源码同版本头文件）
   与我们的头文件逐字段对比，特别是 `struct wlfw_bdf_download_req_msg_v01`
   的 `data[]` 长度与 `data_len` 类型。
2. **`QMI_WLFW_MAX_DATA_SIZE_V01`（单段大小）大于固件上限**：若我们按 0x1000 分段而固件只收
   0x800，会被拒。可在 `qmi.c` 里把分片大小临时改小试一次。
3. **`bdf_type` 编码差异**：REGDB 用 4、BDF 用 0；若固件该版本只认 0–3，需要调整。
4. **`project-info` 节点缺失**：会让 board_id 走默认路径并选 `_cn` 变体（不是拒绝的直接原因，
   但补齐后更接近原厂）。

### 12.5 下一步（最省力的顺序）

1. 在 `qmi.c` 打印实际发送的 `req->bdf_type / total_size / data_len / seg_id / file_id`，
   确认第一个分段的字段（几行打印即可）；
2. 把分段大小从 `QMI_WLFW_MAX_DATA_SIZE_V01` 临时改为 0x800 试一次；
3. 对比 vendor 的 `wlan_firmware_service_v01.h`（可从 ROM 里的 5.10 `wlan_firmware_service.ko`
   或 sky-lineage 树的 `include/` 找到）与我们的头文件。

## 13. 第八轮：首段字段打印 + 关键线索（小米厂商私有 board_id 逻辑）

### 13.1 打印结果（`BDF seg0`）

```
BDF seg0: type=4 file_id=0xff total=19348 seg=0 file=adrastea/regdb.bin max_data=6144
icnss2_qmi: REGDB download request failed, result: 1, err: 1
BDF seg0: type=0 file_id=0xff total=26328 seg=0 file=adrastea/bdwlant_cn.bin max_data=6144
icnss2_qmi: BDF download request failed, result: 1, err: 1
```

分片大小已从 6144 试改 2048（`ath11k_bdf_chunk`），**仍被拒**；
REGDB(type=4) 与 BDF(type=0) 都被拒 ⇒ 排除 `bdf_type` 与分片大小。

### 13.2 决定性线索：原厂模块里有小米私有 board_id 逻辑

`jp_icnss2.ko`（5.10，与固件同批）里有一批**我们构建版本没有**的字符串：

```
icnss2_qmi: m19_test1,board_id: 0x%x,filename_tmp:%s
icnss2_qmi: m19_test2,board_id: 0x%x,filename_tmp:%s
icnss2_qmi: %s:device board id read success:%s,%s
Board ID: 0x%x
get_board_id_from_dtb
```

⇒ 小米在原厂 icnss2 里加了私有逻辑：**从某个来源读真实 board id，再据此选择 BDF 文件名**
（`m19` 是小米内部代号）。我们这份 mainline 移植版走的是
`get_board_id_from_dtb`（`project-info` 节点不存在）→ 回落 `board_id=0xff`
→ 文件名落到 `bdwlant_cn.bin`。

而固件很可能用 `file_id`（=board_id）做校验，所以 `0xff` 被拒（`result=1/err=1`）。

补充：`project-info` 在 ROM 的 **boot/vendor_boot/dtbo 里都不存在**（只在 `super.img`
里有 3 处字符串命中），说明原厂也走不到该节点 —— 它走的是上面那套私有路径。

### 13.3 下一步（明确）

1. 反汇编 `jp_icnss2.ko` 的 `icnss_get_bdf_file_name` / `get_board_id_from_dtb`
   （符号在，5.10 模块带 kallsyms 重定位，容易读），还原：
   - board id 从哪读（DT 属性？`/vendor/firmware/...` 文件？SMEM？）
   - `m19_test1/m19_test2` 分支对应哪些 board id / 文件名映射
2. 在我们的 `qmi.c` 里照抄该逻辑（或直接把 `priv->board_id` 设成日版真实值，
   并把文件名固定为 `bdwlant_jp.bin`），重编重测。
3. 若固件确实以 `file_id` 校验，这一步应能直接放行 BDF 下载 → 随后 cal_report →
   FW_READY → wlan0。

### 13.4 本轮其他确认

* `/root`、`/tmp` 下推送的模块会因 **sysrq-b 不做 sync** 而丢失/损坏（出现过 0 字节、
  ELF magic 丢失、`insmod rc=8`）⇒ **push 之后必须 `sync`**，并 `md5sum` 校验。
* 镜像侧：`out/sky-shim-boot-v4.img` 内嵌 DTB 已核对为 sky2（64,084 B，
  sha256 `b1b0d1d778f1da020fcf1374…`），设备活体 FDT 一致。

## 14. 原厂 `jp_icnss2.ko` 反汇编结果（board_id / BDF 文件名逻辑）

### 14.1 `get_board_id_from_dtb` @0x109d0

引用字符串（按 reloc 还原）：

```
'project-info'                                     ← of_find_node_by_name
'boardid'                                          ← of_property_read_string
'icnss2_qmi: %s:device board id read success:%s,%s'
'icnss2_qmi: device board id get success: %s.'
'%sicnss2_qmi: %s:device project-info node not exist.'
'%sicnss2_qmi: %s:device board id read fail:ret=%d.'
```

⇒ 原厂逻辑：找 DT 节点 **`project-info`**，读**字符串属性 `boardid`**；
找不到就打印我们见到的那条 `project-info node not exist.`。

**重要推论**：`project-info` 在日版 ROM 的 boot/vendor_boot/dtbo 里**都不存在**
（只有 super.img 有 3 处字符串命中），所以**原厂也走"node not exist"回退路径**。
⇒ **board_id 不是我们被拒的原因**（原厂用同样的回退也能成功下载 BDF）。

### 14.2 `icnss_wlfw_bdf_dnld_send_sync` @0x10e38 的文件名候选

```
regdb.bin
bdwlant_cn.bin        bdwlans_cn.bin       bdwlanu_cn.bin
bdwlan_cn.bin         bdwlan_jp.bin        bdwlan_eu.bin      bdwlan_in.bin
bdwlan.bin            bdwlan.%03x          bdwlan.b%02x       bdwlan.e%02x
bdwlan%02x.e%02x      bdwlan.elf
```

⇒ 原厂支持**大量变体**，`bdwlant_cn.bin` 本身也在候选里，所以**文件名也不是被拒的原因**。

### 14.3 新的头号怀疑：`req->file_id` 的取值来源

我们的首段打印：`file_id=0xff`。而 `0xff` 正好等于
**capability 响应里的 `board_id`（0xff）** —— 说明我们这份移植版很可能把
`priv->board_id` 用 cap 响应的值覆盖了；原厂在 `project-info` 缺失时
`board_id` 保持 **默认值（0，kzalloc）**，也就是它发的 `file_id` 很可能是 **0x0**。

固件对 BDF 下载的 `file_id` 做校验的话，0xff 会被拒（我们正是 `result=1, err=1`）。

**一步验证（下一轮第一件事）**：
```c
/* qmi.c: icnss_wlfw_bdf_dnld_send_sync() 里 */
req->file_id = 0;        /* 或按 DT boardid 解析，缺省 0 */
```
重编 → push（记得 sync）→ insmod → 看 `BDF seg0` 打印里的 `file_id` 与是否还被拒。

### 14.4 其它待验证的次要可疑点

* `total_size` 用 `fw_entry->size`（19348 / 26328）—— 原厂一致；
* `end` 只在最后一段置 1（我们与之一致）；
* 若改 `file_id` 仍被拒，则回到 **WLFW TLV 版本差异**（对比 ROM 的
  `wlan_firmware_service.ko` 的 Ei 表与本项目 `wlan_firmware_service_v01.h`）。

## 15. 第九轮：`file_id = 0` 实验（否定 file_id 假设）+ 两个新事实

### 15.1 实验

`qmi.c`：`req->file_id = priv->board_id;` → `req->file_id = 0;`，重编（808,256 B）后上机。

```text
Sending MSA ready request message, state: 0x481
Received MSA Ready Indication
fw_version: 0x3431fb72, fw_build_timestamp: 2024-09-03 13:50,
            fw_build_id: WLAN.HL.3.4.3-00346-QCAHLSWMTPLZ-1.31602.10      ← ★ 固件版本查询成功
Downloading REGDB: adrastea/regdb.bin, size: 19348
BDF seg0: type=4 file_id=0x0 total=19348 seg=0 file=adrastea/regdb.bin
REGDB download request failed, result: 1, err: 1                        ✗ 仍被拒
m19_test1,board_id: 0xff,filename_tmp:bdwlant_cn.bin
Downloading BDF: adrastea/bdwlant_cn.bin, size: 26328
BDF seg0: type=0 file_id=0x0 total=26328 seg=0 file=adrastea/bdwlant_cn.bin
BDF download request failed, result: 1, err: 1                          ✗ 仍被拒
```

### 15.2 结论

1. **`file_id` 不是原因**（0x0 与 0xff 都被拒）——假设否定。
2. **固件是活的、版本完全对上**：`fw_version 0x3431fb72 / 2024-09-03 13:50 /
   WLAN.HL.3.4.3-00346-QCAHLSWMTPLZ-1.31602.10`，与日版原厂一致。
   说明 `cap_req`、`msa_ready`、`fw_version` 这些 QMI 交互都正常，只有 BDF 下载这条被拒。
3. **我们的源码其实已经包含小米的 `m19_test1` 钩子**（日志里出现
   `m19_test1,board_id: 0xff,filename_tmp:bdwlant_cn.bin`），即当前树就是小米分支的移植，
   不是"缺少厂商逻辑"。

### 15.3 当时的怀疑：WLFW TLV 布局/版本不一致（后续证据见 §17.4）

`wlfw_bdf_download_req_msg_v01` 的 TLV 集合由 `wlan_firmware_service_v01.c` 里的
`wlfw_bdf_download_req_msg_v01_ei[]` 决定。当时怀疑请求 Ei 与固件预期不同；后续仅确认本地响应定义与 ath10k 响应定义不同，请求定义与 ath10k 一致，**不能据此推断固件返回 `result=1/err=1` 的原因**。ROM 中该服务实际上是 `[permanent]` 而非可提取的独立 `.ko`，见 §17。

**下一步（机械可做）**：

1. 从日版 ROM 的 `vendor` 分区解出 `/vendor/lib/modules/wlan_firmware_service.ko`；
2. 用 `nm`/`objdump` 取出它的 `wlfw_bdf_download_req_msg_v01_ei` 表，
   与本项目 `wlan_firmware_service.ko` 里的同名表**逐字节/逐项对比**
   （`struct qmi_elem_info` = {data_type, offset, array_len, elem_size, tlv_type…}）；
3. 若不同 → 用 ROM 的头文件重新生成 `wlan_firmware_service_v01.c`（或按差异手工修正），
   重编 `wlan_firmware_service.ko` + `icnss2.ko` 再测。
4. 若完全相同 → 说明是请求**字段语义**问题（例如 `seg_id`/`end` 的首段取值、
   或 `bdf_type` 映射 4/0 与固件期望不同），届时再逐字段试探。

### 15.4 本轮附带确认

* `fw_version` 查询成功是重要信号：**固件与 QMI 通道健康、只剩 BDF 下载这一个环节**。
* 设备每次重启后需重新 `insmod wlan_firmware_service.ko` + `icnss2.ko`
  （至少目前没有做开机自动加载）。

## 16. 第十轮：Ei/结构体对比结果（请求一致，响应不一致）

### 16.1 `struct wlfw_bdf_download_req_msg_v01`：**两边完全一致**

|---|---|
| 字段 | `valid / file_id_valid / file_id / total_size_valid / total_size / seg_id_valid / seg_id / data_valid / data_len / data[6144] / end_valid / end / bdf_type_valid / bdf_type` |
| 长度常量 | 都是 `WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN 6182` |

⇒ 我们发出的**请求编码是标准形态**，与 mainline ath10k 的 WLFW 定义逐字段相同。

### 16.2 `struct wlfw_bdf_download_resp_msg_v01`：**不一致**

| 我们（vendor 新头） | 内核 ath10k（旧头） |
|---|---|
| `struct qmi_response_type_v01 resp;`<br>`u8 host_bdf_data_valid;`<br>`u64 host_bdf_data;`<br>`MAX_MSG_LEN 18` | `struct qmi_response_type_v01 resp;`<br>`MAX_MSG_LEN 7` |

⇒ **仅能确定本地响应定义与内核 ath10k 定义不同，不能据此确定 2024 固件使用哪一版。**
`host_bdf_data` 是响应的可选 TLV 0x10；响应描述表不参与 BDF 请求编码。本地和 ath10k 的 BDF 请求结构、请求 Ei 表及最大长度一致。Linux 7.3 的 QMI 解码器按实际收到的 TLV 查找 Ei（`qmi_encdec.c:qmi_decode()`）；不存在的可选 TLV 不要求固件提供，未知的可选 TLV 会跳过。`qmi_txn_wait()` 已成功返回且读到了 `resp.result=1/error=1`，说明这不是本地响应解码失败。**响应定义差异本身无法解释固件拒绝 BDF 请求**；其他消息版本差异仍可逐项检查，不能直接从这个字段推断。

### 16.3 结论与下一步（按性价比）

1. **先确认原因**：逐项比较实际使用的 BDF 请求 Ei 表与已验证的基线，并核对设备上的请求字段、请求先决状态与固件返回码；不要只为了缩短响应结构体而替换 `.c/.h`。若取得与固件同批且可验证来源的完整 WLFW 定义，可整体对照所有相关消息；不要仅裁剪 `host_bdf_data` 当作修复。
2. **次优（快速试探，不需要换头文件）**：
   * 先只下 BDF、**跳过 REGDB**（看是否 REGDB 失败污染了状态）；
   * 试 `bdf_type` = 1/2/3；
   * 在 BDF 前**补发 `wlfw_host_cap`（含 ddr_range）**，模仿 WCN6750 路径。
3. 无论 1 还是 2，都建议在 `qmi.c` 里把 `resp->resp.result/error` 与
   **原始响应字节长度**一起打出来，便于判断固件是不是"没解出 TLVs"。

### 16.4 本轮已验证的正面信号（别忘了）

* `fw_version 0x3431fb72 / 2024-09-03 / WLAN.HL.3.4.3-00346-QCAHLSWMTPLZ-1.31602.10`
  —— 固件与 QMI 通道健康；
* `msa_mem_info` / `msa_ready` / `cap_req` 全部成功，capability 与原厂一致；
* 只差 BDF 下载这一步。

## 17. 第十一轮：换头文件 —— 已备份，但**原厂头文件不可得**（源码未公开）

### 17.1 已完成的备份

```
third_party/micode-sky-icnss2/wlan_firmware_service_v01.c.bak-20261104   162,449 B
third_party/micode-sky-icnss2/wlan_firmware_service_v01.h.bak-20261104    40,626 B
```
回滚：`cp *.bak-20261104` 覆盖同名文件后重编即可。

### 17.2 为什么拿不到原厂头文件

1. 原厂 `wlan_firmware_service` 在 stock 上 `lsmod` 显示为 **`[permanent]`** ——
   即**编译进 vendor 内核**，**ROM 文件系统里没有 `wlan_firmware_service.ko` 这个文件**。
   （已从日版 ROM 解出 vendor_a（EROFS，831 MB）并完整展开 1.5 GB 验证：`lib/modules/` 下无该文件。）
2. 公开源码树里也没有该文件：
   | 来源 | 结果 |
   |---|---|
   | `MiCode/Xiaomi_Kernel_OpenSource` `sky-u-oss` / `sky-v-oss` | 分支不存在（404） |
   | 同上 `sky-t-oss`（唯一存在的 sky 分支） | `drivers/soc/qcom/icnss2/` 只有 main/qmi/power/debug/genl，**无 WLFW 文件** |
   | `sky-lineage/android_kernel_xiaomi_sky` `lineage-23.2` | 同上，无 WLFW 文件 |
   | 本地 `android_kernel_xiaomi_sm8450-modules` clone | 只有 `qcom/opensource/wlan/{fw-api,qca-wifi-host-cmn}` |

⇒ **当前可及的公开源码里都没有已确认与 2024 固件同批的 `wlan_firmware_service_v01.{c,h}`。**
现有版本（带 `host_bdf_data`、`host_mlo_chip_info` 等字段）是本地已确认可用来编译的一份。特别注意：`host_bdf_data` 是响应侧可选字段，它与 ath10k 响应定义不同，**不能单凭这一点判定请求编码不兼容或解释固件的 BDF 拒绝**。

### 17.3 三条可行路径（按可行性）

1. **拿 HyperOS 14 / sky 的官方内核源码包**（小米 OSS 站点或其它分支名），
   从中取 `wlan_firmware_service_v01.{c,h}` 覆盖并重编 —— 最干净。
2. **从原厂内核镜像里反向重建 Ei 表**：解压 stock `boot.img` 的内核，定位
   `wlfw_bdf_download_req_msg_v01_ei` 数据（kallsyms 里可拿到名字/地址线索），
   按 `struct qmi_elem_info` 逐项解码，得到固件期望的 TLV 列表，再改我们的 `.c`。
   工作量中等，但不需要源码。
3. **不改头文件**，改为逐字段试探（用户已排除此方向，仅作备选）。

### 17.4 交接纠偏：响应结构差异不是已证实的卡点

- 本地 `wlan_firmware_service_v01.h` 的 BDF 响应多 `host_bdf_data_valid/host_bdf_data`，对应 `.c` 中的可选 TLV 0x10；ath10k 同名响应只有 `resp`，这是一处真实的**响应描述差异**。
- 两边 BDF **请求**均是 `valid/file_id/total_size/seg_id/data/end/bdf_type`，请求描述表的 TLV 0x01、0x10–0x15 和最大长度 6182 相符；响应 Ei 不参与请求编码。固件是否与 ath10k 的响应布局一致，目前没有来自固件同批 WLFW 源码的证据。
- 内核 `drivers/soc/qcom/qmi_encdec.c:qmi_decode()` 查找实际收到的 TLV，对未知的可选 TLV 跳过；`qmi_txn_wait()` 若解码失败会返回负错误码。现有日志读到 `resp.result=1/error=1`，因此不能把这次失败归为本地响应解码错误，也不能仅凭该差异确定固件拒绝请求的原因。
- 当前 `.c/.h` 与 `*.bak-20261104` 校验值完全一致，尚未做版本替换。**不要单独删除响应字段或改 `MAX_MSG_LEN` 当作修复**；先分析 BDF 请求的实际发送条件和固件响应。若拿到确证同批的 WLFW 定义，再整体比对后修改。


## 18. 第十二轮：尝试抓 BDF 请求的编码字节（部分成功，方法需改进）

### 18.1 背景纠正（来自用户/另一会话）

"响应结构里多 `host_bdf_data`" **不能**作为 BDF 下载被拒的原因：
响应描述不参与请求编码，且日志已解出固件返回的 `result=1/error=1`（不是本地解码错误）。
⇒ 已停止"换头文件"方向；**WLFW 的 `.c/.h` 未修改**（备份仍在
`*.bak-20261104`），当前模块仍用原文件构建。

### 18.2 本轮做的事与结果

为了拿到"请求实际编码字节"，在 `qmi.c` 的 BDF 发送处加了一次**离屏编码 + hexdump**
（`qmi_encode_message(QMI_REQUEST, msgid, &len, 0, ei, req)`）。

```text
icnss2_qmi: BDF seg0: type=0 file_id=0x0 total=26328 seg=0 file=adrastea/bdwlant_cn.bin
qmi_encode: Too Small Buffer @data_type:3
icnss2_qmi: BDF enc failed: -525
```

⇒ **离屏编码复现不出真实发送路径**：`qmi_encode_message()` 内部用
`qmi_calc_max_msg_len(ei)` 估算缓冲区，对我们这份 `*_ei[]` 估算不足
（`data_type:3` = 数据 TLV），返回 `-525`。
而真实发送走的是 `qmi_send_request(..., WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN, ei, req)`
（显式给 6182 字节），所以**发送本身是成功的**（固件有回包）。

**结论**：这条路径拿不到真实编码字节；需要换方法（见 §18.4）。

### 18.3 顺带观察到的新日志（state machine 细节）

```text
m19_test0,M19 chip id type: 0x4130
m19_test1,board_id: 0xff,filename_tmp:bdwlant_cn.bin
…BDF download request failed…
HW Power off: 0x483
Regulator vdd-3.3-ch0 / vdd-1.3-rfa / vdd-1.8-xo / vdd-cx-mx 被禁用
Event Processed: SERVER_ARRIVE(0), ret: -1, state: 0x481
```

⇒ 失败后 icnss2 会走 `HW Power off` 并回退状态（0x483 → 0x481），
这也解释了为什么每次失败后需要重启/重新加载。

### 18.4 下一步取证方法（更可靠）

1. **在真实发送路径上抓字节**：临时把 `qmi_send_request()` 的 `len` 参数调大到 7000+
   并改用可控缓冲（或直接用 `qmi_encode_message` + 显式 7000 缓冲，
   绕开 `qmi_calc_max_msg_len` 的估算不足），再 hexdump；
   若成功，就能看到完整 TLV 列表（每个 TLV 的 id/len 都在前面 8 字节内）。
2. **对比 `qmi_calc_max_msg_len(ei)` 与 6182**：如果前者 < 6182，说明我们的 `ei[]`
   里 `data` 字段的 `array_len/elem_size` 与结构体不一致 —— 这本身可能就是
   固件拒绝的根源（TLV 长度字段算错）。
3. 保持 WLFW `.c/.h` 不变，直到有字节级证据。

## 19. 第十三轮：**BDF 下载问题彻底解决，WLAN FW is ready 达成** ✅

### 19.1 真因：USB 链路把固件文件传成了全零

主机 dmesg 抓到的直接证据：

```
usb 1-1: can't set config #1, error -71      ← EPROTO
usb 1-1: USB disconnect, device number 2
```

设备上文件**大小正确但内容全零**：

```
/lib/firmware/adrastea/regdb.bin       0000000000000000…   （应为 a7cc 0400 3700 4146…）
/lib/firmware/adrastea/bdwlant_cn.bin  0000000000000000…   （应为 0100 0404 0000 d866…）
```

⇒ 固件收到的是**空白 BDF**，因此对 REGDB 和 BDF 下载统一返回 `result=1, err=1`。

**换 USB 线 + 换端口 + 每个文件 md5 双向校验后，问题消失。**

### 19.2 修复后的完整链路（与原厂一致）

```text
icnss2: Platform driver probed successfully
fw_version: 0x3431fb72, fw_build_timestamp: 2024-09-03 13:50,
            fw_build_id: WLAN.HL.3.4.3-00346-QCAHLSWMTPLZ-1.31602.10
HW Power on: state: 0x481
Downloading REGDB: adrastea/regdb.bin, size: 19348        → 成功
Downloading BDF:   adrastea/bdwlant_cn.bin, size: 26328    → 成功
smp2p_map_wlan_1_in 注册（force-fatal-error / early-crash-ind）
icnss2_qmi: Sending cal report request, state: 0x483       → 成功
icnss2: WLAN FW is ready: 0x487                            ★ 与原厂同一状态
```

**被彻底排除的因素**（此前全部怀疑过的，都不是原因）：
WLFW 头文件版本差、响应里的 `host_bdf_data`、`file_id` 0xff/0x0、`bdf_type`、
分片大小、`project-info` 节点、`smp2p_map_wlan_2_in`、
IRQ 资源表（这个是**真 bug 但只影响 probe**，已用 `platform_get_irq()` 修复）。

### 19.3 当前唯一卡点：qcacld 的 probe 返回 -1（无日志）

```text
wlan: Loading driver v2.0.9.17N +PANIC_ON_BUG
Registering driver, state: 0x485
Posting/Processing event: REGISTER_DRIVER(3)
HW Power on: state: 0x485   （regulator 全部正常使能）
icnss2: Driver probe failed: -1, state: 0x20487, probe_cnt: 1
HW Power off: 0x487 → regulator 全部关闭
Event Processed: REGISTER_DRIVER(3), ret: -1
```

`wlan.ko` 的 probe 立刻返回 -1，且**qcacld 自己一行日志都没打**（dmesg 里 hdd/qdf/cds 仅 2 条）。

已排除：配置文件缺失 —— 已安装
`/lib/firmware/wlan/qca_cld/adrastea/WCNSS_qcom_cfg.ini`（13,913 B，取自原厂
`/vendor/etc/wifi/adrastea/`，md5 校验通过）；行为无变化。

### 19.4 下一步（需要一次重启，因为 `CONFIG_MODULE_UNLOAD=n`）

设备 `rmmod` 返回 `Function not implemented` ⇒ **每次开机只能 insmod 一次**，
改参数必须重启。建议：

1. 重启后先只加载两个前置模块：
   `insmod /root/wlan_firmware_service.ko; insmod /root/icnss2.ko;`（等 `FW is ready`）
2. 再带日志开关加载主模块：
   `insmod /root/wlan.ko qdf_log_dump_at_kernel_enable=1`
   （原厂 `adrastea` 模块就有这个参数；若我们的构建没有，则改
   `WCNSS_qcom_cfg.ini` 里的日志级别项，如 `qdf_debug_mask` / `loglevel`）
3. 抓 `dmesg | sed -n '/Loading driver/,+80p'`，定位 probe 里返回 -1 的具体位置。

### 19.5 运维经验（这次踩到的坑，务必沿用）

| 现象 | 处理 |
|---|---|
| `reboot` 命令挂住/无效 | 用 `sync; sync; sleep 1; echo b > /proc/sysrq-trigger`（**先 sync**，否则文件会坏） |
| 设备偶发落进 fastboot | `fastboot flash boot_a sky-mainline/shim/out/sky-shim-boot-v4.img`（内嵌 DTB sha `b1b0d1d7…`）后 `fastboot reboot` |
| 传输后文件内容为全零 | **必须** `md5sum` 双向校验；只看大小不可靠 |
| `rmmod` 不可用 | `CONFIG_MODULE_UNLOAD=n`，一次开机只能加载一次 |
| `/tmp` 下模块损坏 | 推送到 `/root` 并 sync + md5 |

### 19.6 复现步骤（当前设备状态可用）

```sh
insmod /root/wlan_firmware_service.ko
insmod /root/icnss2.ko            # → WLAN FW is ready: 0x487
insmod /root/wlan.ko [qdf_log_dump_at_kernel_enable=1]
# 期望：wlan0/wiphy 出现；当前：probe -1（下一步排查）
```

## 20. 第十四轮：instrument `__hdd_soc_probe` → 卡点精确定位到 `hdd_context_create()`

### 20.1 加了 6 处 `pr_err("sky-dbg: …")`（含用户指出的两个分界点）

`core/hdd/src/wlan_hdd_driver_ops.c`（备份 `*.bak-20261104`）：

| 位置 | 打印 |
|---|---|
| `hdd_soc_probe:692` | `osif_psoc_sync_create_and_trans=%d` ← **旧交接遗漏的分界点** |
| `__hdd_soc_probe:604` | `hdd_init_qdf_ctx=%d` |
| `__hdd_soc_probe:608` | `hdd_init_dma_mask=%d` |
| `hdd_context_create` 之后 | `hdd_context_create ok` |
| `dp_prealloc_init` 之后 | `dp_prealloc_init=%d` |
| `hdd_wlan_startup` / `hdd_psoc_create_vdevs` 之后 | 各自 `=%d` |

重编 `make -f port.mk wlan` → `wlan.ko` 62,482,280 B（md5 `474a6c23…`），
push 到 `/root/wlan.ko`（md5 校验一致）后重启加载。

### 20.2 结果：失败点在 `hdd_context_create()`

```text
sky-dbg: osif_psoc_sync_create_and_trans=0     ✓
sky-dbg: hdd_init_qdf_ctx=0                    ✓
sky-dbg: hdd_init_dma_mask=0                   ✓
（没有 "sky-dbg: hdd_context_create ok"）
icnss2: Driver probe failed: -1, state: 0x20487, probe_cnt: 1
```

⇒ `hdd_context_create(dev)` 返回 **ERR_PTR(-EPERM)**（-1），
`__hdd_soc_probe` 走 `assert_fail_count` 分支返回 -1。
（注意：`hdd_context_create` 内部**不是**返回 -1 的固定值，而是它内部某个子步骤失败后
`ret` 被赋成 -1；需要再往下一层插桩。）

### 20.3 `hdd_context_create()` 内部的候选失败点（`wlan_hdd_main.c` ~13600-13695）

按源码顺序：
1. `qdf_delayed_work_create(&hdd_ctx->psoc_idle_timeout_work, …)`
2. `qdf_mem_malloc(sizeof(struct hdd_config))`
3. **`hdd_parse_config_ini()`**（→ `qdf_status_to_os_return(status)`）★ 最可疑：
   我们的 rootfs 不是 Android 布局，qcacld 默认会去 `/vendor/etc/wifi/...` 找
   `WCNSS_qcom_cfg.ini`；我们目前只放到了
   `/lib/firmware/wlan/qca_cld/adrastea/WCNSS_qcom_cfg.ini`（那是固件目录）
4. `hdd_cfg_parse_connection_roaming_cfg()`
5. `hdd_objmgr_create_and_store_psoc()`
6. `hdd_context_init()`
7. `hdd_init_netlink_services()`
8. wiphy 分配/注册相关

### 20.4 下一轮（两条并行，成本都低）

**A. 先试路径假设（不用重编）**：把 ini 复制到 qcacld 默认会找的位置，重启重试：

```sh
mkdir -p /vendor/etc/wifi/adrastea /vendor/etc/wifi /vendor/firmware/wlan/qca_cld/adrastea
cp /lib/firmware/wlan/qca_cld/adrastea/WCNSS_qcom_cfg.ini /vendor/etc/wifi/adrastea/
cp /lib/firmware/wlan/qca_cld/adrastea/WCNSS_qcom_cfg.ini /vendor/etc/wifi/
# 另可试 insmod /root/wlan.ko fwpath=/vendor/etc/wifi
```
（`fwpath` 是 wlan.ko 的模块参数，`hdd_main.c:297` 有 `fwpath_buffer`）

**B. 若 A 无效**：在 `hdd_context_create()` 的 8 个子步骤后各加一行 `pr_err`，
再重编重测（流程同上，一次约 15 分钟）。

### 20.5 设备当前状态

```
内核 #112；FDT b1b0d1d7…；/root/{wlan_firmware_service.ko, icnss2.ko, wlan.ko}
wlan.ko = 62,482,280 B（含 sky-dbg 打印，md5 474a6c23…）
固件与 ini 均 md5 校验通过；FW is ready 可稳定达成
```

## 21. 第十五轮：**qcacld 在 mainline 上跑通，wlan0 扫描成功** 🎉

### 21.1 三个真实 bug（逐个定位并修复）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `icnss2: Fail to get IRQ-0` | ADRASTEA 分支用 `platform_get_resource(IORESOURCE_IRQ)`，而平台设备**无资源表**（`num_resources==0`） | 改用 `platform_get_irq(pdev, i)`（内部回退 DT 解析） |
| 2 | probe 返回 **-1**（无任何日志） | `hdd_context_create()` → `cfg_parse(WLAN_INI_FILE)` 失败（status=16）。`WLAN_INI_FILE` 是相对路径 `wlan/qca_cld/WCNSS_qcom_cfg.ini`，我们把 ini 只放在 `.../qca_cld/adrastea/` 下 | 把 `WCNSS_qcom_cfg.ini` 同时放到 `/lib/firmware/wlan/qca_cld/` 与 `/vendor/firmware/wlan/qca_cld/`（以及 adrastea 子目录） |
| 3 | probe 在 `netif_napi_add_weight_locked` 处 **oops** | `struct qca_napi_info` 内嵌 `struct net_device`（`netdev`/`rx_thread_netdev`）**从未初始化**；7.3 的 `netif_napi_add_weight_locked()` 会 `netif_napi_dev_list_add(dev, napi)` → 操作 `dev->napi_list`（全零）→ NULL 写。`init_dummy_netdev()` **未导出**，不能用 | 在两处 `netif_napi_add*` 之前手工 `INIT_LIST_HEAD(&dev->napi_list); dev->reg_state = NETREG_UNREGISTERED;` |

插桩方法（对后续类似问题可复用）：在
`wlan_hdd_driver_ops.c`（`hdd_soc_probe:692`、`__hdd_soc_probe` 五步 + `hdd_psoc_create_vdevs`）
和 `wlan_hdd_main.c::hdd_context_create()`（8 个子步骤）里各加 `pr_err("sky-dbg[2]: …=%d")`，
每轮重编 `make -f port.mk wlan`（约 10 分钟）→ push（md5 校验）→ 重启 → 加载 → 读 dmesg。

### 21.2 跑通后的实测结果

```
iw dev
  phy#0   Interface wlan0  type managed  addr 00:0a:f5:a0:f6:c7
iw phy phy0 info
  Wiphy phy0, wiphy index 0, max # scan SSIDs: 10, max scan IEs length: 2048
  max # sched scan SSIDs: 16, max # match sets: 16, Retry short/long limit 7/4 …
iw dev wlan0 scan                      ← ★ 成功
  BSS 54:9b:49:6f:bf:b4(on wlan0)
     freq: 2412.0, signal: -83.00 dBm, beacon interval 100 TUs
     SSID: 001aterm-6aa40b-g
     Supported rates / DS Parameter set / ERP / HT capabilities(0x19ef, HT20/HT40) …
```

⇒ **icnss2 + qcacld-3.0 + adrastea 固件在 mainline 7.3 上完成 probe、创建 wlan0、扫描成功。**
这是本项目从 ath11k 转向 qcacld 路线后的**第一个功能里程碑**。

### 21.3 环境补齐（本轮做的）

* `iw` 未预装（Alpine 3.21 rootfs）→ 从主机下载 `iw-6.9-r0.apk` + `libnl3-3.11.0-r0.apk`
  推入设备后 `apk add --allow-untrusted --no-network` 安装成功。
* `WCNSS_qcom_cfg.ini` 复制到 5 个候选路径（见 21.1 #2）。
* `/data/vendor/wifi{,/wlan,/wlan_logs,/pktlog}`、`/data/misc/wifi/sockets` 已建。

### 21.4 下一步（功能验证）

1. **连接测试**：装 `wpa_supplicant`（Alpine 包）+ `wpa_cli`，配置 WPA2 PSK 连接；
   `udhcpc -i wlan0` 拿地址；`ping` 验证。
2. **检查项**（TEST-CHECKLIST 里的 cfg80211 重点）：ROC cookie、mgmt TX status、
   change_beacon、control-port、key 安装/重连/删除。
3. **SSR 恢复**：受控触发 WPSS 重启，验证 QMI client 重建 + `ICNSS_FW_READY` 恢复 + wlan 重新可用。
4. 稳定性：反复扫描 / 长时间连接，确认无 oops / hung task。

### 21.5 复现命令（当前设备状态）

```sh
insmod /root/wlan_firmware_service.ko
insmod /root/icnss2.ko            # 等 8s，期望 "WLAN FW is ready: 0x487"
insmod /root/wlan.ko              # 期望 wlan0 出现
ip link set wlan0 up
iw dev wlan0 scan                 # 期望列出周围 AP
```

## 22. 第十六轮：修掉 NAPI teardown 的 3 条 WARNING ✅（并定位新的断连卡死）

### 22.1 修复内容（`hif_napi.c`）

7.3 的 NAPI 状态语义与老内核相反：
* `napi_enable_locked()`  **清除** `NAPI_STATE_SCHED|NPSVC`
* `napi_disable_locked()` **置位** `NAPI_STATE_SCHED|NPSVC`
* `__netif_napi_del_locked()`：`WARN_ON(!test_bit(NAPI_STATE_SCHED))`
  —— 即「删除时若 NAPI 仍处于 enabled 状态就告警」

qcacld 里 `hif_init_rx_thread_napi()` 调了 `napi_enable()`，
但 `hif_deinit_rx_thread_napi()` **只 `netif_napi_del()`**，于是每个 CE 一条 WARNING（3 个 = 3 条）。

补丁：

```c
static void hif_deinit_rx_thread_napi(struct qca_napi_info *napii)
{
	/* NAPI_STATE_SCHED 置位 = 已 disable；napi_disable() 会自旋等待 SCHED/NPSVC 清零，
	 * 因此只在 enabled 时才调用 */
	if (!test_bit(NAPI_STATE_SCHED, &napii->rx_thread_napi.state))
		napi_disable(&napii->rx_thread_napi);
	netif_napi_del(&napii->rx_thread_napi);
}
```

**验证**：新 `wlan.ko`（md5 `026b01f6775118389346cbe747582789`，62,485,096 B）经
`rc-service sky-wifi start` 加载 → 连接成功（`wpa_state=COMPLETED`, `192.168.1.186`）→
经完整 teardown（含 SSR 崩溃路径）后 `dmesg | grep -c netif_napi_del_locked` = **0**（此前 3）。

### 22.2 新发现：断连卡死 → 强制 FW assert → SSR 恢复失败

触发：`ip link set wlan0 down`（等于用户态 disconnect）

```text
[184] osif_cm_disconnect_sync: Received Disconnect reason:3 REASON_DEAUTH_NETWORK_LEAVING
[184] Deauth TX: vdev 0 seq_num 2051 reason 3 waitForAck 1 to 78:8c:b5:82:fd:88
[184] ol_txrx_peer_detach … peer_unmap_timer
[196] scheduler_watchdog_notify: Callback cm_handle_disconnect_resp exceeded its allotted time of 10s
[196] scheduler_watchdog_timeout: Triggering self recovery
[196] cds_trigger_recovery_handler: critical host timeout trigger fw recovery for reason code 31
[196] cds_force_assert_target_via_pld / icnss2: Received early crash indication from FW
[198] remoteproc1: crash detected in wpss: type watchdog → recovering wpss
[198] icnss2: WPSS went down, crashed: 1 → PD_SERVICE_DOWN → SERVER_EXIT
[203] qcom_q6v5_pas: start timed out / remoteproc1: can't start rproc wpss: -110
[207] cm_disconnect_start_req_sync: Disconnect timeout with status 15
[259] icnss2: Timeout waiting for FW Ready 0x1368a
```

### 22.3 代码路径（已确认）

`cm_handle_disconnect_resp` 只有两个来源（`lim_send_sme_rsp_messages.c`）：
1. `lim_cm_send_disconnect_rsp()` ← `lim_process_sme_req_messages.c:4257`（**仅无 session 的快速路径**）
2. 同文件 `:836`，即 `eWNI_SME_DISCONNECT_DONE_IND` 的处理里

正常路径必须等 **deauth 帧 TX 完成（收到 ACK）** 才会产生 `DISCONNECT_DONE_IND`。
日志里 `waitForAck 1` 之后**没有任何 TX completion/ACK 记录** ⇒ 推断：
**管理帧 TX 完成事件（HTT tx_complete）没有被上报/处理**，导致断连永远等不到完成，
10s 后被 scheduler watchdog 判定超时。

⇒ 这与"下一轮要查的 qcacld RX 问题"高度相关：需要确认
`HTT_TX_COMPL_IND` / mgmt tx completion 是否正常（`hdd_tx_completion`、`ol_tx_completion_handler`、
`lim_process_mgmt_tx_complete` 之类），以及 RX ring 是否丢弃了 firmware 的 completion 事件。

### 22.4 附带观察

* `pktlog_send_per_pkt_stats_to_user: Send Failed -3 drop_count = 2/3` —— pktlog 未订阅时的正常拒绝，可忽略
* `wlan_hdd_pld_uevent: Received firmware hang event` / `FW crashed, state: 0x1348b` —— SSR 流程被正确触发，但**恢复失败**（`rproc start timed out -110`），说明 SSR 重启 WPSS 的路径还有问题（下一轮一并查）
