#!/bin/sh
# sky M8: Wi-Fi (WCN6750 "adrastea") bring-up helper.
#
# The WPSS firmware (this *is* the WLAN firmware) lives in the rootfs, so the
# remoteproc auto-boot at ~0.4 s and the ath11k probe both fail with -ENOENT
# before switch_root. Once /lib/firmware is visible, start WPSS here and let
# ath11k bind again - ath11k boots the remote processor itself (rproc_boot
# refcounts, so an already running wpss is fine).
LOG=/log/wifi-up.log

log() { echo "sky-wifi: $*" >> "$LOG"; }

log "start"

# --- 1. WPSS remoteproc --------------------------------------------------
i=0
state=
while [ $i -lt 60 ]; do
	i=$((i + 1))
	for r in /sys/class/remoteproc/remoteproc*; do
		[ "$(cat "$r/name" 2>/dev/null)" = wpss ] || continue
		state=$(cat "$r/state" 2>/dev/null)
		[ "$state" = running ] || echo start > "$r/state" 2>>"$LOG"
	done
	[ "$state" = running ] && break
	sleep 2
done
log "wpss: ${state:-not found}"

# --- 2. ath11k -----------------------------------------------------------
# The wifi platform device probed at boot, found no firmware and failed, so
# it is unbound. Bind it again now and give the QMI handshake time.
i=0
while [ $i -lt 20 ]; do
	i=$((i + 1))
	dev=
	for d in /sys/bus/platform/devices/*.wifi; do dev=$d; done
	if [ -z "$dev" ]; then
		sleep 2
		continue
	fi
	if [ -e "$dev/driver" ]; then
		log "${dev##*/}: bound"
		break
	fi
	echo "${dev##*/}" > /sys/bus/platform/drivers/ath11k/bind 2>>"$LOG"
	sleep 5
done

[ -e /sys/class/net/wlan0 ] && log "wlan0 present" || log "no wlan0"
log "done"
