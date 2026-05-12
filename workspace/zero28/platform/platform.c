#define _GNU_SOURCE
// zero28
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <sched.h>
#include <msettings.h>
#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>

///////////////////////////////

volatile int useAutoCpu = 1;

static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;
void PLAT_initInput(void) {
	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0)
		LOG_error("Failed initializing joysticks: %s\n", SDL_GetError());
	num_joysticks = SDL_NumJoysticks();
	if (num_joysticks > 0) {
		joysticks = (SDL_Joystick **)malloc(sizeof(SDL_Joystick *) * num_joysticks);
		for (int i = 0; i < num_joysticks; i++) {
			joysticks[i] = SDL_JoystickOpen(i);
			LOG_info("Opening joystick %d: %s\n", i, SDL_JoystickName(joysticks[i]));
		}
	}
}

void PLAT_quitInput(void) {
	if (joysticks) {
		for (int i = 0; i < num_joysticks; i++) {
			if (SDL_JoystickGetAttached(joysticks[i])) {
				LOG_info("Closing joystick %d: %s\n", i, SDL_JoystickName(joysticks[i]));
				SDL_JoystickClose(joysticks[i]);
			}
		}
		free(joysticks);
		joysticks = NULL;
		num_joysticks = 0;
	}
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

void PLAT_updateInput(const SDL_Event *event) {
	switch (event->type) {
	case SDL_JOYDEVICEADDED: {
		int device_index = event->jdevice.which;
		SDL_Joystick *new_joy = SDL_JoystickOpen(device_index);
		if (new_joy) {
			joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * (num_joysticks + 1));
			joysticks[num_joysticks++] = new_joy;
			LOG_info("Joystick added at index %d: %s\n", device_index, SDL_JoystickName(new_joy));
		} else {
			LOG_error("Failed to open added joystick at index %d: %s\n", device_index, SDL_GetError());
		}
		break;
	}

	case SDL_JOYDEVICEREMOVED: {
		SDL_JoystickID removed_id = event->jdevice.which;
		for (int i = 0; i < num_joysticks; ++i) {
			if (SDL_JoystickInstanceID(joysticks[i]) == removed_id) {
				LOG_info("Joystick removed: %s\n", SDL_JoystickName(joysticks[i]));
				SDL_JoystickClose(joysticks[i]);

				// Shift down the remaining entries
				for (int j = i; j < num_joysticks - 1; ++j)
					joysticks[j] = joysticks[j + 1];
				num_joysticks--;

				if (num_joysticks == 0) {
					free(joysticks);
					joysticks = NULL;
				} else {
					joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * num_joysticks);
				}
				break;
			}
		}
		break;
	}

	default:
		break;
	}
}

///////////////////////////////

void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	PLAT_getBatteryStatusFine(is_charging, charge);

	// worry less about battery and more about the game you're playing
	     if (*charge>80) *charge = 100;
	else if (*charge>60) *charge =  80;
	else if (*charge>40) *charge =  60;
	else if (*charge>20) *charge =  40;
	else if (*charge>10) *charge =  20;
	else           		 *charge =  10;
}

void PLAT_getBatteryStatusFine(int* is_charging, int* charge)
{
	*is_charging = getInt("/sys/class/power_supply/axp2202-usb/online");
	*charge = getInt("/sys/class/power_supply/axp2202-battery/capacity");
}

///////////////////////////////

static struct WIFI_connection connection = {
	.valid = false,
	.freq = -1,
	.link_speed = -1,
	.noise = -1,
	.rssi = -1,
	.ip = {0},
	.ssid = {0},
};

static inline void connection_reset(struct WIFI_connection *c)
{
	c->valid = false;
	c->freq = -1;
	c->link_speed = -1;
	c->noise = -1;
	c->rssi = -1;
	*c->ip = '\0';
	*c->ssid = '\0';
}

static bool bluetoothConnected = false;

void PLAT_getNetworkStatus(int* is_online)
{
	if (WIFI_enabled())
		WIFI_connectionInfo(&connection);
	else
		connection_reset(&connection);

	if (is_online)
		*is_online = (connection.valid && connection.ssid[0] != '\0');

	bluetoothConnected = false; // BT not available on this Moss image
}

