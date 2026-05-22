// zero28
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>

#include "msettings.h"

///////////////////////////////////////

typedef struct SettingsV1 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int toggled_brightness;
	int toggled_colortemperature;
	int toggled_contrast;
	int toggled_saturation;
	int toggled_exposure;
	int toggled_volume;
	int turbo_a;
	int turbo_b;
	int turbo_x;
	int turbo_y;
	int turbo_l1;
	int turbo_l2;
	int turbo_r1;
	int turbo_r2;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
	int audiosink; // was bluetooth true/false before
	int fanSpeed; // 0-100, -1 for auto
} SettingsV1;

// When incrementing SETTINGS_VERSION, update the Settings typedef and add
// backwards compatibility to InitSettings!
#define SETTINGS_VERSION 1
typedef SettingsV1 Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = SETTINGS_DEFAULT_BRIGHTNESS,
	.colortemperature = SETTINGS_DEFAULT_COLORTEMP,
	.headphones = SETTINGS_DEFAULT_HEADPHONE_VOLUME,
	.speaker = SETTINGS_DEFAULT_VOLUME,
	.mute = 0,
	.contrast = SETTINGS_DEFAULT_CONTRAST,
	.saturation = SETTINGS_DEFAULT_SATURATION,
	.exposure = SETTINGS_DEFAULT_EXPOSURE,
	.toggled_brightness = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_colortemperature = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_contrast = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_saturation = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_exposure = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_volume = 0, // mute is default
	.turbo_a = 0,
	.turbo_b = 0,
	.turbo_x = 0,
	.turbo_y = 0,
	.turbo_l1 = 0,
	.turbo_l2 = 0,
	.turbo_r1 = 0,
	.turbo_r2 = 0,
	.jack = 0,
	.audiosink = AUDIO_SINK_DEFAULT,
	.fanSpeed = SETTINGS_DEFAULT_FAN_SPEED,
};
static Settings* settings;

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];
static int shm_fd = -1;
static int is_host = 0;
static int shm_size = sizeof(Settings);

int scaleBrightness(int);
int scaleColortemp(int);
int scaleContrast(int);
int scaleSaturation(int);
int scaleExposure(int);
int scaleVolume(int);
int scaleFanSpeed(int);

void disableDpad(int);
void emulateJoystick(int);
void turboA(int);
void turboB(int);
void turboX(int);
void turboY(int);
void turboL1(int);
void turboL2(int);
void turboR1(int);
void turboR2(int);

int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}
void putFile(char* path, char* contents) {
	FILE* file = fopen(path, "w");
	if (file) {
		fputs(contents, file);
		fclose(file);
	}
}
void putInt(char* path, int value) {
	char buffer[8];
	sprintf(buffer, "%d", value);
	putFile(path, buffer);
}

void touch(char* path) {
	close(open(path, O_RDWR|O_CREAT, 0777));
}
int exactMatch(char* str1, char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

int peekVersion(const char *filename) {
	int version = 0;
	FILE *file = fopen(filename, "r");
	if (file) {
		fread(&version, sizeof(int), 1, file);
		fclose(file);
	}
	return version;
}

void InitSettings(void) {
	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));

	shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		// puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	else { // host
		// puts("Settings host"); // keymon
		is_host = 1;
		// we created it so set initial size and populate
		ftruncate(shm_fd, shm_size);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

		// peek the first int from fd, it's the version
		int version = peekVersion(SettingsPath);
		if(version > 0) {
			int fd = open(SettingsPath, O_RDONLY);
			if (fd>=0) {
				if (version == SETTINGS_VERSION) {
					read(fd, settings, shm_size);
				}
				else {
					// initialize with defaults
					memcpy(settings, &DefaultSettings, shm_size);

					// overwrite with migrated data
					if(version == 42) {
						// do migration (TODO when needed)
					}
					else {
						printf("Found unsupported settings version: %i.\n", version);
					}
				}

				close(fd);
			}
			else {
				// load defaults
				memcpy(settings, &DefaultSettings, shm_size);
			}
		}
		else {
			// load defaults
			memcpy(settings, &DefaultSettings, shm_size);
		}

		// these shouldn't be persisted
		settings->mute = 0;
	}

	system("amixer -D hw:audiocodec cset name='Headphone Switch' 1");
	system("amixer -D hw:audiocodec cset name='Headphone Volume' 3");
	system("amixer -D hw:audiocodec cset name='HpSpeaker Switch' 1");
	system("amixer sset 'digital volume' 0");       // 0 = 0dB (no attenuation)
	system("amixer sset 'Soft Volume Master' 255"); // 255 = 100%

	// This will implicitly update all other settings based on FN switch state
	SetMute(settings->mute);
}
int InitializedSettings(void) {
	return (settings != NULL);
}
void QuitSettings(void) {
	munmap(settings, shm_size);
	if (is_host) shm_unlink(SHM_KEY);
}
static inline void SaveSettings(void) {
	int fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
	if (fd>=0) {
		write(fd, settings, shm_size);
		close(fd);
		sync();
	}
}

