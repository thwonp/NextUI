ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

TARGET  = hciattach_aic
CC      = $(CROSS_COMPILE)gcc
CFLAGS  = -Os -Wall -Wextra -Wl,--gc-sections

all: $(TARGET)

$(TARGET): hciattach_aic.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