ConnectionStrength PLAT_connectionStrength(void) {
	if (!WIFI_enabled() || !connection.valid || connection.rssi == -1)
		return SIGNAL_STRENGTH_OFF;
	else if (connection.rssi == 0)
		return SIGNAL_STRENGTH_DISCONNECTED;
	else if (connection.rssi >= -60)
		return SIGNAL_STRENGTH_HIGH;
	else if (connection.rssi >= -70)
		return SIGNAL_STRENGTH_MED;
	else
		return SIGNAL_STRENGTH_LOW;
}

bool PLAT_hasBluetooth(void) { return false; }
bool PLAT_btIsConnected(void) { return false; }

///////////////////////////////

#define BLANK_PATH "/sys/class/graphics/fb0/blank"
void PLAT_enableBacklight(int enable) {
	if (enable) {
		SetRawBrightness(8);                    // wake fix: prevents screen staying dark on some board revisions
		SetBrightness(GetBrightness());
		system("bl_enable");
		putInt(BLANK_PATH, FB_BLANK_UNBLANK);
	}
	else {
		SetRawBrightness(0);
		system("bl_disable");
		putInt(BLANK_PATH, FB_BLANK_POWERDOWN);
	}
}

void PLAT_powerOff(int reboot) {
	system("rm -f /tmp/nextui_exec && sync");
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();

	system("cat /dev/zero > /dev/fb0 2>/dev/null");
	if (reboot > 0)
		touch("/tmp/reboot");
	else
		touch("/tmp/poweroff");
	sync();
	exit(0);
}

int PLAT_supportsDeepSleep(void) { return 1; }

int PLAT_deepSleep(void)
{
	const char *state_path = "/sys/power/state";

	while (!PAD_wake()) {
		int fd = open(state_path, O_WRONLY);
		if (fd < 0)
			return -1;
		write(fd, "mem", 3);
		close(fd);
		SDL_Delay(100); // let input subsystem settle before checking wake event
	}
	return 0;
}

///////////////////////////////

#define GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed"
void PLAT_setCPUSpeed(int speed) {
	int freq = 0;
	switch (speed) {
		case CPU_SPEED_MENU:        freq =  600000; break;
		case CPU_SPEED_POWERSAVE:   freq =  816000; break;
		case CPU_SPEED_NORMAL:      freq = 1416000; break;
		case CPU_SPEED_PERFORMANCE: freq = 1800000; break;
	}
	putInt(GOVERNOR_PATH, freq);
}

void PLAT_setCustomCPUSpeed(int speed) {
	putInt(GOVERNOR_PATH, speed);
}

///////////////////////////////

void PLAT_setRumble(int strength) {
	// no haptic motor on zero28
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	return "Mini Zero 28";
}

void PLAT_getOsVersionInfo(char* output_str, size_t max_len) {
	output_str[0] = '\0';
	FILE *f = fopen("/etc/openwrt_release", "r");
	if (!f) {
		strncpy(output_str, "Unknown", max_len - 1);
		output_str[max_len - 1] = '\0';
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "DISTRIB_DESCRIPTION=", 20) != 0)
			continue;
		// value is 'quoted string\n'
		char *val = line + 20;
		if (*val == '\'') val++;
		size_t len = strlen(val);
		while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r' || val[len-1] == '\''))
			val[--len] = '\0';
		snprintf(output_str, max_len, "%s", val);
		fclose(f);
		return;
	}
	fclose(f);
	strncpy(output_str, "Unknown", max_len - 1);
	output_str[max_len - 1] = '\0';
}

///////////////////////////////

void PLAT_initDefaultLeds(void) {}

///////////////////////////////

int PLAT_setDateTime(int y, int m, int d, int h, int i, int s) {
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock -u -w", y,m,d,h,i,s);
	system(cmd);
	return 0; // why does this return an int?
}

#define MAX_LINE_LENGTH 200
#define ZONE_PATH "/usr/share/zoneinfo"
#define ZONE_TAB_PATH ZONE_PATH "/zone.tab"

