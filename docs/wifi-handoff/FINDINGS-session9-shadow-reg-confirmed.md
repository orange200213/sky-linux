# Session 9: SR_APSS 影子槽机制被**证实**（k95/k96）

## 1. 实验

* k95（内核 #94）：`ath11k_qmi_wlanfw_wlan_cfg_send()` 改为发送 **`shadow_reg`(v1)**
  的 12 条表（按原厂 `target_shadow_reg_cfg_map` 顺序：src CE 0/3/4/5/7 off `0x3C`，
  dst CE 1/2/7/8/9/10/11 off `0x40`），镜像
  `wifi-backup/out/sky-shim-boot-v4-wifi-k95-snoc-shadow-reg.img`。
* k96（内核 #95）：把表做成运行时可调（免刷机二分）：`snoc_shadow_reg_mode`
  （0 全表 / 1 只 src / 2 只 dst / 3 单条 / 4 逆序 / 5 补齐 24）、
  `snoc_shadow_off_src`、`snoc_shadow_off_dst`。
  镜像 `…-k96-shadow-reg-tunable.img`。

## 2. 结果（关键）

### 2.1 12 条表 → 固件断言

```text
ath11k: snoc: wlan_cfg shadow_reg v1 mode 0, 12 entries, off 0x3c/0x40
qcom_q6v5_pas 8a00000.remoteproc: fatal error received:
    cmnos_thread.c:4003:Asserted in platform_helium_init.c:platform_config_shadow_reg:241
```
⇒ 固件里**确实有 `platform_config_shadow_reg()`**（`platform_helium_init.c`），
它处理我们的表并拒绝了 12 条的组成（不是格式问题，见下）。

### 2.2 单条表（`mode=3`，{CE0, 0x3C}）→ 被接受，槽位真的被配置

```text
ath11k: snoc: wlan_cfg shadow_reg v1 mode 3, 1 entries, off 0x3c/0x40     ← 无断言
之后用 skyprobe 读 SR_APSS：
0x228300e0 = 0x1        ← ADDRESS_VALID = 1     （此前所有测量都是 0！）
0x22830020 = 0x9000f    ← SHADOW_ADDRESS0 = 0x9000F
```

`0x9000F` 是**字地址**（原厂 `SHADOW_ADDRESS(i) << 2`），换算字节地址
= `0x9000F << 2 = 0x24003C` = **CE0 基址 `0x240000` + `SR_WR_INDEX 0x3C`**。

⇒ **机制 100% 坐实**：
1. 主机通过 `WLAN_CFG.shadow_reg`(v1) 告诉固件"我要访问哪些 CE 寄存器"；
2. 固件把地址（字地址，指向 chip 偏移 0x24003C 这种位置）写进 `SHADOW_ADDRESS[n]`，
   并置 `ADDRESS_VALID` 的对应位；
3. 之后主机只写 **`SHADOW_VALUE[n]`（0x22832000+4n，实测可写）** 就能完成寄存器写，
   **完全不需要 APSS 直接访问 0x24xxxx**。

这同时解释了 session 3–6 的所有谜团：membase 里只有 SR_APSS 可读是**设计**，不是缺使能；
APSS 的寄存器通路本来就是"SR_APSS 影子槽 + 表由固件配置"。

### 2.3 仍然 abort 的地方（预期）

单条表只映射了 slot 0（CE0 写指针），CE/DP 的其它寄存器仍不在表内，
所以 `ath11k_ce_init_ring()`/`ath11k_dp_srng_setup()` 之后照样 abort（1 次）：

```
ath11k_ahb_window_write32_snoc → ath11k_hal_srng_setup → ath11k_dp_srng_setup
  → ath11k_dp_alloc → ath11k_core_qmi_firmware_ready
```

## 3. 结论：这条路线的**架构上限**已经清楚

* 主机能碰的寄存器 = SR_APSS 块（常驻） + **最多 24 个影子槽**（映射到固件允许的
  若干 CE 寄存器，形如 `0x240000 + ce*0x400 + off`）。
* **UMAC（WBM/REO/TCL，chip 0xa00000+）不在可达范围**：既没有 MMIO 解码，
  也不可能塞进 24 个 CE 影子槽。
* 原厂 adrastea 驱动**根本不编程 DP ring**（session 8 已证），数据面由固件自管。
  ⇒ mainline ath11k 的 `ath11k_dp_alloc()/ath11k_dp_srng_setup()` 在本平台
  **无法通过"找对地址"解决**，必须整体替换数据面模型（CE+HTC/WMI/HTT-only），
  这是架构级改造，不是移植工作量。

## 4. 下一步可选项

1. **继续摸清影子表约束**（k96 已把表做成参数，免刷机）：
   `snoc_shadow_reg_mode=1`（5 条 src）/ `2`（7 条 dst）/ 自定义条数，
   二分找到固件接受的最大集合与 `platform_config_shadow_reg:241` 的断言条件
   （可能是"同一 CE 只能一条"、"offset 必须单调"或"最多 8 条"）。
2. **验证影子写入真的落到 CE 寄存器**：对已映射的 slot 0 写 `SHADOW_VALUE0`，
   观察 `2.2` 里 `SR_INTERRUPT_STATUS.DIRTY_BIT_SET`/`INVALID_ADDR_ACCESS` 的正确语义，
   再回到 mainline 的 `ce.c`：把 `CE_SRC_RING_WRITE_IDX_SET` 改成写 `SHADOW_VALUE[slot]`。
3. **评估 CE-only 改造**：只把 CE ring base/size/hp/tp 这些寄存器也加进影子表
   （若固件允许），让 HTC/WMI/HTT 能跑起来；DP 则放弃或用固件自管方案。
4. 若 3 走不通 ⇒ 如实记录 "mainline ath11k 在此 SoC 不可行"，
   转 downstream（qcacld-3.0 adrastea，见仓库根 `qcacld3-qca6750-sm4450-sky-report.md`）。

## 5. 工具状态

* `snoc_shadow_reg_v1`（默认 **true**）、`snoc_shadow_reg_mode`、`snoc_shadow_off_src`、
  `snoc_shadow_off_dst` 都是 `ath11k` 模块参数，可在 `/etc/sky-wifi-params` 里逐次试验。
* ⚠️ 注意：rootfs 由 `backup/pre-stock-2026-11-02/rootfs-skyroot-2026-11-02.tar.gz`
  还原，而该 tar 是**加"一次性参数机制"之前**打的包，所以 `/etc/sky-wifi-params.try`
  在当前 rootfs 上**不生效**（脚本里没有那段）。要么直接改 `/etc/sky-wifi-params`
  （记得改回来），要么重新打包 rootfs。

---

