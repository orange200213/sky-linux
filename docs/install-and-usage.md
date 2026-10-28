# Install and use a released boot image

Every release ships **one file**: `sky-linux-<milestone>-boot-a.img`, an Android
v4 boot image that contains the shim, the mainline kernel and the device tree.
It is flashed to `boot_a` **only** — nothing else on the phone is touched
(`boot_b`, `vendor_boot`, `super`, `userdata` stay as they are).

> Early image: it boots to a Linux user land with a USB serial console, a USB
> network + SSH, and UFS mounted read/write. There is **no display driver** yet
> — the screen shows the kernel's framebuffer console, not a desktop.

## Requirements

* Xiaomi Redmi 12 5G (`sky`, SM4450) with an unlocked bootloader and `fastboot`
* active slot `a` (`fastboot getvar current-slot` → `a`)
* a PC with `fastboot`, `ssh`, and (optionally) `picocom`/`screen`
* the stock `boot.img` of your firmware as a recovery image

## Flash

```bash
# 1. verify the download
sha256sum sky-linux-m4-boot-a.img        # compare with the release notes

# 2. flash only boot_a
fastboot flash boot_a sky-linux-m4-boot-a.img
fastboot reboot
```

Recovery at any time:

```bash
fastboot flash boot_a /path/to/stock/boot.img
fastboot reboot
```

`Mirrors`: the image is self-contained, it never writes to storage.

## What happens on boot

1. the bootloader loads our image and jumps to the shim,
2. the shim copies the kernel to `0xC0000000`, the DTB right behind it, and
   jumps at EL1,
3. Linux boots with the framebuffer console (`simpledrm`) on the display ABL
   left behind,
4. the initramfs brings up the USB gadget (CDC ACM + CDC NCM), a DHCP server,
   `sshd`, mounts UFS (`userdata`, ext4) on `/mnt` and saves
   `/mnt/log/dmesg-last.txt`,
5. a root shell waits on the USB serial port.

## Use it

### Serial console (recommended)

Plug the phone into the PC with a **data** cable (not charge-only).

```bash
dmesg | tail -5                      # expect: cdc_acm … ttyACM0
ls -l /dev/ttyACM*                   # /dev/ttyACM0
sudo picocom -b 115200 /dev/ttyACM0  # exit: Ctrl-A Ctrl-X
```

You should land in a shell (`~ #`) as root.

### SSH over USB networking

The phone runs a DHCP server on the USB interface.

```bash
ip -br link | grep -E 'enx|usb'      # the new interface
sudo dhclient -v <iface>             # or: sudo ip addr add 192.168.7.20/24 dev <iface>
ping 192.168.7.1
ssh root@192.168.7.1                 # password: sky
```

### Storage

```sh
cat /proc/partitions                 # 6 UFS LUNs, userdata = /dev/sda20
df -h /mnt                           # ~84 GiB free
cat /mnt/log/dmesg-last.txt | head   # log of the current boot
skyufs                               # helper: block sizes + partitions + ufs log
```

### Power

```sh
reboot            # clean reboot (sync + umount /mnt, then reboot(2))
poweroff          # real power off
```

## Known limitations of this milestone

| Area | State |
|------|-------|
| display | framebuffer console only (no DPU/DSI/panel driver) |
| touch / keys | only the PMIC power key (no touch driver) |
| Wi-Fi / BT | not enabled (WCN6750 nodes are `disabled` in the DTS) |
| audio | not enabled |
| camera | not enabled |
| `userdata` | mounted read/write; Android data is still there and untouched |
| Android | will not boot while this image is in `boot_a` |

## Troubleshooting

| Symptom | Meaning |
|---------|---------|
| stuck on the vendor splash, no console | kernel died before `simpledrm`; re-flash stock boot and report the DTS/kernel version |
| screen goes grey/white early | the display lost its DSI stream (clocks switched off); make sure `clk_ignore_unused` is in the boot arguments |
| no `/dev/ttyACM0` | check `lsusb` for `1d6b:0104`; if absent the gadget did not bind |
| `ssh: Connection refused` | `sshd` not running — use the serial console and read `/tmp/sshd.log` |
| UFS I/O hangs | known issue if the `qcom,sm4450-ufshc` quirk is missing from the kernel |

## Verifying the build yourself

Everything in the image is reproducible from this repository:
`docs/m0-boot-shim.md` (shim + boot image), `kernel/README.md` (kernel build),
`dts/README.md` (device tree), `initramfs/README.md` (user land).
