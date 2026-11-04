#!/bin/sh
# sky: load the mainlined qcacld-3.0 + icnss2 WLAN stack and bring up wlan0.
# One load per boot (CONFIG_MODULE_UNLOAD=n).
LOG=/log/wifi-load.log
M=/root
FW=/lib/firmware
WPA_CONF=/etc/wpa_supplicant/wpa_supplicant.conf

log() { echo "sky-wifi-load: $*" >> "$LOG"; }

# single instance only (OpenRC + any manual call must not race)
LOCK=/run/sky-wifi.lock
mkdir -p /run
if ! mkdir "$LOCK" 2>/dev/null; then
	log "another instance is running ($LOCK) -> exit"
	exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM

[ -f "$LOG" ] && [ "$(wc -l < "$LOG" 2>/dev/null || echo 0)" -gt 300 ] && : > "$LOG"
log "=== $(date '+%F %T') start ==="

if [ -n "$(lsmod | grep '^wlan_firmware_service')" ]; then
	log "modules already loaded; ensuring link"
else
	for f in $M/wlan_firmware_service.ko $M/icnss2.ko $M/wlan.ko \
	         $FW/adrastea/regdb.bin $FW/wlan/qca_cld/WCNSS_qcom_cfg.ini; do
		[ -s "$f" ] || { log "missing/empty: $f -> abort"; exit 1; }
	done
	# wait for the WPSS remote processor: icnss2's QMI handshake needs it up
	i=0
	while [ $i -lt 180 ]; do
		for r in /sys/class/remoteproc/remoteproc*; do
			[ -r "$r/name" ] || continue
			[ "$(cat $r/name 2>/dev/null)" = wpss ] || continue
			[ "$(cat $r/state 2>/dev/null)" = running ] && i=999 && break
			# starts at ~1.9 s in the kernel, before /lib/firmware is mounted -> ENOENT.
			# Retry here; it is a no-op when the firmware was already loaded.
			echo start > $r/state 2>/dev/null
		done
		[ "$i" = 999 ] && break
		i=$((i+1)); sleep 0.5
	done
	if [ "$i" = 999 ]; then log "wpss remoteproc running (started on demand)"; else log "WARN: wpss not running after 90s"; fi

	insmod $M/wlan_firmware_service.ko || { log "insmod wlan_firmware_service FAILED"; exit 1; }
	log "wlan_firmware_service loaded"
	insmod $M/icnss2.ko || { log "insmod icnss2 FAILED"; exit 1; }
	log "icnss2 loaded"
	i=0
	while [ $i -lt 60 ]; do
		dmesg | grep -q "WLAN FW is ready" && break
		i=$((i+1)); sleep 0.5
	done
	if dmesg | grep -q "WLAN FW is ready"; then
		log "WLAN FW is ready: $(dmesg | grep -m1 'WLAN FW is ready' | sed 's/.*: //')"
	else
		log "WARN: FW ready not seen in 30s"
	fi
	insmod $M/wlan.ko || { log "insmod wlan FAILED"; exit 1; }
	log "wlan loaded"
	sleep 3
fi

ip link set wlan0 up 2>/dev/null && log "wlan0 up"

# association + DHCP (only if a config with a network block is present)
if [ -s "$WPA_CONF" ] && grep -q '^network=' "$WPA_CONF"; then
	mkdir -p /var/run/wpa_supplicant
	if ! pgrep -f "wpa_supplicant.*-i wlan0" >/dev/null 2>&1; then
		wpa_supplicant -B -i wlan0 -D nl80211 -c "$WPA_CONF" 2>/dev/null
		log "wpa_supplicant started"
	fi
	i=0
	while [ $i -lt 30 ]; do
		s=$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')
		[ "$s" = "COMPLETED" ] && break
		i=$((i+1)); sleep 1
	done
	log "wpa_state=${s:-unknown}"
	if [ "$s" = "COMPLETED" ] && ! ip -4 addr show wlan0 | grep -q inet; then
		udhcpc -i wlan0 -q -n -t 6 -T 3 >/dev/null 2>&1
		log "dhcp: $(ip -4 addr show wlan0 2>/dev/null | awk '/inet /{print $2}')"
	fi
fi

log "=== done ==="
