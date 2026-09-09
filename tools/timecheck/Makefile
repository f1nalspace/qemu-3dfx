# timecheck -- are the guest clocks still telling the truth once the wrapper hooked them?
#
# Background in docs/LOG.md [380] and [409]: UT runs far too fast over Direct3D and OpenGL
# but not over Glide, and HookPatchTimer() in wrappers/fxlib/fxhook.c redirects timeGetTime
# and GetTickCount on NT/XP.
#
#   make          builds build/TIMECHK.EXE
#   make clean

SRC    := src/timecheck.c
BUILD  := build
TARGET := $(BUILD)/TIMECHK.EXE

CROSS  := i686-w64-mingw32-

# Same switches as the other tools: C89 with GCC extensions, the guest CPU as target, and
# msvcrt-os so nothing links against the UCRT that Windows 9x does not have.
CFLAGS := -std=gnu89 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os
LDLIBS := -lopengl32 -lwinmm -lgdi32 -luser32

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