## 6. 继续 k96：影子表的条数约束 + 一个重要怀疑

### 6.1 条数二分（一次开机一次试验，参数免刷机）

| `snoc_shadow_reg_mode` | 内容 | 结果 |
|---|---|---|
| 3 | 1 条 `{CE0, 0x3C}` | ✅ 接受，`ADDRESS_VALID=0x1`、`SHADOW_ADDRESS0=0x9000F`(=0x24003C) |
| 1 | 5 条（src CE 0/3/4/5/7 @0x3C） | ❌ **断言** `platform_config_shadow_reg:241` |
| 0 | 12 条（5 src + 7 dst） | ❌ **断言** |

⇒ `platform_config_shadow_reg()` 对**多于 1 条**的表就断言（至少在我们当前的固件上）。
下一步若要继续二分，需要 k97 增加 `snoc_shadow_reg_n`（发前 N 条）把 2/3/4 条试出来。

### 6.2 强烈怀疑：这是**固件版本**问题，而不是表内容问题

| | 原厂日版 HyperOS 14 | 我们移植用的 rootfs |
|---|---|---|
| wpss/ath11k 固件 build | **2024-09-03 13:50**，`fw_version 0x3431fb72`（dmesg/logcat） | **2023-06-23 08:24**，`fw_version 0x343b815a`（我们自己的 dmesg） |
| 12 条 shadow_reg 表 | ✅ 原厂正常工作 | ❌ `platform_config_shadow_reg` 断言 |

两个固件差 ~14 个月、版本号都是 `0x343?8???` 系列但不同分支。**极可能新固件才修好了
多槽影子配置**，旧固件在 >1 条时就命中断言。

因此**最高优先级实验**（比继续猜表内容更值）：
1. 从日版 ROM 的 `super.img` 里取出 `vendor` 分区（extent 4，790 MiB @ raw
   `0x114e00000`，EROFS），拿到 `/vendor/firmware/adrastea/wpss.*`；
2. 覆盖到我们 rootfs 的 `/lib/firmware/qcom/sm4450/xiaomi-sky/`（保留旧文件备份）；
3. 用 12 条表（`snoc_shadow_reg_mode=0` + `snoc_shadow_reg_v1=y`）重跑，
   观察是否还断言。若通过 → `ADDRESS_VALID` 应出现多个位，CE 影子写入即可实现。

### 6.3 本轮另外的产出

* 一次性参数机制已重新加进设备上的 `/usr/local/sbin/sky-wifi-up.sh`
  （直接从备份 tar 还原会丢掉它——见 §5 的 ⚠️），并单独存档为
  `backup/pre-stock-2026-11-02/sky-wifi-up.sh.oneshot`：
  **恢复 rootfs 后覆盖这个文件即可**（`cp … /usr/local/sbin/sky-wifi-up.sh && chmod +x`），
  不必重新打包整个 rootfs。

---

## 7. 换固件（未完成：定位到一半）

目标：用日版 ROM 的 WPSS/WLAN 固件替换我们 rootfs 里的旧固件（2023-06-23 vs 原厂 2024-09-03）。

### 7.1 已确认的事实

1. **日版 `super.img` 的 LP 布局与欧版不同**（必须用它自己的元数据解析）：

   ```
   odm_a         ext 0  off 0x01000000   17 MiB
   product_a     ext 1  off 0x01300000 2638 MiB
   system_a      ext 2  off 0xa6100000  730 MiB
   system_ext_a  ext 3  off 0xd3c00000  517 MiB
   vendor_a      ext 4  off 0xf4200000  793 MiB   ← EROFS（已验证）
   vendor_dlkm_a ext 5  off 0x125b00000  32 MiB
   mi_ext_a      ext 6  off 0x127c00000 843 MiB
   ```

2. 用按需 seek 的 sparse 读取器取出了 `vendor_a`（831 MB），`dump.erofs` 确认 EROFS 正常；
   注意：**CRC chunk（`0xCAC4`）必须在文件里跳过 `total_sz-12` 字节**，否则后续 chunk 错位
   （本轮踩过一次，表现为整块数据不是任何已知文件系统）。

3. **`/vendor/firmware/adrastea/` 在 vendor 里不存在**
   （`dump.erofs --ls --path=/firmware/adrastea` → "read inode failed"）。
   vendor 里只有 `/firmware/wlan/qca_cld/adrastea/`（两个符号链接：cfg ini 和 wlan_mac.bin）
   和 `/etc/wifi/adrastea/`。⇒ **WPSS 固件不在 vendor 分区**。

4. 日版 `vendor_boot.img`（header v4）的 vendor ramdisk：
   - 位置 = 第 1 页之后（offset `0x1000`），长度 16 MiB（头里的 `vendor_ramdisk_size`）；
   - 内容 = **LZ4 legacy frame**（magic `02 21 4c 18`），用 `lz4 -d` 解出 35 MB；
   - 解出后是 **cpio**（offset 0 有 `070701`），`cpio -idm '*wpss*'` **匹配不到任何条目**，
     但原始数据里 "wpss" 出现 62 次 ⇒ 名字在别处（很可能在某个清单/嵌套容器里）。
   ⇒ 需要换一种方式拆（例如先列出 cpio 全部条目名，或找嵌套镜像）。

5. 我们现在用的固件：`/lib/firmware/qcom/sm4450/xiaomi-sky/wpss.mdt` 大小 **4684 B**
   （md5 `ffd92b07…`，日期 2023-12-12）；原厂 dmesg 里 `Booting fw image adrastea/wpss.mdt,
   size 4684` —— **大小一致**，所以差异在 `.b*` 段（也就是 WLAN 固件本体），
   这解释了 `fw_version 0x343b815a`(旧) vs `0x3431fb72`(新)。

### 7.2 下一步（定位固件的正确去处）

按优先级：
1. **`NON-HLOS.bin`**（两份 ROM 里都有，266 MB）——Qualcomm 设备上 WLAN/Modem 固件常在这个
   "modem image" 容器里；`strings NON-HLOS.bin | grep -i wpss` 先探一下，再按 FAT/ext4 解包。
2. `mi_ext_a`（843 MiB，小米自有分区）里是否带 `firmware/`。
3. 把 vendor_boot ramdisk 的 cpio **完整列名**（`cpio -it < vb_rd.raw`），看 wpss 是否以
   别的名字/路径存在（或嵌在某个 .img 里）。
4. 最快但需要在原厂系统里做：`adb shell su -c 'find / -name "wpss.*" 2>/dev/null'`
   ——原厂系统当前没跑（我们已切到主线 Linux），需要时再刷回去查一次即可，路径会一目了然。

---

## 8. WPSS/WLAN 固件已换新（2024-09 版），**断言与固件版本无关**

