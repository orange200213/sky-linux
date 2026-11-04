#!/bin/sh
# Push the sky-linux rootfs overlay to the device and apply the OpenRC
# runlevels. Safe to re-run.
#
#   tools/install-rootfs.sh            # files + runlevels (/init untouched)
#   tools/install-rootfs.sh --init     # ... and install the /init shim
#
# The overlay mirrors /: rootfs/init -> /init, rootfs/etc/... -> /etc/...
set -eu

HOST=${HOST:-root@192.168.7.1}
PASS=${PASS:-sky}
WITH_INIT=no
[ "${1:-}" = "--init" ] && WITH_INIT=yes

here=$(cd "$(dirname "$0")" && pwd)
overlay="$here/../rootfs"

ssh_d() {
	sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 "$HOST" "$@"
}

if [ "$WITH_INIT" = yes ]; then
	echo "==> backing up /init"
	ssh_d '[ -f /init.pre-openrc ] || cp -f /init /init.pre-openrc; ls -l /init.pre-openrc'
fi

echo "==> pushing files to $HOST"
if [ "$WITH_INIT" = yes ]; then
	tar -C "$overlay" -cf - .
else
	tar -C "$overlay" -cf - --exclude=./init .
fi | ssh_d 'cd / && tar xf - --no-same-owner'

ssh_d 'chmod 0755 /etc/init.d/sky-* /etc/init.d/sshd /etc/init.d/dev
      chmod 0644 /etc/inittab /etc/network/interfaces /etc/udhcpd.conf
      chmod 0755 /init'

echo "==> applying runlevels"
ssh_d '
set -e
for s in devfs procfs sysfs;                  do rc-update add $s sysinit  >/dev/null; done
for s in sky-initramfs fsck root localmount bootmisc hostname loopback \
         sky-adsp sky-modules networking;     do rc-update add $s boot     >/dev/null; done
for s in sshd sky-udhcpd sky-console local sky-wifi; do rc-update add $s default >/dev/null; done
for s in sky-shutdown;                        do rc-update add $s shutdown >/dev/null; done
'

if [ "$WITH_INIT" = yes ]; then
	echo "==> /init installed:"
	ssh_d 'head -2 /init; ls -l /init'
fi
