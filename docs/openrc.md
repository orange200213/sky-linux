# OpenRC boot

The rootfs is an ordinary Alpine 3.21 userland driven by OpenRC; PID 1 is
`/sbin/openrc-init`. This file records the boot chain, why a small `/init` still
exists, and the traps that cost us several bricks getting here.

## Boot chain

```
kernel -> (boot.img) initramfs -> switch_root -> /init (shim) -> exec /sbin/openrc-init (PID 1)
                                                                     |
                                        openrc sysinit -> openrc boot -> openrc default
```

`switch_root` is done by the initramfs in the boot image; nothing in this repo
changes it. It execs the path `/init`, which is a plain shell script.

## Why /init is not just a symlink to openrc-init

Each block below prevents a specific, observed failure.

| Block | Why it exists |
|---|---|
| `mkdir -p /proc /sys /dev`, then mount proc/sysfs/devtmpfs/devpts/dev-shm | The rootfs image has shipped **without the `/proc`, `/sys` and `/tmp` directories at all**. `/init` used to mount them with `2>/dev/null`, so the boot "succeeded" with no `/proc`: `sshd` could not allocate ptys (`PTY allocation request failed on channel 0`) and OpenRC cannot run without `/proc`. Never rely on the initramfs having mounted these, either. |
| tmpfs on `/run` | Alpine's `bootmisc` runs in the *boot* runlevel and does `mount --bind / $dir && rm -rf $dir/run/*`, i.e. it wipes the **underlying** `/run` on `/`. That is only safe when a tmpfs sits on top (normally mounted by the initramfs). Otherwise it deletes `/run/openrc/softlevel` mid-boot and the `default` runlevel then decides OpenRC never booted this system and refuses to start anything - no sshd, no console. |
| clock fix | This board has no usable RTC (`/dev/rtc0` reads 1970). OpenRC's `deep_mtime_check()` compares `/etc/init.d`, `/etc/conf.d` and `/etc/rc.conf` against the clock, and with the clock 56 years behind it re-caches the dependency tree on every `rc` invocation, mid-boot. `/init` seeds the clock from the newest `/etc` mtime plus a minute, which also makes `apk`/TLS work. A residual `WARNING: clock skew detected!` can still appear (something in `/etc` gets an mtime newer than the tree); it only rewrites the mtime of `/run/openrc/deptree` and is harmless. |
| `exec >>/var/log/openrc-boot.log` | PID 1's stdout is `/dev/console` = the gadget serial port. If the host does not drain that port (`ModemManager` grabs `/dev/ttyACM0` and only probes it), a write **blocks**, and the boot stops at the first message OpenRC prints. The log is on disk, so a failed boot is diagnosable afterwards. Kernel messages still appear on the console. |
| watchdog | If `:22` is not listening after 120 s, dump state to `/var/log/boot-failed.log`, restore `/init.pre-openrc` and reboot. Without sshd there is no way into the box, so a bad boot would otherwise need a manual reflash. |
| `/sbin/openrc-init` fallback | If the binary is missing, run `/init.pre-openrc` instead of letting PID 1 die (that is a panic loop with `panic=15`). |

`/init.pre-openrc` is the pre-OpenRC shell init. `tools/install-rootfs.sh --init`
refuses to overwrite it once it exists, so it always means "the last init that
worked without OpenRC".

## What replaced the old /init

| old /init (shell) | now |
|---|---|
| adsp remoteproc retry loop | `sky-adsp` (boot) |
| `insmod ktd3136.ko` (+ panel module when built) | `sky-modules` (boot) |
| proc/sys/dev/devpts/dev-shm mounts | `/init` for the minimum, then `devfs`/`procfs`/`sysfs` (sysinit) |
| `hostname sky` | `hostname` (boot) + `/etc/hostname` |
| `usb0` + `192.168.7.1/24` | `networking` (boot) + `/etc/network/interfaces` |
| `udhcpd` with an inline config | `sky-udhcpd` (default) + `/etc/udhcpd.conf` |
| `pkill sshd`, keygen, start sshd | `sshd` (default), the script from the `openssh` package |
| root shell on `/dev/ttyGS0` | `sky-console` (default): `supervise-daemon` + `getty`, login `root`/`sky` |
| power-key backlight toggle | dropped: it was dead code (it sat after the blocking serial loop) and belongs with the DE/WM work |
| ath11k `sky-wifi-up.sh` | dropped, qcacld-3.0 + icnss2 replaced ath11k |

## sky-wifi runs last, on purpose

`sky-wifi` wraps `/usr/local/sbin/sky-wifi-load.sh` (qcacld-3.0 + icnss2:
start WPSS, `insmod` wlan_firmware_service/icnss2/wlan, `wpa_supplicant`,
`udhcpc`). It is `after *` **and** `after local`, and nothing may depend on it,
because `insmod` of the 62 MB `wlan.ko` can bus-abort and panic the box (see
`docs/wifi-handoff/`). By the time it runs, sshd, the serial getty and the DHCP
server are already up, so a failure in there cannot take the box away - the boot
log shows it as the last line of the boot.

It also carries the anti-panic-loop guard: `/var/lib/sky-wifi/in-progress` exists
only while the stack is loading and is removed on a clean shutdown. If it is
still there at the next boot, the previous boot died during the bring-up, so the
service skips **once** (keeping the box reachable to read `/sys/fs/pstore` and
`/log/wifi-load.log`) and clears the guard, so the boot after that tries again.
Worst case the box alternates instead of looping. Retry by hand with
`rm -f /var/lib/sky-wifi/in-progress && rc-service sky-wifi start`.

## Runlevels

```
sysinit : devfs procfs sysfs
boot    : sky-initramfs fsck root localmount bootmisc hostname loopback sky-adsp sky-modules networking
default : sshd sky-udhcpd sky-console local sky-wifi
shutdown: sky-shutdown
```

`dev` (`/etc/init.d/dev`) is a no-op provider: the stock `hwdrivers` and
`machine-id` scripts need a `dev` service, and without one every dependency
cache emits a warning. There is no hotplug manager here; devtmpfs is enough.

`sky-shutdown` only syncs. `openrc-init` runs that runlevel when PID 1 gets
SIGTERM from `reboot`/`poweroff`, and nothing remounts `/` read-only.

## Sockets held by the initramfs

`switch_root` deletes the initramfs but not the processes it started. Its
`sshd` and `udhcpd` keep holding `:22` and `:67`, so the rootfs copies cannot
bind them and the box ends up unreachable even though every service reports
started.

`sky-initramfs` (boot runlevel) reaps exactly those, using a precise test: a
process whose `/proc/<pid>/exe` link ends in ` (deleted)` came from the removed
initramfs. It cannot match anything started from the rootfs.

## Verifying

```sh
rc-status --all                       # every runlevel, every service
tr '\0' ' ' < /proc/1/cmdline         # must be /sbin/openrc-init
netstat -ltn | grep ':22 '            # sshd
netstat -lun | grep ':67 '            # dhcp
ls /var/log/openrc-boot.log           # the rc log of the current boot
ls /var/log/boot-failed.log           # only exists if the watchdog fired
```

## Rolling back

```sh
cp /init.pre-openrc /init && sync && reboot -f
```

The watchdog does exactly this on its own if the OpenRC boot does not produce a
listening sshd within 120 s.

## Deploying changes

```sh
tools/install-rootfs.sh          # files + runlevels, leaves /init alone
tools/install-rootfs.sh --init   # ... and installs the /init shim
```

`rootfs/` mirrors `/`. The host needs `sshpass`; the device defaults to
`root@192.168.7.1`, password `sky` (`HOST`/`PASS` override both).
