# ddcube -- DirectDraw and Direct3D 7 test case for qemu-3dfx
#
# Guest only: DirectDraw exists on Windows, so there is no host build. The
# counterpart to d3dcube (Direct3D 8 over WineD3D) and glidecube (Glide).
#
#   make        cross for Windows 9x, MinGW -> build/DDCUBE.EXE

SRC   := src/ddcube.c
BUILD := build

CROSS := i686-w64-mingw32-

# -march=prescott matches the guest CPU (qemu -cpu coreduo, SSE3 is the ceiling), -mcrtdll=msvcrt-os
# because of Windows 98: without it the toolchain links against the UCRT, which
# does not exist on Windows 9x. Same two traps as in the other tools.
CFLAGS := -std=gnu99 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all clean

all: $(BUILD)/DDCUBE.EXE

$(BUILD)/DDCUBE.EXE: $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) -lddraw -ldxguid -lgdi32 -luser32 -lm
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
