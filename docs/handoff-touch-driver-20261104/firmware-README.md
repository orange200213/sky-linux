# 固件说明

驱动通过 DTS 的 `firmware-name = "novatek/nt36672c.bin"` 调用 `request_firmware()`。
文件需要出现在 rootfs 的 `/lib/firmware/novatek/` 或 initramfs 的同一路径下。

## 必须使用原厂 FW 0x18

| 版本 | FW_VER | sha256 | 来源 | 结论 |
| --- | --- | --- | --- | --- |
| **0x18（正确）** | `0x18` / `~0xE7` | `87dba28ecbc7e81f908185007ff03dc1c40585b68e9783cd64e9303b45a1902d` | 本机原厂 `/vendor/firmware/novatek_ts_fw.bin`（139264 B） | 实测 45 分钟无复位 |
| 0x12（错误） | `0x12` / `~0xED` | `ef2465a5c9a0d03a556623032c24ba3e23bd2c430b8d96a8bdfe8141b5e07b11` | 早期误用的其它树固件（同样 139264 B） | 运行约 9.5 分钟后固件复位，且重刷无法救回 |

两者都能通过驱动里 `FW_VER + VER_BAR == 0xFF` 的校验，**只看这个检查分不出来**，
必须比对版本号或与设备 `/vendor/firmware/` 的同一文件做 sha256 比较。

## 校验方法

```sh
# FW 版本在文件里最后一个含 "NVT"/"MOD" 标记的 4 KiB 块的首字节
python3 - <<'PY'
d = open('nt36672c.bin','rb').read()
end = next(i*4096 for i in range(len(d)//4096, 0, -1)
           if d[i*4096-3:i*4096] in (b'NVT', b'MOD'))
print('size', len(d), 'FW_VER', hex(d[end-4096]), 'VER_BAR', hex(d[end-4096+1]))
PY
# 期望: size 139264 FW_VER 0x18 VER_BAR 0xe7
```

本仓库不包含该二进制（版权归 Novatek / 小米），请自行决定是否入库；
不放仓库时把上述文件放进 initramfs 或 rootfs 的 `/lib/firmware/novatek/`。
