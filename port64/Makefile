# AArch64 (64-bit) port of the Minecraft Bedrock 26.x loader. Stage 1: loader + Bionic shim + load test.
#   cross build on a PC:  make CROSS=aarch64-linux-gnu-
#   native on the R36S:   make
CROSS ?=
CC    := $(CROSS)gcc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function -Isrc -U_FORTIFY_SOURCE
B := build

all: $(B)/loadtest
$(B):
	mkdir -p $@
$(B)/loadtest: src/loadtest.c src/elf64_loader.c src/bionic64.c src/elf64_loader.h | $(B)
	$(CC) $(CFLAGS) -rdynamic -o $@ src/loadtest.c src/elf64_loader.c src/bionic64.c -ldl -lpthread -lm
clean:
	rm -rf $(B)
