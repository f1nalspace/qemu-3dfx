# d3dcube -- Direct3D test case for qemu-3dfx
#
# Unlike glidecube there is only ONE build result. The same EXE runs on the host
# under Wine and in the guest under Windows 98 -- so the two frame rates compare
# directly, with no second compilation standing in between.
#
#   make            builds build/D3DCUBE.EXE
#   make hostinfo   runs it under Wine, adapter details only
#   make hostrun    runs it under Wine and draws
#   make clean

SRC   := src/d3dcube.c
BUILD := build
TARGET := $(BUILD)/D3DCUBE.EXE

CROSS := i686-w64-mingw32-

# -std=gnu89 instead of -std=c89: the source is C89, but d3d8.h uses anonymous
# unions, and those only became standard in C11. As a GCC extension they work in
# gnu89 as well -- and Visual C++ 6.0 knows them anyway.
#
# -march=pentium2 because of the guest CPU, -mcrtdll=msvcrt-os because of
# Windows 98: without the second switch the toolchain links against the UCRT,
# which does not exist on Windows 9x. The same two switches as in glidecube and
# in the qemu-3dfx wrapper.
CFLAGS := -std=gnu89 -Wall -Wextra -O2 -march=pentium2 -mtune=core2 -mcrtdll=msvcrt-os

# Direct3DCreate8 is fetched at run time with LoadLibrary, so d3d8 is NOT
# linked against. Only GDI and USER are needed.
LDLIBS := -lgdi32 -luser32

.PHONY: all hostinfo hostrun clean

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

# Reference number on the host. Wine brings its own d3d8, which leads to the
# same WineD3D -- only straight onto the host GPU instead of through the device
# boundary. Exactly the comparison glidecube draws against OpenGLide.
hostinfo: $(TARGET)
	wine $(TARGET) -info

hostrun: $(TARGET)
	wine $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
