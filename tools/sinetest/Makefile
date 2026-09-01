# sinetest -- waveOut test case for the QEMU sound device models
#
# One build result, for the guest only: waveOut has no counterpart on the host,
# and the host side of the comparison is the recording QEMU writes with
# -audiodev wav. See docs/LOG.md in the qemu-3dfx-build tree.
#
#   make          builds build/SINETEST.EXE
#   make clean

SRC    := src/sinetest.c
SRC_DS := src/sinetest_ds.c
BUILD  := build
TARGET := $(BUILD)/SINETEST.EXE
TARGET_DS := $(BUILD)/SINEDS.EXE

CROSS := i686-w64-mingw32-

# The same two switches as in glidecube, d3dcube and the qemu-3dfx wrapper:
# -march=prescott matches the guest CPU (qemu -cpu coreduo, SSE3 is the ceiling),
# and -mcrtdll=msvcrt-os keeps the toolchain off the UCRT, which does not exist
# on Windows 9x.
CFLAGS := -std=gnu89 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

LDLIBS := -lwinmm
# DirectSound is fetched with LoadLibrary, so dsound is not linked against --
# the same trick d3dcube uses for Direct3DCreate8. Only ole32 is needed for the
# COM-style calls, and even that comes in through windows.h on this toolchain.
LDLIBS_DS := -luser32

.PHONY: all clean

all: $(TARGET) $(TARGET_DS)

$(TARGET): $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(TARGET_DS): $(SRC_DS) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC_DS) $(LDLIBS_DS)
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
