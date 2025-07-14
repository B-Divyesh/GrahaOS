# Makefile for GrahaOS Userland Programs (Phase 6c)

# Inherit from parent Makefile
PREFIX ?= /home/atman/GrahaOS/toolchain
TARGET ?= x86_64-elf

# Tools
CC := $(PREFIX)/bin/$(TARGET)-gcc
LD := $(PREFIX)/bin/$(TARGET)-ld

# Flags
# NOTE: We are building for user-space, so we don't need kernel-specific flags.
# -I../ is added to find kernel headers like gcp.h
CFLAGS := -I. -I../ -ffreestanding -fno-stack-protector -fpie -g \
          -Wall -Wextra -std=gnu11 -fno-stack-check -m64 \
          -march=x86-64 -mno-red-zone -fno-builtin -fomit-frame-pointer \
          -O0

LDFLAGS := -T linker.ld -nostdlib -static --build-id=none

# MODIFIED: Build both grahai and gash
PROGRAMS := grahai gash

.PHONY: all clean

all: $(PROGRAMS)

# Build grahai with its JSON parser
grahai: grahai.o json.o
	@echo "Linking user program: $@"
	@$(LD) $(LDFLAGS) -o $@ $^

# Build gash shell
gash: gash.o
	@echo "Linking user program: $@"
	@$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f *.o $(PROGRAMS)