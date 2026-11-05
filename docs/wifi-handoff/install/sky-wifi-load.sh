#!/bin/sh
# sky: load the mainlined qcacld-3.0 + icnss2 WLAN stack and bring up wlan0.
# One load per boot (CONFIG_MODULE_UNLOAD=n). Idempotent; guarded by /run/sky-wifi.lock.
LOG=/log/wifi-load.log
M=/root
FW=/lib/firmware
WPA_CONF=/etc/wpa_supplicant/wpa_supplicant.conf
LOCK=/run/sky-wifi.lock

log() { echo "sky-wifi-load: $*" >> "$LOG"; }

[ -f "$LOG" ] && [ "$(wc -l < "$LOG" 2>/dev/null || echo 0)" -gt 300 ] && : > "$LOG"
mkdir -p /run
if ! mkdir "$LOCK" 2>/dev/null; then
	log "another instance is running ($LOCK) -> exit"
	exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM

log "=== $(date '+%F %T') start ==="

ensure_default_route() {
	ip route | grep -q "^default .*wlan0" && return 0
	gw=$(ip route | awk '/^default/ {print $3; exit}')
	[ -z "$gw" ] && gw=$(sed -n 's/^\(router=\|opt router \)//p' /var/run/udhcpc.wlan0.info 2>/dev/null | awk '{print $1; exit}')
	[ -z "$gw" ] && gw=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*brd \([0-9.]*\) .*/\1/p' | awk -F. '{print $1"."$2"."$3".1"}')
	[ -n "$gw" ] && ip route replace default via "$gw" dev wlan0 && log "default route via $gw dev wlan0"
}

if [ -n "$(lsmod | grep '^wlan_firmware_service')" ]; then
	log "modules already loaded; ensuring link"
else
	for f in $M/wlan_firmware_service.ko $M/icnss2.ko $M/wlan.ko \
	         $FW/adrastea/regdb.bin $FW/wlan/qca_cld/WCNSS_qcom_cfg.ini; do
		[ -s "$f" ] || { log "missing/empty: $f -> abort"; exit 1; }
	done

	# start the WPSS remote processor (kernel tried at ~1.9 s, before /lib/firmware)
	i=0
	while [ $i -lt 180 ]; do
		for r in /sys/class/remoteproc/remoteproc*; do
			[ -r "$r/name" ] || continue
			[ "$(cat $r/name 2>/dev/null)" = wpss ] || continue
			[ "$(cat $r/state 2>/dev/null)" = running ] && i=999 && break
			echo start > $r/state 2>/dev/null
		done
		[ "$i" = 999 ] && break
		i=$((i+1)); sleep 0.5
	done
	if [ "$i" = 999 ]; then log "wpss remoteproc running (started on demand)"; else log "WARN: wpss not running after 90s"; fi

	# modules are loaded strictly in order: each step must succeed before the next
	load_mod() {
		if insmod "$1"; then
			log "$2 loaded"
			return 0
		fi
		log "insmod $2 FAILED (rc=$?)"
		return 1
	}

	wait_fw_ready() {
		i=0
		while [ $i -lt 60 ]; do
			dmesg | grep -q "WLAN FW is ready" && break
			i=$((i+1)); sleep 0.5
		done
		if dmesg | grep -q "WLAN FW is ready"; then
			log "WLAN FW is ready: $(dmesg | grep -m1 'WLAN FW is ready' | sed 's/.*: //')"
			return 0
		fi
		log "FW ready not seen in 30s -> abort"
		return 1
	}

	load_mod "$M/wlan_firmware_service.ko" wlan_firmware_service && \
	load_mod "$M/icnss2.ko" icnss2 && \
	wait_fw_ready && \
	load_mod "$M/wlan.ko" wlan || {
		log "bring-up aborted (insmod/FW chain failed)"
		exit 1
	}
	sleep 3
fi

ip link set wlan0 up 2>/dev/null && log "wlan0 up"

# power save off (helps latency/reliability on this firmware)
iw dev wlan0 set power_save off 2>/dev/null && log "power_save off"

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
	ensure_default_route
fi

log "=== done ==="
