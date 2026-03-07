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

# Build all user programs
PROGRAMS := grahai gash libctest sbrk_test printf_test spawntest cantest metatest eventtest nettest httptest dnstest aitest fdtest pipetest simtest clustertest

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

# Build gash shell (with JSON parser for AI command)
gash: gash.o json.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ gash.o json.o $(LIBC)

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

# Build spawn test (Phase 7d, with libc)
spawntest: spawntest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ spawntest.o $(LIBC)

# Build CAN test (Phase 8b-ii, with libc)
cantest: cantest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ cantest.o $(LIBC)

# Build AI Metadata test (Phase 8c, with libc)
metatest: metatest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ metatest.o $(LIBC)

# Build CAN Event test (Phase 8d, with libc)
eventtest: eventtest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ eventtest.o $(LIBC)

# Build E1000 NIC test (Phase 9a, with libc)
nettest: nettest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ nettest.o $(LIBC)

# Build HTTP/TCP test (Phase 9b, with libc)
httptest: httptest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ httptest.o $(LIBC)

# Build DNS/HTTP client test (Phase 9c, with libc)
dnstest: dnstest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ dnstest.o $(LIBC)

# Build AI integration test (Phase 9e, with libc)
aitest: aitest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ aitest.o $(LIBC)

# Build FD table test (Phase 10a, no libc needed)
fdtest: fdtest.o
	@echo "Linking user program: $@ (no libc)"
	@$(LD) $(LDFLAGS) -o $@ fdtest.o

# Build pipe test (Phase 10b, no libc needed)
pipetest: pipetest.o
	@echo "Linking user program: $@ (no libc)"
	@$(LD) $(LDFLAGS) -o $@ pipetest.o

# Build SimHash test (Phase 11a, with libc)
simtest: simtest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ simtest.o $(LIBC)

# Build Cluster test (Phase 11b, with libc)
clustertest: clustertest.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ clustertest.o $(LIBC)

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f *.o $(PROGRAMS)