### 8.1 固件找到了：`NON-HLOS.bin`

日版 ROM 的 `NON-HLOS.bin`（280 MB）是 **FAT** 文件系统，里面就有：

```
image/adrastea/wpss.mdt   (4684 B)
image/adrastea/wpss.b00 .. wpss.b10   (b06 4.7 MB 最大)
image/adrastea/bdwlan.102/104/105... (+ .txt)
```

提取（`7z x NON-HLOS.bin 'image/adrastea/wpss.*'`）后与 rootfs 现有固件对比：

| 文件 | 大小 | JP ROM md5 | 我们原来的 md5 |
|---|---|---|---|
| wpss.b02 | 461433 | `93fef43e…` | `0462848f…` |
| wpss.b06 | 4715708 | `baf60a06…` | `254ad3b8…` |
| wpss.b10 | 4280 | `63d1098a…` | `d25b1441…` |
| wpss.mdt | 4684 | `ccf4f902…` | `ffd92b07…` |

⇒ **同尺寸、不同 build**（这就是 `fw_version 0x343b815a`(旧) → `0x3431fb72`(新) 的来源）。

### 8.2 换固件 + 12 条表重测

已备份旧固件到设备 `/root/wpss-old/`（12 个文件），把 JP 的 11 个文件覆盖到
`/lib/firmware/qcom/sm4450/xiaomi-sky/`，并 `wpss.mbn` 同步为新的 mdt。

重启后：

```text
fw_version 0x3431fb72            ← 与原厂完全一致（换固件成功）
snoc: wlan_cfg shadow_reg v1 mode 0, 12 entries, off 0x3c/0x40
remoteproc: Asserted in platform_helium_init.c:platform_config_shadow_reg:241   ← 仍然断言
failed to wait wlan config request: -110                                        ← 10 s 超时
```

⇒ **断言与固件版本无关**。同一份固件在原厂驱动下 12 条表是正常工作的，
所以差异在我们**发出去的 WLAN_CFG 内容**（或与之同包的 `tgt_cfg`/`svc_cfg`）。

### 8.3 下一步排查方向（按可能性）

1. **`tgt_cfg`/`svc_cfg` 与影子表的交叉校验**：固件可能要求影子表里引用的 CE 号
   必须在同一请求的 `tgt_cfg` 里出现过。我们 SNOC 走的是 wcn6750 的 CE pipe 配置
   （`ath11k_ce_get_shadow_config()`/`host_ce_config`），CE 数量/编号可能与 adrastea 的
   12 条表不匹配 ⇒ 先用 `snoc_shadow_reg_mode=1`（只 CE 0/3/4/5/7）配合
   `tgt_cfg_len` 打印确认，再对齐 qcacld 的 `target_lowdesc_ce_config_wlan_adrastea`。
2. **条数边界**：需要 k97 加 `snoc_shadow_reg_n`（只发前 N 条），把 2/3/4 条试出来，
   找出 `platform_config_shadow_reg:241` 的确切断言条件（可能与某字段上限有关）。
3. **换 v2 字段试一次**（`shadow_reg_v2`，绝对地址形式）：虽然源码显示 adrastea 走 v1，
   但这是最低成本的对照实验。
4. **正面取证**：回到原厂系统，用 kprobe 抓 `qmi_send_request` 的**数据缓冲**
   （`%x6` = c_struct 指针），直接看原厂 WLAN_CFG 里 `shadow_reg` 的实际内容
   （条数、id、offset），一次就能对齐。

### 8.4 注意

新固件保留在设备上（`/root/wpss-old/` 是旧版备份）。若要回退：
`cp /root/wpss-old/wpss.* /lib/firmware/qcom/sm4450/xiaomi-sky/ && cp wpss.mdt wpss.mbn`。

---

## 9. tgt_cfg/svc_cfg 对齐核查（任务 1）

### 9.1 发现一个真实缺陷：我们的 CE 管配置用错了表

| | 原厂 adrastea | 我们（mainline wcn6750 hw_params） |
|---|---|---|
| CE 数 | **12**（CE0…CE11） | **9**（`ce_count = 9`） |
| tgt_cfg 条数 | 12（`target_ce_config_wlan_adrastea[]`） | 9（借用 **qca6390** 的 `ath11k_target_ce_config_wlan_qca6390`） |
| svc→ce map | `…_adrastea` | `…_qca6390`，14 条 |

原厂表（`qca-wifi-host-cmn/hif/src/ce/ce_assignment.h`）：
- `host_ce_config_wlan_adrastea[12]`（line 926）：
  CE0 16×2048，CE1 0×2048/512，CE2 0×2048/128，CE3 32×2048，CE4 256×256(DIS_INTR)，
  CE5 256×512(DIS_INTR)，CE6/7/8 全 0，CE9/CE10 0×2048/512，CE11 0×2048/512
- `target_ce_config_wlan_adrastea[12]`（line 988）：
  CE0 OUT 32/2048，CE1 IN 32/2048，CE2 IN 64/2048，CE3 OUT 32/2048，
  CE4 OUT 256/256(DIS_INTR)，CE5 OUT 1024/64(DIS_INTR)，CE6 INOUT 32/16384，
  CE7 INOUT_H2H 0/0(DIS_INTR)，CE8 IN 32/2048，CE9 IN 32/2048，
  CE10 IN 32/2048，CE11 IN 32/2048(DIS_INTR)

⇒ **这是必须修的**（否则 CE 管编号/属性都不对），但**它不能解释断言**（见 9.2）。

### 9.2 断言与 CE 管缺失无关（已证伪）

`mode 1` 的影子表只引用 **CE 0/3/4/5/7**（全部 < 9，在我们现有 9 管 tgt_cfg 里都存在），
**仍然断言**。所以"影子表引用了 tgt_cfg 里不存在的 CE"这个假设**不成立**。

### 9.3 新假设（最贴合数据）：固件要求影子表是**稠密/按序**的

现有证据：

| 表 | 结果 |
|---|---|
| `[{id:0, off:0x3C}]` | ✅ 接受（槽 0 = CE0 写指针） |
| `[{0,0x3C},{3,0x3C},{4,0x3C},{5,0x3C},{7,0x3C}]` | ❌ 断言 |
| 12 条（5 src + 7 dst，id 顺序 0,3,4,5,7,1,2,7,8,9,10,11） | ❌ 断言 |

单条时 `id` 恰好等于数组下标 0；多条时 `id` 与下标错开（第 2 条 id=3）。
固件很可能用 `for (i = 0; i < len; i++) if (cfg[i].id != i) assert;` 之类的校验，
或者要求"id 必须从 0 开始稠密递增"。

