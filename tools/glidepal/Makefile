# glidepal -- Glide test case for palettized textures and the chroma key
#
# Two build routes from the same source:
#   make host    Linux, against OpenGLide          -> build/glidepal
#   make win32   cross for Windows 9x, MinGW       -> build/GLIDEPAL.EXE
#   make run     builds host and starts it
#
# The Glide declarations come from glidecube's glidemin.h -- one header for
# both test cases, so a constant is only ever written down once.

QEMU3DFX     := $(abspath ../..)
PROJECT_ROOT := $(abspath ../../..)
OPENGLIDE   ?= $(PROJECT_ROOT)/qemu-xtra/build-openglide/.libs
WRAPPER_DLL ?= $(QEMU3DFX)/wrappers/3dfx/build/glide2x.dll
GLIDEMIN    := $(abspath ../glidecube/src)

SRC     := src/glidepal.c
HEADERS := $(GLIDEMIN)/glidemin.h
BUILD   := build

CFLAGS_COMMON := -std=c89 -Wall -Wextra -O2 -I$(GLIDEMIN)

CROSS       := i686-w64-mingw32-
CFLAGS_WIN  := $(CFLAGS_COMMON) -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all host win32 implib run clean

all: host win32

# --- host: against OpenGLide -------------------------------------------------

host: $(BUILD)/glidepal

$(BUILD)/glidepal: $(SRC) $(HEADERS) | $(BUILD)
	gcc $(CFLAGS_COMMON) -o $@ $(SRC) -L$(OPENGLIDE) -lglide2x -lm

run: host
	LD_LIBRARY_PATH=$(OPENGLIDE) $(BUILD)/glidepal

# --- guest: cross build with MinGW -------------------------------------------

implib: $(BUILD)/libglide2x.a

$(BUILD)/libglide2x.a: $(WRAPPER_DLL) | $(BUILD)
	cd $(BUILD) && gendef $(WRAPPER_DLL)
	$(CROSS)dlltool -d $(BUILD)/glide2x.def -D glide2x.dll -l $@

win32: $(BUILD)/GLIDEPAL.EXE

$(BUILD)/GLIDEPAL.EXE: $(SRC) $(HEADERS) $(BUILD)/libglide2x.a
	$(CROSS)gcc $(CFLAGS_WIN) -o $@ $(SRC) -L$(BUILD) -lglide2x
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