///////// Getters exposed in public API

int GetBrightness(void) { // 0-10
	if (settings->mute && GetMutedBrightness() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedBrightness();

	return settings->brightness;
}
int GetColortemp(void) { // 0-40
	if (settings->mute && GetMutedColortemp() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedColortemp();

	return settings->colortemperature;
}
int GetVolume(void) { // 0-20
	if (settings->mute && GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedVolume();

	if(settings->jack || settings->audiosink != AUDIO_SINK_DEFAULT)
		return settings->headphones;

	return settings->speaker;
}
int GetContrast(void) {
	if (settings->mute && GetMutedContrast() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedContrast();

	return settings->contrast;
}
int GetSaturation(void) {
	if (settings->mute && GetMutedSaturation() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedSaturation();

	return settings->saturation;
}
int GetExposure(void) {
	if (settings->mute && GetMutedExposure() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedExposure();

	return settings->exposure;
}
// monitored and set by thread in keymon
int GetJack(void) {
	return settings->jack;
}
// zero28 has no BT or USB DAC — always report default
int GetAudioSink(void) {
	return AUDIO_SINK_DEFAULT;
}

int GetHDMI(void) {
	return 0;
}

int GetMute(void) {
	return settings->mute;
}
int GetMutedBrightness(void) {
	return settings->toggled_brightness;
}
int GetMutedColortemp(void) {
	return settings->toggled_colortemperature;
}
int GetMutedContrast(void) {
	return settings->toggled_contrast;
}
int GetMutedSaturation(void) {
	return settings->toggled_saturation;
}
int GetMutedExposure(void) {
	return settings->toggled_exposure;
}
int GetMutedVolume(void) {
	return settings->toggled_volume;
}
int GetMuteDisablesDpad(void) {
	return 0;
}
int GetMuteEmulatesJoystick(void) {
	return 0;
}
int GetMuteTurboA(void) {
	return settings->turbo_a;
}
int GetMuteTurboB(void) {
	return settings->turbo_b;
}
int GetMuteTurboX(void) {
	return settings->turbo_x;
}
int GetMuteTurboY(void) {
	return settings->turbo_y;
}
int GetMuteTurboL1(void) {
	return settings->turbo_l1;
}
int GetMuteTurboL2(void) {
	return settings->turbo_l2;
}
int GetMuteTurboR1(void) {
	return settings->turbo_r1;
}
int GetMuteTurboR2(void) {
	return settings->turbo_r2;
}
int GetFanSpeed(void) {
	return settings->fanSpeed;
}

///////// Setters exposed in public API

void SetBrightness(int value) {
	if (settings->mute && GetMutedBrightness() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawBrightness(scaleBrightness(GetMutedBrightness()));

	SetRawBrightness(scaleBrightness(value));
	settings->brightness = value;
	SaveSettings();
}
void SetColortemp(int value) {
	if (settings->mute && GetMutedColortemp() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawColortemp(scaleColortemp(GetMutedColortemp()));

	SetRawColortemp(scaleColortemp(value));
	settings->colortemperature = value;
	SaveSettings();
}
void SetContrast(int value) {
	if (settings->mute && GetMutedContrast() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawContrast(scaleContrast(GetMutedContrast()));

	SetRawContrast(scaleContrast(value));
	settings->contrast = value;
	SaveSettings();
}
void SetSaturation(int value) {
	if (settings->mute && GetMutedSaturation() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawSaturation(scaleSaturation(GetMutedSaturation()));

	SetRawSaturation(scaleSaturation(value));
	settings->saturation = value;
	SaveSettings();
}
void SetExposure(int value) {
	if (settings->mute && GetMutedExposure() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawExposure(scaleExposure(GetMutedExposure()));

	SetRawExposure(scaleExposure(value));
	settings->exposure = value;
	SaveSettings();
}
void SetVolume(int value) { // 0-20
	if (settings->mute && GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return SetRawVolume(scaleVolume(GetMutedVolume()));

	SetRawVolume(scaleVolume(value));
	if (settings->jack || settings->audiosink != AUDIO_SINK_DEFAULT)
		settings->headphones = value;
	else
		settings->speaker = value;
	SaveSettings();
}
// monitored and set by thread in keymon
void SetJack(int value) {
	printf("SetJack(%i)\n", value); fflush(stdout);

	settings->jack = value;
	SetVolume(GetVolume());
}
// zero28 has no BT or USB DAC — no-op
void SetAudioSink(int value) {
	(void)value;
}

void SetHDMI(int value) {
	(void)value;
}

void SetMute(int value) {
	settings->mute = value;

	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
	SetColortemp(GetColortemp());
	SetContrast(GetContrast());
	SetSaturation(GetSaturation());
	SetExposure(GetExposure());

	if(GetMuteTurboA())
		turboA(settings->mute);
	if(GetMuteTurboB())
		turboB(settings->mute);
	if(GetMuteTurboX())
		turboX(settings->mute);
	if(GetMuteTurboY())
		turboY(settings->mute);
	if(GetMuteTurboL1())
		turboL1(settings->mute);
	if(GetMuteTurboL2())
		turboL2(settings->mute);
	if(GetMuteTurboR1())
		turboR1(settings->mute);
	if(GetMuteTurboR2())
		turboR2(settings->mute);
}

void SetMutedBrightness(int value) {
	settings->toggled_brightness = value;
	SaveSettings();
}
void SetMutedColortemp(int value) {
	settings->toggled_colortemperature = value;
	SaveSettings();
}
void SetMutedContrast(int value) {
	settings->toggled_contrast = value;
	SaveSettings();
}
void SetMutedSaturation(int value) {
	settings->toggled_saturation = value;
	SaveSettings();
}
void SetMutedExposure(int value) {
	settings->toggled_exposure = value;
	SaveSettings();
}
void SetMutedVolume(int value) {
	settings->toggled_volume = value;
	SaveSettings();
}
void SetMuteDisablesDpad(int value) {
	(void)value;
}
void SetMuteEmulatesJoystick(int value) {
	(void)value;
}
void SetMuteTurboA(int value) {
	settings->turbo_a = value;
	SaveSettings();
}
void SetMuteTurboB(int value) {
	settings->turbo_b = value;
	SaveSettings();
}
void SetMuteTurboX(int value) {
	settings->turbo_x = value;
	SaveSettings();
}
void SetMuteTurboY(int value) {
	settings->turbo_y = value;
	SaveSettings();
}
void SetMuteTurboL1(int value) {
	settings->turbo_l1 = value;
	SaveSettings();
}
void SetMuteTurboL2(int value) {
	settings->turbo_l2 = value;
	SaveSettings();
}
void SetMuteTurboR1(int value) {
	settings->turbo_r1 = value;
	SaveSettings();
}
void SetMuteTurboR2(int value) {
	settings->turbo_r2 = value;
	SaveSettings();
}
// zero28 has no fan
void SetFanSpeed(int value) {
	(void)value;
}

///////// trimui_inputd modifiers — zero28 does not use trimui_inputd, stub as no-ops

void disableDpad(int value) {
	(void)value;
}
void emulateJoystick(int value) {
	(void)value;
}
void turboA(int value) {
	(void)value;
}
void turboB(int value) {
	(void)value;
}
void turboX(int value) {
	(void)value;
}
void turboY(int value) {
	(void)value;
}
void turboL1(int value) {
	(void)value;
}
void turboL2(int value) {
	(void)value;
}
void turboR1(int value) {
	(void)value;
}
void turboR2(int value) {
	(void)value;
}

///////// Platform specific scaling

int scaleVolume(int value) {
	int raw = value * 5;
	if (raw > 0) raw = 96 + (64 * raw) / 100;
	return raw; // 0 (mute) or 96-160 (audible range)
}

int scaleBrightness(int value) {
	switch (value) {
		case  0: return   1;
		case  1: return   8;
		case  2: return  16;
		case  3: return  32;
		case  4: return  48;
		case  5: return  72;
		case  6: return  96;
		case  7: return 128;
		case  8: return 160;
		case  9: return 192;
		case 10: return 255;
		default: return  16;
	}
}
int scaleColortemp(int value) {
	int raw;

	switch (value) {
		case 0: raw=-200; break; 		// 8
		case 1: raw=-190; break; 		// 8
		case 2: raw=-180; break; 		// 16
		case 3: raw=-170; break;		// 16
		case 4: raw=-160; break;		// 24
		case 5: raw=-150; break;		// 24
		case 6: raw=-140; break;		// 32
		case 7: raw=-130; break;		// 32
		case 8: raw=-120; break;		// 32
		case 9: raw=-110; break;	// 64
		case 10: raw=-100; break; 		// 0
		case 11: raw=-90; break; 		// 8
		case 12: raw=-80; break; 		// 8
		case 13: raw=-70; break; 		// 16
		case 14: raw=-60; break;		// 16
		case 15: raw=-50; break;		// 24
		case 16: raw=-40; break;		// 24
		case 17: raw=-30; break;		// 32
		case 18: raw=-20; break;		// 32
		case 19: raw=-10; break;		// 32
		case 20: raw=0; break;	// 64
		case 21: raw=10; break; 		// 0
		case 22: raw=20; break; 		// 8
		case 23: raw=30; break; 		// 8
		case 24: raw=40; break; 		// 16
		case 25: raw=50; break;		// 16
		case 26: raw=60; break;		// 24
		case 27: raw=70; break;		// 24
		case 28: raw=80; break;		// 32
		case 29: raw=90; break;		// 32
		case 30: raw=100; break;		// 32
		case 31: raw=110; break;	// 64
		case 32: raw=120; break; 		// 0
		case 33: raw=130; break; 		// 8
		case 34: raw=140; break; 		// 8
		case 35: raw=150; break; 		// 16
		case 36: raw=160; break;		// 16
		case 37: raw=170; break;		// 24
		case 38: raw=180; break;		// 24
		case 39: raw=190; break;		// 32
		case 40: raw=200; break;		// 32
	}
	return raw;
}
int scaleContrast(int value) {
	int raw;

	switch (value) {
		// dont offer -5/ raw 0, looks like it might turn off the display completely?
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}
int scaleSaturation(int value) {
	int raw;

	switch (value) {
		case -5: raw=0; break;
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}
int scaleExposure(int value) {
	int raw;

	switch (value) {
		// stock OS also avoids setting anything lower, so we do the same here.
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}
int scaleFanSpeed(int value) {
	// zero28 has no fan
	(void)value;
	return 0;
}

///////// Platform specific, unscaled accessors

void SetRawVolume(int val) { // 0 (mute) or 96-160 (audible range)
	if (settings->mute && GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		val = scaleVolume(GetMutedVolume());

	char cmd[256];
	sprintf(cmd, "amixer sset 'DAC volume' %i &> /dev/null", val);
	system(cmd);
}

#define DISP_LCD_SET_BRIGHTNESS  0x102
void SetRawBrightness(int val) { // 0-255, linear: higher = brighter
	int fd = open("/dev/disp", O_RDWR);
	if (fd >= 0) {
		unsigned long param[4] = {0, val, 0, 0};
		ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
		close(fd);
	}
}

void SetRawColortemp(int val) {
	FILE *fd = fopen("/sys/devices/virtual/disp/disp/attr/color_temperature", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawContrast(int val) {
	FILE *fd = fopen("/sys/devices/virtual/disp/disp/attr/enhance_contrast", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawSaturation(int val) {
	FILE *fd = fopen("/sys/devices/virtual/disp/disp/attr/enhance_saturation", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawExposure(int val) {
	FILE *fd = fopen("/sys/devices/virtual/disp/disp/attr/enhance_bright", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawFanSpeed(int val) { // zero28 has no fan
	(void)val;
}