**k97 的判别实验**（一次刷机可覆盖）：
1. 新增 `snoc_shadow_reg_dense=1` → 发 `[{0,0x3C},{1,0x3C}]`（稠密 id、同 offset）；
2. `snoc_shadow_reg_n=N` → 只发前 N 条（测 2/3/4 条边界）；
3. 若稠密表被接受 → 说明固件按"slot 下标 == CE 号"建表，
   那么正确的表应该是**按 CE 号 0..11 稠密排列的写指针 offset 表**
   （src CE 用 0x3C、dst CE 用 0x40，其余 CE 用 0 或省略）；
4. 同时把 §9.1 的 12 管 CE 表补进 `hw.c`（`ce_count=12`、`target_ce_count=12`、
   `host_ce_config`/`target_ce_config` 换成 adrastea 版本），
   这是后续 CE/HTC 正确性的前提，与断言排查相互独立。

---

## 10. k97：影子表**约束被定位**（有可测量的诊断信号）

k97 新增 `snoc_shadow_reg_dense`（稠密 id）与 `snoc_shadow_reg_n`（只发前 N 条）。
镜像 `wifi-backup/out/sky-shim-boot-v4-wifi-k97-shadow-dense.img`（md5 `30cbf7d0…`，内核 #97 系列）。

### 10.1 实验结果（新固件 0x3431fb72）

| 表 | 结果 | `ADDRESS_VALID` |
|---|---|---|
| `[{0,0x3C}]` | ✅ | `0x1` |
| `[{0,0x3C},{1,0x3C}]`（稠密 id，CE1 是 IN 方向却给 0x3C） | ❌ 断言 | – |
| dst-only 7 条（CE 1/2/7/8/9/10/11 @0x40） | ❌ 断言 | `0x86000`（bit13/14/19，配置了一部分后断言） |
| `[{0,0x3C},{3,0x3C}]`（**同方向、都合法**） | ✅ **通过** | **`0x9` = bit0+bit3** ✅ |
| `[{0,0x3C},{3,0x3C},{4,0x3C},{5,0x3C}]` | ❌ 断言 | `0x19` = bit0+3+4 ⇒ **在第 4 条 CE5 上断言** |

### 10.2 结论

1. **机制完全工作**：多条影子项可以同时配置，`SHADOW_ADDRESS0=0x9000F`(=0x24003C) 正确、
   `ADDRESS_VALID` 是**24 位槽位图**，且 **bit 号 == CE 号**（我们发 `{0},{3}` → bit0、bit3）。
2. **断言是逐条校验，会在「不合法的第 k 条」上触发**，且触发前已配置的槽位会留下
   `ADDRESS_VALID` 残留位 —— 这给我们一个**精确诊断器**：
   `ADDRESS_VALID` 的位告诉我们哪些 CE 被接受，最后一个未置位的条目就是触发点。
3. 目前**被接受**：CE0、CE3、CE4（src/@0x3C）；**被拒**：CE1(用 0x3C)、CE5、CE7。
4. 这强烈指向原假设（我上一轮"证伪"得太早）：**影子项合法性取决于同一个 WLAN_CFG 包里
   `tgt_cfg` 对该 CE 的定义**。CE5/CE7 在我们借用的 **qca6390 9 管表**里属性与原厂
   adrastea 12 管表不同（原厂 CE5=OUT/DIS_INTR，CE7=INOUT_H2H/DIS_INTR）。

### 10.3 下一步（k98，已明确）

把 `hw.c`/`hw.h` 里 wcn6750 的 CE 配置换成**原厂 adrastea 版本**（`ce_assignment.h`）：

* `host_ce_config_wlan_adrastea[12]`（line 926）
* `target_ce_config_wlan_adrastea[12]`（line 988）
* `ce_count = 12`、`target_ce_count = 12`（现在借用 qca6390 的 9）

成功判据（无需猜）：
```
dmesg 里不再出现 platform_helium 断言，
skyprobe rd 0x228300e0 → ADDRESS_VALID 至少包含 bit0,3,4,5,7（src 5 槽全配置）
```
若 CE5/CE7 仍被拒，则再对比 `svc_to_ce_map`（原厂 `target_service_to_ce_map_wlan_adrastea`）
与 `tgt_cfg` 的 pipe 方向字段，逐条对齐。

---

## 11. k98：**影子表打通了**（CE 配置对齐是正确答案）

k98 把 wcn6750 的 CE 配置换成原厂 adrastea 版本后，12 条影子表**完全被固件接受**。

### 11.1 改动

* `ce.c` / `ce.h`：新增 `ath11k_host_ce_config_adrastea[12]`
  （flags/src_nentries/src_sz_max/dest_nentries 取自原厂 `ce_assignment.h:926`；
  CE1/2 用 `ath11k_htc_rx_completion_handler`，CE9/10/11 用 `ath11k_dp_htt_htc_t2h_msg_handler`）。
* `hw.c` / `core.h`：新增 `ath11k_target_ce_config_wlan_adrastea[12]`
  （CE0 OUT 32/2048，CE1 IN 32/2048，CE2 IN 64/2048，CE3 OUT 32/2048，
  CE4 OUT 256/256 DIS_INTR，CE5 OUT 1024/64 DIS_INTR，CE6 INOUT 32/16384，
  CE7 INOUT_H2H 0/0 DIS_INTR，CE8 IN 32/2048，CE9/CE10 IN 32/2048，CE11 IN 32/2048 DIS_INTR）。
* `core.c`：wcn6750 hw_params 的 `.host_ce_config/.target_ce_config/.ce_count=12/.target_ce_count=12`
  切到 adrastea 版本（原来借用 qca6390 的 9 管表）。
* 注意 mainline 宏名差异：`CE_ATTR_DIS_INTR`（不是 `CE_ATTR_DISABLE_INTR`）、
  `CE_HTT_H2T_MSG_SRC_NENTRIES` 未定义 → 用字面量 `256`。

镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k98-adrastea-ce.img`（md5 `0e53a0b1…`）。

### 11.2 结果（决定性）

```text
ath11k: snoc: wlan_cfg shadow_reg v1 mode 0 dense 0, 12 entries, off 0x3c/0x40
（无 platform_helium 断言 —— WLAN_CFG 被完整接受）
... 流程继续到 CE init / DP ...
ath11k_ahb_window_write32_snoc → ath11k_hal_srng_setup → ath11k_dp_srng_setup   ← 已知的 DP 问题

skyprobe rd 0x228300e0 → ADDRESS_VALID = 0xf860b9
  解码：bit {0,3,4,5,7} + {13,14} + {19} + {20,21,22,23}
  = 12 个槽，与原厂映射表**完全一致**
    src CE 0/3/4/5/7 → slot 0/3/4/5/7
    dst CE 1/2     → slot 13/14
    dst CE 7/8/9/10/11 → slot 19/20/21/22/23
    （原厂还有 dst CE5 → slot 17，我们的表没列，所以缺 bit17）
