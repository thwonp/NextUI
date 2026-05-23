#!/bin/sh

WIFI_INTERFACE="wlan0"

start() {
    modprobe 8189es 2>/dev/null
    /usr/sbin/rfkill unblock wifi 2>/dev/null
    /etc/init.d/wpa_supplicant start

    # Wait for wpa_supplicant control socket (up to 5 seconds)
    WPA_SOCK="/etc/wifi/sockets/wlan0"
    WPA_CLI_ARGS="-p /etc/wifi/sockets -i $WIFI_INTERFACE"
    i=0
    while [ $i -lt 10 ]; do
        [ -S "$WPA_SOCK" ] && break
        usleep 500000
        i=$((i + 1))
    done

    # Clear any zero-BSSID filter that wpa_supplicant wrote into the config.
    # A saved bssid=00:00:00:00:00:00 acts as a filter that matches no real AP,
    # causing wpa_supplicant to scan forever and never associate.
    if [ -S "$WPA_SOCK" ]; then
        wpa_cli $WPA_CLI_ARGS list_networks 2>/dev/null | tail -n +2 | \
        while IFS=$(printf '\t') read -r net_id rest; do
            [ -z "$net_id" ] && continue
            bssid=$(wpa_cli $WPA_CLI_ARGS get_network "$net_id" bssid 2>/dev/null)
            bssid=$(printf '%s' "$bssid" | tr -d '\n')
            if [ "$bssid" = "00:00:00:00:00:00" ]; then
                wpa_cli $WPA_CLI_ARGS set_network "$net_id" bssid any 2>/dev/null
            fi
        done
        wpa_cli $WPA_CLI_ARGS reassociate 2>/dev/null
    fi

    if ! pidof udhcpc > /dev/null 2>&1; then
        udhcpc -i $WIFI_INTERFACE -b 2>/dev/null
    fi
}

stop() {
    /etc/init.d/wpa_supplicant stop
    /usr/sbin/rfkill block wifi
    killall udhcpc 2>/dev/null
    rmmod 8189es 2>/dev/null
}

case "$1" in
  start|"")
        start
        ;;
  stop)
        stop
        ;;
  *)
        echo "Usage: $0 {start|stop}"
        exit 1
esac
