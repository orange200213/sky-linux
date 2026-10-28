# M4 — UFS storage

**Result:** the UFS controller enumerates, the GPT is readable, and the
`userdata` partition (ext4, 108 GiB) is mounted read/write as persistent
storage. Kernel logs now survive reboots.

## Kernel side

```
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y
CONFIG_SCSI_UFSHCD=y
CONFIG_SCSI_UFS_QCOM=y
CONFIG_PHY_QCOM_QMP=y
CONFIG_PHY_QCOM_QMP_UFS=y        # must be =y; =m cannot be loaded
CONFIG_EXT4_FS=y
CONFIG_F2FS_FS=y                 # for later, Android uses it elsewhere
```

The QMP UFS PHY driver already contains SM4450 tables (in our tree via the
ROCKNIX `0043` patch), and its match table has
`"qcom,sm4450-qmp-ufs-phy"`.

The UFS **host** driver does not know SM4450, so the node falls back to the
generic `"qcom,ufshc"` compatible and gets `drvdata = NULL` — i.e. **no
quirks**. That is what made the controller die as soon as it went idle:

```
dd if=/dev/sda bs=4096 count=2   →  blocks forever, device unresponsive
```

Fix (`kernel/patches/0002-ufs-qcom-add-sm4450-ufshc-compatible.patch`):

```c
	/* sky bring-up: SM4450 behaves like SM8550 (no PHY retention, LSBDS) */
	{ .compatible = "qcom,sm4450-ufshc", .data = &ufs_qcom_sm8550_drvdata },
```

with

```c
static const struct ufs_qcom_drvdata ufs_qcom_sm8550_drvdata = {
	.quirks = UFSHCD_QUIRK_BROKEN_LSDBS_CAP,
	.no_phy_retention = true,     /* not preserving PHY state across suspend */
};
```

`no_phy_retention` is the important one: without it the driver powers the PHY
down and never restores it, and the next I/O never completes.

## Device tree

`ufshc@1d84000` + `phy@1d87000` (both from the ROCKNIX/Anbernic board file, they
match sky's vendor DT):

```
compatible      "qcom,sm4450-ufshc", "qcom,ufshc", "jedec,ufs-2.0"
lanes-per-direction = <2>
vcc/vccq/vccq2  pm6450 rails (vreg_l…)
reset-gpios     <&tlmm 136 GPIO_ACTIVE_HIGH>
clocks          core_clk, bus_aggr_clk, iface_clk, core_clk_unipro, ref_clk,
                tx_lane0_sync_clk, rx_lane0_sync_clk, rx_lane1_sync_clk
iommus          <&apps_smmu 0x580 0>
```

## What the device looks like

```
sda: logical_block_size=4096 physical_block_size=4096     (6 LUNs, all 4K)

sda   122773504 KiB  (117 GiB)   sda1 … sda20
sdb/sdc   8192 KiB   sdd 32768 KiB   sde 2097152 KiB   sdf …
```

GPT on `sda` (header at byte offset 4096 because of the 4K logical blocks):

```
 1 ssd          2 persist    3 misc        4 countrycode   5 keystore
 6 frp          7 rescue     8 cust (2 GiB) 9 super (6656 MiB)
10 vbmeta_system_a  11 vbmeta_system_b     12 metadata (16 MiB)
13 rawdump     14 opconfig  15 gsort     16 ffu
17 minidump    18 oem      19 log (64 MiB)
20 userdata   110511 MiB (~108 GiB)
```

## Reading the disk: use 4 KiB I/O

`dd bs=512` on a 4K-logical-block device returned all zeroes (and at one point
hung the controller). Always use `bs=4k` or larger:

```sh
dd if=/dev/sda bs=1M count=1 | md5sum      # GPT area, stable across runs
dd if=/dev/sda20 bs=1M count=4 | md5sum
```

## Mounting `userdata`

`/dev/sda20` already contains an **ext4** filesystem (Android's userdata, with
FBE at the file level). It mounts cleanly and gives 84 GiB of free space:

```sh
mkdir -p /mnt
mount -t ext4 /dev/sda20 /mnt
df -h /mnt        # 105.7G total, ~21G used by Android leftovers
echo hello > /mnt/hello.txt
dd if=/dev/zero of=/mnt/skyroot/test.bin bs=1M count=64 && sync && md5sum …
```

The initramfs mounts it on every boot (`/mnt`) and stores
`/mnt/log/dmesg-last.txt`.

If a clean filesystem is wanted, the initramfs also carries `mke2fs`
(from the Alpine userland) plus its libraries:

```sh
mke2fs -q -t ext4 -m 0 -L skyroot -E lazy_itable_init=1 /dev/sda20
```

(The first attempt failed with `econf_errString: symbol not found` because
`libeconf.so.0` was missing from the initramfs — it is included now.)

> `/data` is FBE-encrypted; do not try to read Android's app data from Linux,
> and remember that formatting it destroys the Android installation (TWRP can
> no longer read it, fastboot is unaffected).

## Power commands

`reboot`, `poweroff`, `halt`, `shutdown` are shell wrappers in `init` because
busybox's applets signal PID 1 (our init script) and hang:

```sh
#!/bin/busybox sh
sync; sync
umount /mnt 2>/dev/null
exec /bin/busybox reboot -f      # direct reboot(2), bypasses init
```

`reboot` → PSCI `SYSTEM_RESET` → ABL → our `boot_a` image again.
`poweroff` → PSCI `SYSTEM_OFF` → the phone actually powers down.

## Next step (M5 storage work)

With a persistent 84 GiB filesystem the initramfs can be reduced to a
`switch_root` stub: put a real rootfs in `/mnt/skyroot`, `mount --move` and
`switch_root`, and from then on only the kernel + DTB need flashing.