```

⇒ **SR_APSS 影子通道配置链路已完全打通**：
`WLAN_CFG.shadow_reg`(v1) + 正确的 12 管 CE 配置 ⇒ 固件配置全部 12 个槽位，
主机可用 `SHADOW_VALUE[n]` 写这些 CE 寄存器。这也证明 k97 的"CE5/CE7 被拒"
确实是 `tgt_cfg` 定义不一致造成的。

### 11.3 下一步

1. **用影子写入驱动 CE**：把 mainline `ce.c` 里 `CE_SRC_RING_WRITE_IDX_SET` /
   `CE_DEST_RING_WRITE_IDX_SET` 改成写 `SHADOW_VALUE[slot]`（slot 号见上表），
   并验证 `SHADOW_VALUE` 写入后固件侧寄存器真的更新（读 `DIRTY`/`INVALID_ADDR_ACCESS` 语义）。
2. CE 的 ring base/size 等其它寄存器仍不在影子表里 → 需要确认固件是否已预配置，
   或这些寄存器是否存在别的通路（否则 HTC 无法启动）。
3. **DP（WBM/REO/TCL）依然不可达**：原厂 adrastea 驱动不编程 DP ring，
   数据面由固件自管。mainline ath11k 的 `ath11k_dp_srng_setup()` 必须禁用/改造。

---

## 12. 影子**写入**验证通过（用户态，无需改内核）

环境：k98（内核 #98）+ 12 槽影子表（`snoc_shadow_reg_v1=y`、`mode=0`），
`ADDRESS_VALID = 0xf860b9`（12 槽已配置）。

```text
baseline:   SR_INTERRUPT_STATUS = 0x100   DIRTY = 0   INVALID_ADDR_ACCESS = 0
            SHADOW_VALUE0        = 0

写 SHADOW_VALUE0 = 0x12345678     (slot0 = CE0 src write-index, 目标 0x24003C)
之后:       SHADOW_VALUE0        = 0x12345678   (值保留)
            DIRTY                = 0            ← 已被硬件消费/转发
            SR_INTERRUPT_STATUS  = 0x101        ← bit0 DIRTY_BIT_SET 被触发
            INVALID_ADDR_ACCESS  = 0            ← 目标地址合法，无错误
            ADDRESS_VALID        = 0xf860b9     (不变)
```

⇒ **SR_APSS 影子通道从"配置"到"数据"全链路验证完成**：
主机写 `SHADOW_VALUE[slot]` → 硬件转发到该槽映射的 CE 寄存器 → 无 invalid-address 错误、
`DIRTY` 脉冲。`SHADOW_VALUE` 不适合"读回"业务值（它保存最后一次写入），
状态通过 `SR_INTERRUPT_STATUS`/`INVALID_ADDR_ACCESS`/`DIRTY` 判断。

## 13. 剩余工作清单（下一位接手请从这里开始）

### 13.1 把 CE 写指针改走影子通道（内核改动，明确）

mainline 里 CE ring 的写指针更新走 `hal_write32_mb()`（`ath11k_hal_srng_*`），
地址形如 `CE0_SRC + n*0x1000 + 0x3C`（src）/ `CE0_DST + n*0x1000 + 0x40`（dst）。
在 SNOC 上应改成：命中 CE 写指针寄存器时，改写 `membase + 0x32000 + 4*slot`，
slot 用 §11.2 的映射表：

```
src CE 0/3/4/5/7        -> slot 0/3/4/5/7
dst CE 1/2/5/7/8/9/10/11 -> slot 13/14/17/19/20/21/22/23
```
（我们的表没列 dst CE5，所以要补 `{5, 0x40}` 才能凑满原厂 13 项 / 12 槽。）

实现位置建议：
* `ahb.c` 新增 `snoc_reg_mode=6`：在 `ath11k_ahb_window_write32_snoc()` 里，
  若 offset 落在 CE 写指针集合内 → 写 `SR_APSS SHADOW_VALUE[slot]`；
  其它 CE 寄存器（ring base/size/hp/tp）先 `ath11k_warn` 跳过，观察固件行为；
* 或更干净：在 `hal.c` 的 `hal_srng_*_hw_index_update()` 里加一个 ops 钩子。

### 13.2 CE 的 ring base/size 等寄存器怎么办（未解）

影子表只覆盖写指针；`SR_BA_LOW/HIGH`、`SR_SIZE`、`HP/TP` 等不在表里。
可能性：固件预先配好（因为数据面由固件自管），主机只需写写指针推进队列。
需要用 §13.1 的实验确认（若 CE 初始化后 HTC 能收到数据，即说明 base/size 由固件负责）。

### 13.3 DP（WBM/REO/TCL）不可达 —— 架构问题

原厂 adrastea 驱动**不编程 DP ring**；mainline 的 `ath11k_dp_srng_setup()`
在本平台必须禁用或彻底改造（改用固件自管数据面 + CE 收发的模型）。
这是"能否让 mainline ath11k 在这颗芯片上工作"的关键判断点。

---

## 14. k99：CE 写指针走影子通道（已实现，验证未完成）

### 14.1 实现（`ahb.c` / `qmi.c`）

* `ahb.c` 新增 `snoc_reg_mode=6`：
  - 新增槽位表 `ath11k_snoc_src_slot[12] = {0,-1,-1,3,4,5,-1,7,...}`、
    `ath11k_snoc_dst_slot[12] = {-1,13,14,-1,-1,17,-1,19,20,21,22,23}`；
  - `ath11k_snoc_ce_shadow_write()`：CE 块内 `rem==0x3C`（src 写指针）/
    `rem==0x4C`（dst 写指针，即 dst base 0x0C + 0x40）→ 改写
    `membase + 0x32000 + 4*slot`；无槽位的 CE 写 → warn 跳过；
  - mode 6 下非 CE 写也一律 warn 跳过（**故意不碰 DP，避免 abort**）。
* `qmi.c`：`dst_ces[]` 补了 CE5（凑原厂 13 项）。

镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k99-ce-shadow.img`（md5 `b6c3a5c6…`，内核 #98）。

### 14.2 结果

| 表 | 结果 | `ADDRESS_VALID` |
|---|---|---|
| 13 条（含 dst CE5→slot17） | ❌ 断言 | `0x60b9`（bit 0,3,4,5,7,13,14）→ **卡在第 8 条 dst CE5** |
| 12 条（不含 dst CE5） | ✅（k98 已证） | `0xf860b9` |

