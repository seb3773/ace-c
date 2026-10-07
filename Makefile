CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wmissing-prototypes -Wno-unused-parameter -O2 -g
CPPFLAGS = -Iinclude
LDFLAGS ?=
PREFIX  ?= /usr/local

LIB_SRCS = \
	src/util.c \
	src/crc.c \
	src/bitstream.c \
	src/huffman.c \
	src/lz77.c \
	src/sound.c \
	src/pic.c \
	src/engine.c \
	src/blowfish.c \
	src/blowfish_tables.c \
	src/compress.c \
	src/oem.c \
	src/sfx_stubs.c \
	src/archive.c

LIB_OBJS = $(LIB_SRCS:.c=.o)

.PHONY: all clean test

all: ace tests/test_core

# Unified binary (mirroring legacy DOS ACE, without the .exe extension).
ace: src/ace.o src/mkace.o src/unace.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_core: tests/test_core.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

HDRS = include/ace.h include/ace/*.h

src/%.o: src/%.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/%.o: tests/%.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: all
	./tests/test_core
	python3 tests/gen_store_ace.py testdata/hello.ace testdata/hello.txt
	./ace t testdata/hello.ace
	./ace l testdata/hello.ace
	./ace a testdata/roundtrip.ace testdata/hello.txt
	./ace t testdata/roundtrip.ace
	./ace a testdata/lz77.ace -z testdata/hello.txt
	./ace t testdata/lz77.ace
	./ace l testdata/lz77.ace
	./ace a testdata/blocked.ace -2 testdata/hello.txt
	./ace t testdata/blocked.ace
	./ace l testdata/blocked.ace
	./ace a testdata/ace_cli.ace -z testdata/hello.txt
	./ace t testdata/ace_cli.ace
	./ace l testdata/ace_cli.ace
	./ace x -d testdata/_acex testdata/ace_cli.ace
	rm -rf testdata/ace_cli.ace testdata/_acex
	python3 tests/test_roundtrip.py
	python3 tests/winace_corpus.py
	rm -rf testdata/hello.ace testdata/roundtrip.ace testdata/lz77.ace testdata/blocked.ace testdata/rt_* testdata/_*

clean:
	rm -rf ace mkace unace tests/test_core src/*.o tests/*.o build
	rm -rf testdata/hello.ace testdata/roundtrip.ace testdata/lz77.ace testdata/blocked.ace testdata/rt_* testdata/_* testdata/real_corpus

# Rebuild embedded SFX stubs (DOS 32-bit, Win32 CL, Win32 GUI, Linux 64-bit ELF)
update-sfx:
	python3 tools/gen_sfx_stubs.py

