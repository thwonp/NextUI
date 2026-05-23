#!/bin/sh

WIFI_INTERFACE="wlan0"

start() {
    modprobe 8189es 2>/dev/null
    /usr/sbin/rfkill unblock wifi 2>/dev/null
    /etc/init.d/wpa_supplicant start
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