⇒ **dst CE5 不能被影子映射**：CE5 在原厂配置里是 `PIPEDIR_OUT`（只有 src ring），
没有 dst ring，给它配 dst 写指针会被固件拒绝。**正确表就是 12 条**（src 0/3/4/5/7 + dst 1/2/7/8/9/10/11）。

用 7 条已验证子集（`snoc_shadow_reg_n=7`）+ mode 6 的最后一次测试**没有跑到 WLAN_CFG**：
BDF 下载在 `seg 4` 处收到 `QMI error 19`（`qmi failed to load bdf file`），
原因是上一轮 13 条表导致 WPSS 连续崩溃 5 次（每次 ~13 s 重试），固件处于脏状态。
**mode 6 的 CE 影子写通路尚未被实际跑到**，需要用干净的一次开机重测。

### 14.3 下次继续（k100）

1. 干净开机（先 `/etc/sky-wifi-params` 用安全值 + `snoc_shadow_reg_n=12`、`mode=6`），
   确认 `wlan_cfg` 正常、无断言；
2. 观察 `skip CE write 0x…` 日志：应该只出现在 CE ring base/size 等**非写指针**寄存器上，
   而 src/dst 写指针走影子；
3. 若 HTC 仍然起不来 → 说明 CE ring 的 base/size 必须由固件预配置（或需要另一条通路），
   此时回到 §13.2/13.3 的判断：mainline 的 DP/CE 模型与这颗芯片不兼容，需要架构级改造。

---

## 15. k100 现场状态（等待分析）

* 镜像：`wifi-backup/out/sky-shim-boot-v4-wifi-k100-mode6-clean.img`
  （md5 `bf506556…`，内核 #99 系列）—— = k99 的 mode 6 代码 + **dst 表恢复成 7 项**（共 12 条，不含 CE5）。
* 已刷入 `boot_a`（/dev/sde13），并写入参数：
  `snoc_reg_mode=6 / snoc_membase_mib=0 / bdf_total_full=y / snoc_probe=n /
   snoc_shadow_probe=y / snoc_device_info=n / snoc_shadow_reg_v1=y / snoc_shadow_reg_mode=0`
  然后 sysrq-b 重启。
* 读取结果的第一次检查被设备重启打断（用户侧重启/离开），**结论尚未取得**。

### 回来后第一件事（按顺序看）

```sh
ssh root@192.168.7.1
dmesg | grep -c platform_helium                      # 期望 0（有断言说明表又被拒）
dmesg | grep -c "synchronous external abort"         # 期望 0（mode 6 不碰 DP）
dmesg | grep -iE "shadow_reg v1 mode|skip CE write|htc|ce_init" | tail -20
/usr/local/sbin/skyprobe rd 0x228300e0               # 期望 0xf860b9（12 槽）
```

判读：
* 若 `ADDRESS_VALID=0xf860b9` 且出现若干 `snoc: skip CE write 0x…`（都是 ring base/size 之类，
  不是 0x…3C/0x…4C 写指针）→ mode 6 的映射正确、CE 影子写通路在工作；
* 若 HTC 报超时/失败 → CE ring base/size 必须由固件预配置（见 §13.2），
  或说明 mainline 的 CE 模型与这颗芯片不兼容；
* 若仍未跑到 WLAN_CFG（BDF 报 `QMI error 19`）→ 先恢复安全参数
  （`snoc_reg_mode=4`、`snoc_shadow_reg_v1=n`）重启一次，让固件回到干净状态再测。

---

## 16. k100 结果（panic 日志实测）与 k101 计划

### 16.1 现场（截图转写，内核 #99）

```text
9.322222] ath11k: snoc: wlan_cfg shadow_reg v1 mode 0 dense 0, 12 entries, off 0x3c/0x40
9.660366] ath11k: snoc: skip CE write 0x240000 (no shadow slot)
9.665101] ath11k: snoc: skip CE write 0x240000 (no shadow slot)
9.666285] ath11k: snoc: skip CE write 0x240080 (no shadow slot)
9.670024] ath11k: snoc: skip CE write 0x2400c0 (no shadow slot)
9.670422] ath11k: snoc: skip CE write 0x240280 (no shadow slot)
9.680228] ath11k: snoc: skip CE write 0x240020 (no shadow slot)
9.686475] ath11k: snoc: skip CE write 0x240400 (no shadow slot)
9.690923] ath11k: snoc: skip CE write 0x240020 (no shadow slot)
9.701553] Internal error: synchronous external abort: 0000000096000010 [#1] SMP
9.726608]  ath11k_pcic_write32+0x4c/0xfc  (P)
9.730853]  ath11k_hal_srng_access_end+0x54/0x68
9.734861]  ath11k_dp_link_desc_setup+0x214/0x350
9.745161]  ath11k_qmi_driver_event_work
9.751111] Kernel panic - not syncing: synchronous external abort: Fatal exception in interrupt
```

### 16.2 结论（三条，都是好消息 + 一个明确的缺口）

1. ✅ **12 条影子表被完整接受**（全程没有 `platform_helium` 断言）——
   "adrastea 12 管 CE 配置 + 12 条 shadow_reg" 这个组合是**稳定可用**的配置。
2. ✅ **mode 6 的 CE 写过滤按设计工作**：CE 块的 ring base/size 等写被逐个跳过并打日志
   （`0x240000/0x240020/0x240080/0x2400c0/0x240280/0x240400`），没有一次落到真硬件。
   注意这些都不是写指针（`0x…3C`/`0x…4C`），因为 CE ring 建立在 base 写被跳过之后就没继续。
3. ❌ **缺口**：DP 路径的写绕过了 mode 6 的钩子 —— `ath11k_dp_link_desc_setup()` →
   `ath11k_hal_srng_access_end()` → **`ath11k_pcic_write32()`** 走的是**另一条写通道**
   （direct write，不经过 `ath11k_ahb_window_write32_snoc()`），于是打到真硬件 → abort。
   而这次 abort 发生在**中断上下文**，所以不是 oops 而是 **kernel panic + 15 s 后重启**。

### 16.3 k101 计划（很小的一步）

把 mode 6 的过滤**上移一层**，覆盖所有写通道：

* 在 `ath11k_pcic_write32()`（以及 `ath11k_ahb_write32_snoc()`，如果它是另一条路径）
  开头加：
  ```c
  if (ath11k_snoc_reg_mode == 6) {
          if (ath11k_snoc_ce_shadow_write(ab, offset, value) == 0)
                  return;          /* CE 写指针 -> SHADOW_VALUE[slot] */
          return;                  /* 其它一切写：跳过（可加 ratelimited 日志） */
  }
  ```
  注意 `ath11k_snoc_ce_shadow_write()` 需要改为按"寄存器偏移"判断，与调用方一致
  （`ath11k_pcic_write32` 拿到的是**偏移**，不是窗口地址 —— 先确认，避免二次误判）。
