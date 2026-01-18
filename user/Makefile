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
# -I../libc/include is added to find libc headers (Phase 7c)
# Include GCC builtin headers (for stdarg.h, etc.)
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)
CFLAGS := -I. -I../ -I../libc/include -isystem $(GCC_INCLUDE) \
          -ffreestanding -fno-stack-protector -fpie -g \
          -Wall -Wextra -std=gnu11 -fno-stack-check -m64 \
          -march=x86-64 -mno-red-zone -fno-builtin -fomit-frame-pointer \
          -O0 -nostdinc

LDFLAGS := -T linker.ld -nostdlib -static --build-id=none

# Phase 7c: Link against libc
LIBC := ../libc/libc.a

# MODIFIED: Build grahai, gash, libctest, sbrk_test, and printf_test
PROGRAMS := grahai gash libctest sbrk_test printf_test

.PHONY: all clean libc

all: libc $(PROGRAMS)

# Build libc first
libc:
	@echo "Building libc..."
	@$(MAKE) -C ../libc

# Build grahai with its JSON parser
grahai: grahai.o json.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ grahai.o json.o $(LIBC)

# Build gash shell
gash: gash.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ gash.o $(LIBC)

# Build libc test program
libctest: libctest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ libctest.o $(LIBC)

# Build sbrk test (minimal, no libc)
sbrk_test: sbrk_test.o
	@echo "Linking user program: $@ (no libc)"
	@$(LD) $(LDFLAGS) -o $@ sbrk_test.o

# Build printf test (with libc)
printf_test: printf_test.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ printf_test.o $(LIBC)

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f *.o $(PROGRAMS)