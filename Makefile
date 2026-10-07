CC          ?= gcc
CROSS_WIN64 ?= x86_64-w64-mingw32-gcc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wmissing-prototypes -Wno-unused-parameter -O2 -g
CPPFLAGS    ?= -Iinclude
LDFLAGS     ?=
PREFIX      ?= /usr/local

WIN64_CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -g
WIN64_LDFLAGS ?= -static

BUILD_DIR   ?= build
LINUX_DIR   = $(BUILD_DIR)/linux
WIN64_DIR   = $(BUILD_DIR)/win64

HAVE_WIN64 := $(shell which $(CROSS_WIN64) 2>/dev/null)
HAVE_WINE  := $(shell which wine 2>/dev/null)

HDRS = include/ace.h include/ace/*.h

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

APP_SRCS = \
	src/ace.c \
	src/mkace.c \
	src/unace.c \
	$(LIB_SRCS)

LINUX_OBJS      = $(patsubst src/%.c,$(LINUX_DIR)/%.o,$(APP_SRCS))
LINUX_LIB_OBJS  = $(patsubst src/%.c,$(LINUX_DIR)/%.o,$(LIB_SRCS))
WIN64_OBJS      = $(patsubst src/%.c,$(WIN64_DIR)/%.o,$(APP_SRCS))

LINUX_BIN  = $(LINUX_DIR)/ace
LINUX_TEST = $(LINUX_DIR)/test_core
WIN64_BIN  = $(WIN64_DIR)/ace.exe

.PHONY: all linux win64 clean test test-win64 update-sfx

all: linux $(if $(HAVE_WIN64),win64)

linux: $(LINUX_BIN) $(LINUX_TEST) ace

win64: $(WIN64_BIN)

# Convenience root symlink pointing to the Linux executable
ace: $(LINUX_BIN)
	ln -sf $(LINUX_BIN) $@

# Linux build rules
$(LINUX_DIR):
	mkdir -p $(LINUX_DIR)

$(LINUX_DIR)/%.o: src/%.c $(HDRS) | $(LINUX_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LINUX_DIR)/test_core.o: tests/test_core.c $(HDRS) | $(LINUX_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LINUX_BIN): $(LINUX_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(LINUX_TEST): $(LINUX_DIR)/test_core.o $(LINUX_LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Win64 build rules
$(WIN64_DIR):
	mkdir -p $(WIN64_DIR)

$(WIN64_DIR)/%.o: src/%.c $(HDRS) | $(WIN64_DIR)
	$(CROSS_WIN64) $(CPPFLAGS) $(WIN64_CFLAGS) -c -o $@ $<

$(WIN64_BIN): $(WIN64_OBJS)
	$(CROSS_WIN64) $(WIN64_CFLAGS) -o $@ $^ $(WIN64_LDFLAGS)

test: linux
	$(LINUX_TEST)
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

test-win64: win64
	@if [ -z "$(HAVE_WINE)" ]; then \
		echo "wine is not installed, skipping win64 runtime test"; \
	else \
		echo "Running Win64 test via wine..."; \
		wine $(WIN64_BIN) h > /dev/null && echo "wine $(WIN64_BIN) h: OK"; \
		wine $(WIN64_BIN) a testdata/win_test.ace testdata/hello.txt > /dev/null && \
		wine $(WIN64_BIN) t testdata/win_test.ace > /dev/null && \
		echo "wine $(WIN64_BIN) a/t roundtrip: OK"; \
		rm -f testdata/win_test.ace; \
	fi

clean:
	rm -rf $(BUILD_DIR) ace mkace unace tests/test_core src/*.o tests/*.o
	rm -rf testdata/hello.ace testdata/roundtrip.ace testdata/lz77.ace testdata/blocked.ace testdata/rt_* testdata/_* testdata/real_corpus testdata/win_test.ace

# Rebuild embedded SFX stubs (DOS 32-bit, Win32 CL, Win32 GUI, Linux 64-bit ELF)
update-sfx:
	python3 tools/gen_sfx_stubs.py
