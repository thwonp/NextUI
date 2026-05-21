ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(SYSROOT))
$(error missing SYSROOT for this toolchain)
endif

ifeq (,$(CROSS_TRIPLE))
$(error missing CROSS_TRIPLE for this toolchain)
endif

###########################################################

SDL2_VERSION  = 2.26.1
SDL2_TARBALL  = SDL2-$(SDL2_VERSION).GE8300.tgz
SDL2_URL      = https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/$(SDL2_TARBALL)
SDL2_SRC      = SDL2-$(SDL2_VERSION)
SDL2_SONAME   = libSDL2-2.0.so.0.2600.1

OUTPUT_DIR    = $(CURDIR)/output

DL_DIR        = $(CURDIR)/dl

CC            = $(CROSS_COMPILE)gcc
CXX           = $(CROSS_COMPILE)g++
STRIP         = $(CROSS_COMPILE)strip

###########################################################

.PHONY: all clean

all: $(OUTPUT_DIR)/$(SDL2_SONAME)

$(DL_DIR)/$(SDL2_TARBALL):
	mkdir -p $(DL_DIR)
	wget -q --show-progress -O $@ $(SDL2_URL)

$(SDL2_SRC)/configure: $(DL_DIR)/$(SDL2_TARBALL)
	tar -xzf $(DL_DIR)/$(SDL2_TARBALL)
	touch $(SDL2_SRC)/configure

# Note on path nesting: we cd into $(SDL2_SRC)/build and run ../configure
# (out-of-tree build). SDL2's own Makefile then uses an internal `build/`
# subdir for object files, so libtool emits the .so at
# $(SDL2_SRC)/build/build/.libs/, not $(SDL2_SRC)/build/.libs/.
$(SDL2_SRC)/build/build/.libs/$(SDL2_SONAME): $(SDL2_SRC)/configure
	mkdir -p $(SDL2_SRC)/build
	cd $(SDL2_SRC)/build && \
		CC="$(CC)" CXX="$(CXX)" \
		../configure \
			--host=$(CROSS_TRIPLE) \
			--prefix=$(SYSROOT)/usr \
			--enable-shared=yes \
			--enable-static=no \
			--enable-video-mali \
			--enable-video-opengles2 \
			--disable-video-kmsdrm \
			--disable-video-x11 \
			--disable-video-wayland \
			--with-sysroot=$(SYSROOT)
	cd $(SDL2_SRC)/build && make -j$(shell nproc)

$(OUTPUT_DIR)/$(SDL2_SONAME): $(SDL2_SRC)/build/build/.libs/$(SDL2_SONAME)
	mkdir -p $(OUTPUT_DIR)
	$(STRIP) --strip-unneeded -o $@ $<
	@echo "Built: $@"

clean:
	rm -rf $(SDL2_SRC) $(OUTPUT_DIR)
