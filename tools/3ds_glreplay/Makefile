# GL trace player for the New 3DS: plays traces recorded by runtime/gles.cpp (NOVA3_GLTRACE)
# through runtime/gles_pica.cpp. Build (from Git Bash or MSYS2):
#   C:/msys64/usr/bin/bash.exe -lc "cd /e/nova3/tools/3ds_glreplay && make"
# Output: build/3ds/glreplay/glreplay.3dsx. See main.cpp for how it is run. `make PROFILE=1`
# (after `make clean`) adds the backend's per-draw phase timings (gles_pica.cpp, PROFILE_MARK).
ifeq ($(strip $(DEVKITPRO)),)
export DEVKITPRO := /opt/devkitpro
endif
export DEVKITARM := $(DEVKITPRO)/devkitARM
export PATH := $(DEVKITPRO)/tools/bin:$(DEVKITARM)/bin:$(PATH)
CTRULIB := $(DEVKITPRO)/libctru

ROOT := $(abspath ../..)
OUT := $(ROOT)/build/3ds/glreplay
PREFIX := arm-none-eabi-
CXX := $(PREFIX)g++
CC := $(PREFIX)gcc

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CXXFLAGS := -O2 -g -Wall -Wno-psabi -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ -std=gnu++20 \
            -fno-rtti -fno-exceptions -I$(CTRULIB)/include -I$(ROOT)/runtime -I$(OUT) $(if $(PROFILE),-DPICA_PROFILE)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lcitro3d -lctru -lm

SHADERS := $(ROOT)/runtime/pica/generic.v.pica $(ROOT)/runtime/pica/lit.v.pica $(ROOT)/runtime/pica/point2.v.pica
OBJS := $(OUT)/main.o $(OUT)/gles_pica.o $(OUT)/pica_shbin.o

all: $(OUT)/glreplay.3dsx

$(OUT):
	mkdir -p $@

# the vertex shaders: one DVLB, in the order gles_pica.cpp's Vs enum expects
$(OUT)/pica.shbin: $(SHADERS) | $(OUT)
	picasso -o $@ $(SHADERS)

$(OUT)/pica_shbin.o: $(OUT)/pica.shbin
	cd $(OUT) && bin2s -a 4 -H pica_shbin.h pica.shbin > pica_shbin.s && $(CC) -x assembler-with-cpp $(ARCH) -c pica_shbin.s -o pica_shbin.o

$(OUT)/gles_pica.o: $(ROOT)/runtime/gles_pica.cpp $(ROOT)/runtime/gles_pica.h | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/main.o: main.cpp $(ROOT)/runtime/gles_pica.h | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/glreplay.elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OUT)/glreplay.3dsx: $(OUT)/glreplay.elf
	3dsxtool $< $@

clean:
	rm -f $(OUT)/*.o $(OUT)/*.elf $(OUT)/*.3dsx $(OUT)/*.shbin $(OUT)/pica_shbin.*

.PHONY: all clean
