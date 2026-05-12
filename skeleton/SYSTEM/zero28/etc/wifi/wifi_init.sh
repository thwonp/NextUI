#!/bin/sh

WIFI_INTERFACE="wlan0"

start() {
    rfkill.elf unblock wifi 2>/dev/null
    /etc/init.d/wpa_supplicant start
    if ! pidof udhcpc > /dev/null 2>&1; then
        udhcpc -i $WIFI_INTERFACE -b 2>/dev/null
    fi
}

stop() {
    /etc/init.d/wpa_supplicant stop
    rfkill.elf block wifi
    killall udhcpc 2>/dev/null
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