* 加一个 `snoc_reg_mode=7`（= 6 + 完全静默）方便安静地跑一遍，只看 HTC/WMI 是否推进。
* 目的：让整个 bring-up 过程中**没有任何一次** APSS→WCSS 的真实写，
  从而干净地回答 §13.2 的问题：CE ring 的 base/size 是否真的由固件预配置。

### 16.4 恢复记录（本次 panic 后）

1. `fastboot flash boot out/sky-shim-boot-v4-wifi-nofdt-recover.img` → 以"WiFi DTB 关闭"启动，
   绕过实验参数（当时 `/etc/sky-wifi-params` 里还是 `snoc_reg_mode=6`）。
2. 起来后把 `/etc/sky-wifi-params` 改回安全值（7 行，`snoc_reg_mode=4`、`snoc_shadow_reg_v1=n`）。
3. `dd` 刷回 **k98**（`0e53a0b1…`，已验证回读一致）并重启 → 设备回到已知良好状态。

> 教训：**实验参数一旦写进 `/etc/sky-wifi-params`，会导致每次开机都复现 panic 并自动重启**。
> 以后一律用 `/etc/sky-wifi-params.try`（一次性，脚本消费后即删），
> 或先刷 `nofdt-recover` 镜像再改参数。

---

## 17. k101 结果：写全被过滤了，**读**还没过滤（下一步 k102）

镜像 `wifi-backup/out/sky-shim-boot-v4-wifi-k101-filter-all.img`（md5 `09ba3a7a…`）。
改动：把 mode≥6 的过滤从 `ath11k_ahb_window_write32_snoc()` **上移到 `ath11k_pcic_write32()`**，
覆盖所有写通道；新增 `snoc_reg_mode=7`（= 6 但不打日志）。

实测（mode 7）：

```text
helium=0  panic=0  aborts=1          ← 写不再 abort，也不再 panic ✅
9.417  ath11k: snoc: no window for 0x2420bc
9.421  ath11k: snoc: no window for 0x2490bc
9.426  ath11k: snoc: no window for 0x24a0bc
9.430  ath11k: snoc: no window for 0x24b0bc
9.452  pc : ath11k_ahb_window_read32_snoc+0x13c/0x1e4     ← 读路径
9.455  lr : __ath11k_pcic_read32+0x28/0x58
9.489  ath11k_hw_wcn6855_reo_setup+0x34/0x15c
9.491  ath11k_dp_alloc+0x48c/0x5f0
```

⇒ **写通道已经完全覆盖**（k100 的 panic 消失），但 `ath11k_dp_alloc()` →
`ath11k_hw_wcn6855_reo_setup()` 里的**读**仍然打到真硬件（REO 寄存器 0x24x0bc），
于是 abort（这次在进程上下文，只有 oops，不 panic）。

### k102（很小的一步）

在 `__ath11k_pcic_read32()` / `ath11k_pcic_read32()` 里加同样的过滤：

```c
if (ath11k_snoc_reg_mode >= 6) {
        /* 影子槽只写不读，直接返回 0 即可（也可只对 CE 块返回 0） */
        return 0;
}
```

目的：让整个 bring-up **零真实 MMIO**（读和写都被拦下），
然后观察 HTC/WMI 是否推进，从而干净地回答 §13.2 的问题
（CE ring 的 base/size 是否由固件预配置）。

> 注意：DP 相关的 read-modify-write（如 `reo_setup`）拿到 0 之后会怎样，需要看代码；
> 如果它依赖读回值做判断，可能需要在 mode 7 下**跳过整个 `ath11k_dp_alloc()`**，
> 这才是最终形态（原厂 adrastea 根本不编程 DP）。

---

## 18. k102：零 MMIO 实验 —— **架构结论已确定**

镜像 `wifi-backup/out/sky-shim-boot-v4-wifi-k102-no-mmio.img`（md5 `37dab53d…`）。
在 k101（写全过滤）基础上，把 `__ath11k_pcic_read32()` 也加了 mode≥6 过滤（直接返回 0），
整个 bring-up **没有任何一次真实 WCSS MMIO**。

### 18.1 实测（mode 7）

```text
helium 断言=0   abort=0   panic=0        ← 完全干净，设备正常跑完启动
6.98  shadow probe +0x32060..0x32078（SR_APSS 常驻块，正常可读）
9.30  snoc: wlan_cfg shadow_reg v1 mode 0 dense 0, 12 entries, off 0x3c/0x40  ← 接受
10.47 ath11k: failed to receive control response completion, polling..
11.49 ath11k: ctl_resp never came in (-110)
11.49 ath11k: failed to connect to HTC: -110
11.51 ath11k: failed to start core: -110
```

### 18.2 这说明了什么（回答 §13.2）

整条链路现在能推进到 **HTC 连接**（在零 MMIO 前提下），并在那里超时。
HTC 需要 CE 环真正在硬件里建立（ring base/size/hp/tp），
而 CE 环的建立只能靠**被拦下的那些写**。

⇒ **CE ring 的 base/size 不是由固件预配置的**，主机必须自己写；
而主机只能通过 12 个影子槽访问 CE 的**写指针**（`0x3C`/`0x40`），
其余 CE 寄存器与整个 UMAC/DP 都对 APSS 不可见。

**架构结论（本移植的边界）**：

| 能力 | 是否可用 |
|---|---|
| SR_APSS 常驻块（影子槽配置/状态） | ✅ |
| `WLAN_CFG.shadow_reg`(v1) 12 槽（需 adrastea 12 管 CE 配置） | ✅ |
| 写 CE 写指针（`SHADOW_VALUE[slot]`） | ✅（已验证转发） |
| 写 CE ring base/size/hp/tp | ❌ 无法访问 |
| 读写 UMAC / WBM / REO / TCL（DP 数据面） | ❌ 无法访问 |
| ⇒ mainline ath11k 的 CE + HTC + DP 模型 | **在本 SoC 上不可行** |
| ⇒ 原厂方案 | qcacld-3.0 adrastea profile（数据面由固件自管，主机只做 CE 收发 + WMI/HTT） |

### 18.3 后续建议

1. **不要**再尝试「让 mainline 的 CE/DP 工作」—— 已用硬件实测证伪（§18.2）。
2. 若仍要在 mainline 上推进，唯一方向是**重写数据面模型**：
   只保留 CE（写指针走影子槽）+ HTC/WMI/HTT，DP 全部交给固件；
   但 CE ring 的 base/size 不可写这一点，会让连 CE 都起不来（§18.1 已证），
   所以实际可行性极低。
