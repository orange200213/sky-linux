# M3 — USB networking and SSH

**Result:** the phone is reachable at `192.168.7.1` over the USB cable, hands
out a DHCP lease to the host, and runs OpenSSH. `ssh root@192.168.7.1`
(password `sky` in the bring-up image) gives a root shell.

## Composite gadget

One UDC, one gadget, two functions:

```sh
G=/sys/kernel/config/usb_gadget/sky
mkdir -p $G/functions/acm.usb0 $G/functions/ncm.usb0
ln -s $G/functions/acm.usb0 $G/configs/c.1/acm.usb0    # host: /dev/ttyACM0
ln -s $G/functions/ncm.usb0 $G/configs/c.1/ncm.usb0    # host: enx…/usb0
echo $(ls /sys/class/udc | head -1) > $G/UDC
```

Useful extras:

```sh
# stable MACs so the host interface name does not change between boots
echo 02:11:22:33:44:55 > $G/functions/ncm.usb0/host_addr
echo 02:11:22:33:44:66 > $G/functions/ncm.usb0/dev_addr
```

Device side:

```sh
ip link set usb0 up
ip addr add 192.168.7.1/24 dev usb0
udhcpd /tmp/udhcpd.conf      # busybox-extras: hands out 192.168.7.10-50
```

`/tmp/udhcpd.conf`:

```
start 192.168.7.10
end 192.168.7.50
interface usb0
lease_file /tmp/udhcpd.leases
opt router 192.168.7.1
opt dns 192.168.7.1
```

Host side, if DHCP does not pick it up:

```bash
sudo ip addr add 192.168.7.20/24 dev <enx… or usb0>
sudo ip link set <if> up
ssh root@192.168.7.1
```

## OpenSSH 9.x needs three binaries

First attempt gave `Connection refused`. The serial console explained why:

```
~ # /sbin/sshd -f /etc/ssh/sshd_config -E /tmp/sshd.log
~ # cat /tmp/sshd.log
/usr/lib/ssh/sshd-session.pam does not exist or is not executable
```

Modern OpenSSH splits into `sshd` (listener), `sshd-auth` (authentication) and
`sshd-session` (session). They are looked up in `/usr/lib/ssh/`. All three, plus
their libraries, must be present:

```
/sbin/sshd                       -> sshd.pam
/usr/lib/ssh/sshd-auth.pam       needs libpam, libcrypto, libz, libc
/usr/lib/ssh/sshd-session.pam    same
/lib/{ld-musl-aarch64.so.1,libcrypto.so.3,libpam.so.0,libz.so.1}
```

Minimal working `/etc/ssh/sshd_config`:

```
Port 22
PermitRootLogin yes
PasswordAuthentication yes
PermitEmptyPasswords no
UsePAM no
Subsystem sftp internal-sftp
```

Host keys are generated on first boot with `ssh-keygen -A`; `/run/sshd` and
`/var/empty` must exist and `/etc/passwd` needs an `sshd` user (uid 22) for
privilege separation.

## Where the userland binaries come from

The initramfs is assembled from an existing Alpine userland instead of building
one from scratch:

```
bin/busybox                  (musl, dynamic)
bin/busybox-extras           (udhcpd, telnetd, httpd)
sbin/sshd, usr/lib/ssh/*     (OpenSSH 9.x)
bin/ssh-keygen
lib/{ld-musl-aarch64.so.1,libcrypto.so.3,libpam.so.0,libz.so.1,…}
```

Total ≈ 13 MiB uncompressed, ≈ 6 MiB gzipped. `initramfs/README.md` documents
the exact assembly.

## Initramfs `init` (PID 1)

The script does, in order:

1. mount `proc`, `sysfs`, `devtmpfs`, `devpts`, `configfs`, `tmpfs`
2. `busybox --install -s /bin` and `export PATH=/bin:/sbin` — without the applet
   symlinks every command looks "missing"
3. replace `reboot/poweroff/halt/shutdown` with wrappers that `sync`, unmount
   UFS and call `reboot(2)`/`poweroff(2)` directly (`-f`); a plain busybox
   `reboot` signals PID 1, which is this script, and hangs
4. build the configfs gadget, bring `usb0` up, start `udhcpd`
5. mount UFS (M4) and save `dmesg` to `/mnt/log/dmesg-last.txt`
6. `ssh-keygen -A`, start `sshd`, verify port 22 is listening
7. run a root shell on `/dev/ttyGS0`, restarting it when the host hangs up

## Gotchas

* The ACM port is only created by the `acm.usb0` function; if the gadget fails
  to bind, `/dev/ttyGS0` never appears and the shell loop just sleeps.
* `ttyGS0` has no `PATH` inherited unless the init exported it — applets and
  `sshd` are only found through an explicit absolute path otherwise.
* USB re-enumeration on every gadget rebind changes the host interface name if
  you do not pin the MAC addresses.
