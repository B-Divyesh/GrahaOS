# Makefile for GrahaOS Userland Programs (Phase 6b)

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

# MODIFIED: Add json.c to the sources
SOURCES := grahai.c json.c
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean

all: grahai

grahai: $(OBJECTS) linker.ld
	@echo "Linking user program: $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f $(OBJECTS) grahai