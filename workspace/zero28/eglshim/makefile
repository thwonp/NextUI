ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

TARGET = egldrain

.PHONY: build clean

CC = $(CROSS_COMPILE)gcc

CFLAGS  = -Os -fpic -Werror
LDFLAGS = -ldl -s -Wl,--version-script=egldrain.ver

build:
	$(CC) $(CFLAGS) -c "$(TARGET).c" -o "$(TARGET).o"
	$(CC) -shared -o "lib$(TARGET).so" "$(TARGET).o" $(LDFLAGS)

clean:
	rm -f *.o lib$(TARGET).so
