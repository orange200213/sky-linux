# Next prompt (copy/paste to a fresh session)

> **Scope of this project (state it in anything you publish).** Everything in this
> repository was developed and verified on the **Japanese-market** Xiaomi Redmi 12 5G
> (`sky`, SM4450 "ravelin"), stock firmware **V14.0.7.0.TMWINXM** (Android 13). Other
> regional variants of the same model may differ in panel, touch controller, PMIC rails,
> modem/WLAN firmware and partition layout — do not present any value here as universal.
>
> **Touch (M7) is *partial*, not done:** it works on the `m19` panel and has only been
> verified from raw `/dev/input/event3` output (`dd` + `tools/decode_evdev.py`) — no
> desktop/window manager has been run against it — and the controller can still drop out
> (the driver recovers in software, but the root cause is not fully explained).
>
> **AI assistance:** this project was built with the help of **DeepSeek V4.1 (1017)** and
> **GPT-5.6 sol**.

> 目标：继续把 mainline `ath11k` 的 WCN6750 **SNOC/adrastea** 分支做完（Xiaomi Redmi 12 5G /
> `sky` / SM4450 ravelin）。**不要**引入 downstream 内核、也不要移植 `qcacld-3.0`。
>
> 先读这两个文件，不要重新推导：
> - `docs/wifi-handoff/HANDOFF-snoc-mainline.md`（当前进度、已解决的坑、knob 列表、陷阱）
> - `docs/m8-wifi.md` §2.3/2.4（hybrid vs SNOC）、§2.7–§2.17（实测日志）
>
> 起点：内核 **#83**（`wifi-backup/out/sky-shim-boot-v4-wifi-k83-snoc.img`）能启动，
> 固件已跑通整套 WLFW/QMI（含 26 KB BDF、regdb、cal report），CE0–CE11 环已初始化；
> 卡在 `ath11k_dp_srng_setup()` 写 UMAC 环寄存器触发 synchronous external abort ——
> **WCSS UMAC/WBM/REO/TCL 寄存器块在这台机器的 DT 8 MiB membase 里访问不到**。
>
> 下一步（按顺序，详见 HANDOFF §3）：
> 1. 把 `ath11k_ahb_snoc_probe_regs()` 从 probe 路径移到 workqueue（**不能在 PID 1 的 bind
>    里跑，会 panic**，见 #84），然后用 `copy_from_kernel_nofault()` 扫 DT 之外可能的
>    第二个物理基址（0x22a00000 / 0x23000000 / 0x23200000 …）。
> 2. 从 vendor 5.10 树确认 SNOC 路径里 `hal_soc->dev_base_addr` / `pld_get_icnss_hw_info()`
>    到底用的是哪块映射（若与 membase 不同 → UMAC 在另一个基址，第 1 步就能找到）。
> 3. 若确实不可 MMIO 访问 → 实现 SR_APSS shadow 写端口
>    （`SHADOW_ADDRESS0..23 @0x30020+4n` / `SHADOW_VALUE0..23 @0x32000+4n` /
>    `ADDRESS_VALID @0x300E0`），先读一个无害的 UMAC 版本寄存器验证握手。
> 4. 之后才是 DP 环、扩展中断（`ath11k_ahb_config_irq()` 现在对 SNOC 直接 return）、
>    WMI/HTC。
>
> 设备访问：`sshpass -p sky ssh root@192.168.7.1`；没有 fastboot（需要时请用户手动进）。
> 迭代方式：`/etc/sky-wifi-params` + `sysrq-b`（~70 秒一轮，**不用刷机**），
> 参数表见 HANDOFF §8；`bdf_total_full=y` 必须保留。
> 崩溃日志在 `/sys/fs/pstore/console-ramoops-0`（重启后仍在）。
> 约束：只动 WiFi；新东西都放 `wifi-backup/` 和本仓库；每次改动前后备份并记录 sha256。