3. **务实路线**：走 downstream（qcacld-3.0 `adrastea` profile +
   `qca_cld3_adrastea.ko` + icnss2），把它移植到主线内核 —— 参考
   `qcacld3-qca6750-sm4450-sky-report.md`。
4. 本次实验的全部工具（`snoc_reg_mode` 0–7、`snoc_shadow_reg_*`、`skyprobe`、
   `sky-wifi-params.try` 一次性机制）都可复用于上述任何路线。

### 18.4 结论一句话

> WCN6750/adrastea 在 SM4450 上把 APSS 的寄存器视野压缩到「SR_APSS + 12 个 CE 写指针影子槽」，
> 数据面完全由固件自管。mainline ath11k 基于「主机编程 CE/DP 环」的模型在这颗芯片上
> **不可能工作**，这不是驱动移植工作量问题，而是硬件设计边界。

---

## 19. k103 + 补测：**整个 8 MB 窗口只有 SR_APSS 活着**（硬测定论）

### 19.1 k103（`snoc_reg_mode=8`：只放行 CE 块，其余照旧过滤）

```text
helium=0 aborts=0 panic=0
9.470 snoc: no window for 0x2410bc
9.474 snoc: no window for 0x2420bc
9.478 snoc: no window for 0x2490bc
9.482 snoc: no window for 0x24a0bc
9.486 snoc: no window for 0x24b0bc
10.53 failed to receive control response completion, polling..
11.55 ctl_resp never came in (-110) / failed to connect to HTC / failed to start core
```

mode 8 没有真正打到总线（这些 CE 偏移被窗口守卫丢掉），但它暴露了一个**我们从没测过的地址段**：
CE 偏移翻译成 `membase + offset`，即 `0x22800000 + 0x240000 = 0x22A40000`，
而历史扫描只覆盖到 `0x22850000`。

### 19.2 补测（skyprobe 直读物理地址，EL0 安全探测）

```text
0x22a40000 (=membase+0x240000, CE0)      DEAD
0x22a4003c (=CE0 SR_WR_INDEX)            DEAD
0x22a400bc                               DEAD
0x22a41000 / 0x22a410bc (CE1)            DEAD
0x22a4c000 (CE wrapper, 原厂 targetdef)  DEAD
0x22a90000 / 0x22ab0000 / 0x22a00000     DEAD
粗扫 0x22850000..0x23000000 @0x10000：123 个探针，**0 live**
```

⇒ **整个 8 MiB `membase` 窗口里，只有 `0x20000..0x52000`（SR_APSS）可访问**，
其余（含 CE、UMAC、窗口选择器）对 APSS **全部不响应**。
这条现在是实测数据，不再依赖推断。

### 19.3 对路线选择的意义

* mainline ath11k 的 CE 环编程（base/size/hp/tp）**没有任何地址可用** → CE/HTC 起不来。
* 主机唯一的寄存器通路 = SR_APSS + 12 个 CE **写指针**影子槽（已验证可转发）。
* 原厂能工作，说明 CE 环几何（nentries/nbytes_max 等）是随 `WLAN_CFG.tgt_cfg`
  **交给固件**、由固件自己配置的；主机只需用影子槽"踢"写指针。
  这也解释了为什么原厂 `target_shadow_reg_cfg_map` 只覆盖写指针 —— 其它寄存器主机根本不需要碰。

### 19.4 因此若要"不用 downstream 内核"，剩下两条真实路线

1. **重写数据面模型（mainline 内新驱动）**：只做 WLFW QMI + CE 写指针（影子槽）+ WMI/HTT，
   完全不做 CE 环编程、不做 DP；RX 依赖固件通过中断/MSI 投递（需验证固件是否支持）。
   风险：需要确认"固件自己建环 + 主机只踢指针"是否可行（k102/mode7 的 HTC 超时
   可能就是因为主机侧还在等自己建的环）。
2. **out-of-tree 移植 qcacld-3.0 `adrastea` profile 到 mainline**：
   与硬件行为完全对齐（原厂就是这么干的），代价是适配 qdf/hif/PLD 那一层 API。

---

## 20. k104（mode 9）：主机**从未踢过 CE 写指针** → HTC 等的是固件主动发话

镜像 `wifi-backup/out/sky-shim-boot-v4-wifi-k104-mode9-kick.img`（md5 `5158e9fe…`，内核 #103）。
`snoc_reg_mode=9` = 影子写指针生效 + 只允许 SR_APSS 读写 + 其余真实 MMIO 全丢 +
每次影子写打日志（`snoc: shadow kick slot N <- value`）。

### 20.1 实测

```text
helium=0  aborts=0  panic=0
9.50  snoc: wlan_cfg shadow_reg v1 mode 0 dense 0, 12 entries, off 0x3c/0x40   ← 表被接受
11.68 ctl_resp never came in (-110)
11.69 failed to connect to HTC: -110
（日志里【没有任何】"shadow kick" 行）
```

### 20.2 结论

1. **一次影子写都没有发生** —— 因为 `ath11k_htc_wait_target()` 在**等固件主动发 HTC ready**，
   主机在收到它之前不会发送、也就不会踢 CE 写指针。
2. 固件没发 ready ⇒ 它需要先"看到"主机侧的 CE 环（DDR 里的 ring buffer），
   而 ring 的宿主地址只能通过 CE 寄存器告诉它 —— 这部分 APSS 写不到（§19 已实测全 DEAD）。
3. 因此：**"固件自建 CE 环、主机只踢指针"这个假设不成立**（至少在没有额外 QMI 消息的情况下）。
   ⇒ 路线 A（在 mainline 里写"瘦驱动"）也走不通，除非能逆向出原厂是用哪条消息把
   ring 地址交给固件的（原厂 `WLAN_CFG.tgt_cfg` 只给几何参数，不含地址）。

### 20.3 至此的完整判断

| 路线 | 结论 |
|---|---|
| mainline ath11k 原样 | ❌ CE/DP 寄存器不可访问（§19 实测） |
| mainline 改造成"固件自管数据面" | ❌ 固件不自行建环，HTC 无法启动（§20 实测） |
| downstream qcacld-3.0（原厂内核） | ✅ 唯一已知可工作方案 |
| qcacld-3.0 移植到 mainline（out-of-tree） | ⚠️ 理论可行，需适配 qdf/hif/PLD/icnss2 私有 API |

要进一步验证 3，必须先搞清楚原厂到底靠什么把 CE ring 信息交给固件 ——
最直接的办法是在原厂系统上抓 `qmi_send_request` 的**报文内容**（不只是消息 ID），
看 `WLAN_CFG` 里是否还带了 ring 地址字段（我们的 QMI 结构体可能不完整）。
