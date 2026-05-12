###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(PREFIX))
	$(error missing PREFIX for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET=gametimedb

.PHONY: build
.PHONY: clean

CC = $(CROSS_COMPILE)gcc

SYSROOT     := $(shell $(CC) --print-sysroot)

INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/utils.c ../common/api.c ../../$(PLATFORM)/platform/platform.c

CFLAGS  += $(OPT)
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -s

PRODUCT= build/$(PLATFORM)/lib$(TARGET).so

build: sqlite3.o
	mkdir -p build/$(PLATFORM)
	$(CC) -c -Werror -fpic $(SOURCE) -Wl,--no-as-needed $(LDFLAGS) $(CFLAGS)
	$(CC) -shared -o $(PRODUCT) "$(TARGET).o" "utils.o" sqlite3.o $(LDFLAGS)
	cp "$(TARGET).h" "$(PREFIX_LOCAL)/include"
	cp $(PRODUCT) "$(PREFIX_LOCAL)/lib"

sqlite3.o: sqlite3.c
	$(CC) -c -fpic -fvisibility=hidden -DSQLITE_THREADSAFE=0 sqlite3.c -o sqlite3.o

clean:
	rm -f *.o sqlite3.o
	rm -f $(PRODUCT)
	rm -f $(PREFIX_LOCAL)/include/$(TARGET).h
	rm -f $(PREFIX_LOCAL)/lib/lib$(TARGET).so