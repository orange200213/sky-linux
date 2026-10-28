# initramfs — the tiny userland

`init` is PID 1. It mounts the pseudo filesystems, installs busybox applets,
builds the USB gadget (CDC ACM + NCM), starts DHCP and sshd, mounts UFS and runs
a root shell on `/dev/ttyGS0`.

## Building the image

The kernel embeds a *directory* (`CONFIG_INITRAMFS_SOURCE`). That directory is
assembled from an existing Alpine userland — the binaries are not vendored here.

```bash
mkdir -p root/{bin,sbin,lib,usr/lib/ssh,etc/ssh,dev,proc,sys,tmp,run,var/empty,mnt}

# busybox (musl)
cp $ALPINE/bin/busybox            root/bin/
cp $ALPINE/bin/busybox-extras     root/bin/          # udhcpd, telnetd, httpd
cp $ALPINE/lib/ld-musl-aarch64.so.1 root/lib/
ln -sf ld-musl-aarch64.so.1 root/lib/libc.musl-aarch64.so.1

# OpenSSH 9.x: listener + auth + session helpers
cp $ALPINE/usr/sbin/sshd.pam      root/sbin/sshd
cp $ALPINE/usr/bin/ssh-keygen     root/bin/
cp $ALPINE/usr/lib/ssh/sshd-auth.pam    root/usr/lib/ssh/
cp $ALPINE/usr/lib/ssh/sshd-session.pam root/usr/lib/ssh/
cp $ALPINE/usr/lib/libcrypto.so.3 root/lib/
cp $ALPINE/usr/lib/libpam.so.0*   root/lib/ ; ln -sf libpam.so.0.85.1 root/lib/libpam.so.0
cp $ALPINE/usr/lib/libz.so.1.3.2  root/lib/ ; ln -sf libz.so.1.3.2   root/lib/libz.so.1

# ext4 formatting tools (only needed if you want to reformat userdata)
cp $ALPINE/usr/sbin/mke2fs       root/sbin/
cp $ALPINE/usr/lib/lib{ext2fs.so.2.4,com_err.so.2.1,blkid.so.1.1.0,uuid.so.1.3.0,e2p.so.2.3,econf.so.0.8.3} root/lib/
# plus the usual symlinks: libext2fs.so.2, libcom_err.so.2, libblkid.so.1,
#                          libuuid.so.1, libe2p.so.2, libeconf.so.0

cp init root/init && chmod +x root/init
```

`build.sh` only packs the tree into `sky-initramfs.cpio.gz` for inspection; the
kernel packs the directory itself.

Point the kernel at it:

```
CONFIG_INITRAMFS_SOURCE="/absolute/path/to/root"
```

## Accounts

* `root` / `sky` (SHA-512 crypt hash in `/etc/shadow`, generated with
  `openssl passwd -6 -salt sky1 sky`)
* `sshd` uid/gid 22 for privilege separation, `/var/empty` must exist

## Behaviour worth knowing

* Applets have to be installed (`busybox --install -s /bin`) and `PATH`
  exported, otherwise the shell looks like it has no commands.
* `reboot`/`poweroff`/`halt`/`shutdown` are wrappers that sync, unmount `/mnt`
  and use `-f` (direct syscall); busybox's default signals PID 1 and hangs.
* The shell on `/dev/ttyGS0` is restarted whenever the host hangs up.
* `dmesg` is copied to `/mnt/log/dmesg-last.txt` when UFS mounts.