static char cached_timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
static int cached_tz_count = -1;

int compare_timezones(const void *a, const void *b) {
	return strcmp((const char *)a, (const char *)b);
}

void PLAT_initTimezones() {
	if (cached_tz_count != -1) { // Already initialized
		return;
	}

	FILE *file = fopen(ZONE_TAB_PATH, "r");
	if (!file) {
		LOG_info("Error opening file %s\n", ZONE_TAB_PATH);
		return;
	}

	char line[MAX_LINE_LENGTH];
	cached_tz_count = 0;

	while (fgets(line, sizeof(line), file)) {
		// Skip comment lines
		if (line[0] == '#' || strlen(line) < 3) {
			continue;
		}

		char *token = strtok(line, "\t"); // Skip country code
		if (!token) continue;

		token = strtok(NULL, "\t"); // Skip latitude/longitude
		if (!token) continue;

		token = strtok(NULL, "\t\n"); // Extract timezone
		if (!token) continue;

		// Check for duplicates before adding
		int duplicate = 0;
		for (int i = 0; i < cached_tz_count; i++) {
			if (strcmp(cached_timezones[i], token) == 0) {
				duplicate = 1;
				break;
			}
		}

		if (!duplicate && cached_tz_count < MAX_TIMEZONES) {
			strncpy(cached_timezones[cached_tz_count], token, MAX_TZ_LENGTH - 1);
			cached_timezones[cached_tz_count][MAX_TZ_LENGTH - 1] = '\0'; // Ensure null-termination
			cached_tz_count++;
		}
	}

	fclose(file);

	// Sort the list alphabetically
	qsort(cached_timezones, cached_tz_count, MAX_TZ_LENGTH, compare_timezones);
}

void PLAT_getTimezones(char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH], int *tz_count) {
	if (cached_tz_count == -1) {
		LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
		*tz_count = 0;
		return;
	}

	memcpy(timezones, cached_timezones, sizeof(cached_timezones));
	*tz_count = cached_tz_count;
}

char *PLAT_getCurrentTimezone() {
	// easy enough, get current index from config and return the string
	int tz_index = CFG_getCurrentTimezone();
	if (tz_index < 0 || tz_index >= cached_tz_count) {
		LOG_warn("Error: Current timezone index %d out of bounds.\n", tz_index);
		return NULL;
	}

	char *output = (char *)malloc(256);
	if (!output)
		return NULL;

	strncpy(output, cached_timezones[tz_index], 256 - 1);
	output[256 - 1] = '\0'; // Ensure null-termination

	return output;
}

void PLAT_setCurrentTimezone(const char* tz) {
	if (cached_tz_count == -1) {
		LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
		return;
	}

	if (!tz || strlen(tz) == 0) {
		LOG_warn("Error: Invalid timezone string.\n");
		return;
	}

	// get index of timezone
	int tz_index = -1;
	for (int i = 0; i < cached_tz_count; i++) {
		if (strcmp(cached_timezones[i], tz) == 0) {
			tz_index = i;
			break;
		}
	}

	if (tz_index == -1) {
		LOG_warn("Error: Timezone %s not found in cached list.\n", tz);
		return;
	}

	// set in config
	CFG_setCurrentTimezone(tz_index);

	// This fixes the timezone until the next reboot
	char *tz_path = (char *)malloc(256);
	if (!tz_path) {
		return;
	}
	snprintf(tz_path, 256, ZONE_PATH "/%s", tz);
	// replace existing symlink
	if (unlink("/tmp/localtime") == -1) {
		LOG_debug("Failed to remove existing symlink: %s\n", strerror(errno));
	}
	if (symlink(tz_path, "/tmp/localtime") == -1) {
		LOG_error("Failed to set timezone: %s\n", strerror(errno));
	}
	free(tz_path);

	// TODO: verify whether Moss has hwclock before enabling the following line
	// system("hwclock -u -w && hwclock --systz -u");
}

/////////////////////////

// We use the generic video implementation here
#include "generic_video.c"

#define WIFI_SOCK_DIR "/etc/wifi/sockets"
#include "generic_wifi.c"
