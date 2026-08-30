CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lX11 -lXxf86vm

all: build/gammafix

build/gammafix: src/gammafix.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf build
