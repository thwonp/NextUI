#!/bin/sh
# MinUI.pak — zero28 (Moss/MagicX)

export PLATFORM="zero28"
export DEVICE="zero28"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export HOOKS_PATH="$USERDATA_PATH/.hooks"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"
export HOME="$USERDATA_PATH"

#######################################

if [ -f "/tmp/poweroff" ]; then
	poweroff
	exit 0
fi
if [ -f "/tmp/reboot" ]; then
	reboot
	exit 0
fi

#######################################

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$HOOKS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export IS_NEXT="yes"

#######################################

# taken from stock launch sequence
sync
echo 3 > /proc/sys/vm/drop_caches
sync

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/bin:$PATH

# read user wifi preference (default on); used to avoid restarting wifi if the
# user has disabled it in settings
wifion=1
parsed=$(nextval.elf wifi 2>/dev/null | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
[ -n "$parsed" ] && wifion=$parsed

sh "$SYSTEM_PATH/bin/governor.sh" "auto"

keymon.elf & # &> $SDCARD_PATH/keymon.txt &
batmon.elf & # &> $SDCARD_PATH/batmon.txt &

# start fresh, will be populated on the next connect
rm -f $USERDATA_PATH/.asoundrc
audiomon.elf & #&> $SDCARD_PATH/audiomon.txt &

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

# Composable boot hooks (run after auto.sh for backward compatibility)
"$SYSTEM_PATH/bin/run_hooks.sh" boot.d

cd $(dirname "$0")

#######################################

# FIFO QUIT show2.elf so SDL_Quit properly releases the PowerVR GPU context.
# SIGKILL leaves PowerVR in asynchronous cleanup ([pvr_defer_free]) which races
# against nextui.elf's SDL_InitSubSystem. Clean exit avoids the race.
# Sleep 3s after exit: PowerVR hardware needs time to fully reinitialize before
# nextui.elf's SDL_CreateWindow (SDL_WINDOW_OPENGL) can acquire an EGL context.
# 007l diagnostic — remove before upstream PR
{
    echo "=== pre-kill state ==="
    echo "show2.pid: $(cat /tmp/show2.pid 2>/dev/null || echo '(missing)')"
    echo "--- ps ---"
    ps 2>/dev/null
    echo "--- device holders ---"
    for _pid in /proc/[0-9]*; do
        _devs=$(ls -la "$_pid/fd" 2>/dev/null | grep " -> /dev/" | awk '{print $NF}' | tr '\n' ' ')
        [ -n "$_devs" ] && printf "  pid %s (%s): %s\n" \
            "$(basename "$_pid")" \
            "$(cat "$_pid/cmdline" 2>/dev/null | tr '\0' ' ' | cut -c1-40)" \
            "$_devs"
    done
    echo "=== end pre-kill ==="
} > "$LOGS_PATH/launch_diag.txt" 2>&1
sync
SHOW2_PID=$(cat /tmp/show2.pid 2>/dev/null)
echo "QUIT" > /tmp/show2.fifo 2>/dev/null || true
if [ -n "$SHOW2_PID" ]; then
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if ! kill -0 "$SHOW2_PID" > /dev/null 2>&1; then break; fi
        sleep 0.1
    done
fi
killall -9 show2.elf > /dev/null 2>&1 || true
sleep 3
# 007l diagnostic — remove before upstream PR
{
    echo "=== post-quit state ==="
    echo "show2_pid was: $SHOW2_PID"
    echo "kill -0 result: $(kill -0 "$SHOW2_PID" 2>&1; echo exit=$?)"
    echo "--- ps ---"
    ps 2>/dev/null
    echo "--- device holders ---"
    for _pid in /proc/[0-9]*; do
        _devs=$(ls -la "$_pid/fd" 2>/dev/null | grep " -> /dev/" | awk '{print $NF}' | tr '\n' ' ')
        [ -n "$_devs" ] && printf "  pid %s (%s): %s\n" \
            "$(basename "$_pid")" \
            "$(cat "$_pid/cmdline" 2>/dev/null | tr '\0' ' ' | cut -c1-40)" \
            "$_devs"
    done
    echo "=== end post-quit ==="
} >> "$LOGS_PATH/launch_diag.txt" 2>&1
sync

parse_hook_cmd() {
	HOOK_CMD="$1"
	HOOK_EMU_PATH=$(echo "$HOOK_CMD" | sed "s/^'\\([^']*\\)'.*/\\1/")
	_remainder=$(echo "$HOOK_CMD" | sed "s/^'[^']*'//")
	if echo "$_remainder" | grep -q "'"; then
		HOOK_TYPE="rom"
		HOOK_ROM_PATH=$(echo "$_remainder" | sed "s/.*'\\([^']*\\)'.*/\\1/")
	else
		HOOK_TYPE="pak"
		HOOK_ROM_PATH=""
	fi
	[ -f /tmp/last.txt ] && HOOK_LAST=$(cat /tmp/last.txt) || HOOK_LAST=""
	export HOOK_CMD HOOK_EMU_PATH HOOK_TYPE HOOK_ROM_PATH HOOK_LAST
}

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
while [ -f $EXEC_PATH ]; do
	wifi_start_pid=""
	[ "$wifion" -eq 1 ] && { $SYSTEM_PATH/etc/wifi/wifi_init.sh start > /dev/null 2>&1 & wifi_start_pid=$!; }
	nextui.elf &> $LOGS_PATH/nextui.txt

	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		parse_hook_cmd "$CMD"
		[ -n "$wifi_start_pid" ] && { kill "$wifi_start_pid" 2>/dev/null; wait "$wifi_start_pid" 2>/dev/null; }
		$SYSTEM_PATH/etc/wifi/wifi_init.sh stop > /dev/null 2>&1
		wifi_start_pid=""
		"$SYSTEM_PATH/bin/run_hooks.sh" pre-launch.d
		eval $CMD
		"$SYSTEM_PATH/bin/run_hooks.sh" post-launch.d
		rm -f $NEXT_PATH
	fi

	if [ -f "/tmp/poweroff" ]; then
		poweroff
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		reboot
		exit 0
	fi
done

poweroff # just in case
