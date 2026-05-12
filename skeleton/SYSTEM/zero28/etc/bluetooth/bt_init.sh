#!/bin/sh
# BT init script for MagicX Zero28 (NextUI)
# Adapted from skeleton/SYSTEM/tg5040/etc/bluetooth/bt_init.sh
# Chip: AIC8800-series on /dev/ttyS1; rfkill0 = sunxi-bt
# NOTE: /proc/bluetooth/sleep/btwrite does NOT exist on zero28 — omitted.

BT_TOOL="hciattach_aic"
bt_hciattach="${SYSTEM_PATH:-/mnt/SDCARD/.system/zero28}/bin/$BT_TOOL"
BT_TTY="/dev/ttyS1"
BT_BAUD="1500000"
DEVICE_NAME="MagicX Zero28 (NextUI)"

reset_bluetooth_power() {
	echo 0 > /sys/class/rfkill/rfkill0/state
	sleep 1
	echo 1 > /sys/class/rfkill/rfkill0/state
	sleep 1
}

start_hci_attach() {
	h=$(ps | grep "$BT_TOOL" | grep -v grep)
	[ -n "$h" ] && {
		killall "$BT_TOOL"
		sleep 0.5
	}

	reset_bluetooth_power

	"$bt_hciattach" -n -s "$BT_BAUD" "$BT_TTY" aic >/dev/null 2>&1 &

	wait_hci0_count=0
	while true; do
		[ -d /sys/class/bluetooth/hci0 ] && break
		usleep 100000
		wait_hci0_count=$((wait_hci0_count + 1))
		[ "$wait_hci0_count" -eq 70 ] && {
			echo "$BT_TOOL: bring up hci0 failed at ${BT_BAUD} baud"
			exit 1
		}
	done
	echo "$BT_TOOL: hci0 up at ${BT_BAUD} baud"
}

start_bt() {
	"${SYSTEM_PATH:-/mnt/SDCARD/.system/zero28}/bin/rfkill.elf" unblock bluetooth

	if [ -d "/sys/class/bluetooth/hci0" ]; then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi

	# TODO: 013b — start bluetoothd, bluealsa, bluetoothctl once BlueZ is deployed
	# d=$(ps | grep bluetoothd | grep -v grep)
	# [ -z "$d" ] && {
	#     /etc/bluetooth/bluetoothd start
	#     sleep 1
	# }
	# a=$(ps | grep bluealsa | grep -v grep)
	# [ -z "$a" ] && {
	#     bluealsa -p a2dp-source &
	#     sleep 1
	#     bluetoothctl power on 2>/dev/null
	#     bluetoothctl discoverable on 2>/dev/null
	#     bluetoothctl pairable on 2>/dev/null
	#     bluetoothctl agent NoInputNoOutput 2>/dev/null
	#     bluetoothctl default-agent 2>/dev/null
	#     bluetoothctl system-alias "$DEVICE_NAME" 2>/dev/null
	# }
}

stop_bt() {
	# TODO: 013b — stop bluealsa / bluetoothd once deployed
	# killall bluealsa 2>/dev/null
	# d=$(ps | grep bluetoothd | grep -v grep)
	# [ -n "$d" ] && {
	#     bluetoothctl power off 2>/dev/null
	#     bluetoothctl pairable off 2>/dev/null
	#     killall bluetoothctl 2>/dev/null
	#     killall bluetoothd
	#     sleep 1
	# }

	hciconfig hci0 down 2>/dev/null

	h=$(ps | grep "$BT_TOOL" | grep -v grep)
	[ -n "$h" ] && {
		killall "$BT_TOOL"
		usleep 500000
	}

	# NOTE: no /proc/bluetooth/sleep/btwrite on zero28
	echo 0 > /sys/class/rfkill/rfkill0/state
	echo "stop $BT_TOOL"
}

case "$1" in
	start)
		start_bt
		;;
	stop)
		stop_bt
		;;
	restart)
		stop_bt
		sleep 0.5
		start_bt
		;;
	*)
		echo "Usage: $0 {start|stop|restart}"
		exit 1
		;;
esac
