# glidecube -- Glide test case for qemu-3dfx
#
# Three build routes from the same source:
#   make host    Linux, against OpenGLide          -> build/glidecube
#   make win32   cross for Windows 9x, MinGW       -> build/GLIDECUB.EXE
#   make run     builds host and starts it
# The third route, Visual C++ 6.0 inside the guest, is described in README.md.

# glide2x.dll comes from this repository. OpenGLide does not -- it lives in
# kjliew/qemu-xtra, expected as a sibling checkout next to this one. Override
# either path on the command line if your layout differs.
QEMU3DFX     := $(abspath ../..)
PROJECT_ROOT := $(abspath ../../..)
OPENGLIDE   ?= $(PROJECT_ROOT)/qemu-xtra/build-openglide/.libs
WRAPPER_DLL ?= $(QEMU3DFX)/wrappers/3dfx/build/glide2x.dll

SRC     := src/glidecube.c
HEADERS := src/glidemin.h
BUILD   := build

# -std=c89 keeps the source at what Visual C++ 6.0 can compile too.
CFLAGS_COMMON := -std=c89 -Wall -Wextra -O2 -Isrc

CROSS       := i686-w64-mingw32-
# -march=pentium3 matches what the guest CPU actually is (qemu -cpu pentium3):
# SSE1, but none of SSE2/SSE3/SSSE3/SSE4.x/POPCNT that -march=x86-64-v2 would
# emit. -mcrtdll=msvcrt-os because of Windows 98: without it the toolchain links
# against the UCRT, which does not exist on Windows 9x.
CFLAGS_WIN  := $(CFLAGS_COMMON) -march=pentium3 -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all host win32 implib run clean

all: host win32

# --- host: against OpenGLide -------------------------------------------------

host: $(BUILD)/glidecube

$(BUILD)/glidecube: $(SRC) $(HEADERS) | $(BUILD)
	gcc $(CFLAGS_COMMON) -o $@ $(SRC) -L$(OPENGLIDE) -lglide2x -lm

run: host
	LD_LIBRARY_PATH=$(OPENGLIDE) $(BUILD)/glidecube

# --- guest: cross build with MinGW -------------------------------------------
#
# MinGW needs an import library. It is derived from the wrapper's built
# glide2x.dll: gendef reads the exports, dlltool turns them into libglide2x.a.

implib: $(BUILD)/libglide2x.a

$(BUILD)/libglide2x.a: $(WRAPPER_DLL) | $(BUILD)
	cd $(BUILD) && gendef $(WRAPPER_DLL)
	$(CROSS)dlltool -d $(BUILD)/glide2x.def -D glide2x.dll -l $@

win32: $(BUILD)/GLIDECUB.EXE

$(BUILD)/GLIDECUB.EXE: $(SRC) $(HEADERS) $(BUILD)/libglide2x.a
	$(CROSS)gcc $(CFLAGS_WIN) -o $@ $(SRC) -L$(BUILD) -lglide2x
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
