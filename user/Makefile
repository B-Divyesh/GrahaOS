# user/Makefile
PREFIX := $(or $(PREFIX),/home/atman/GrahaOS/toolchain)
TARGET := $(or $(TARGET),x86_64-elf)
CC     := $(PREFIX)/bin/$(TARGET)-gcc
LD     := $(PREFIX)/bin/$(TARGET)-ld

# CRITICAL: Use flags that prevent problematic optimizations
CFLAGS := -ffreestanding -fno-stack-protector -fno-pie -g -Wall -Wextra \
          -std=gnu11 -fno-stack-check -fno-PIC -m64 -march=x86-64 \
          -mno-red-zone -fno-builtin -fomit-frame-pointer \
          -O0  # CRITICAL: No optimization to ensure predictable code

LDFLAGS := -nostdlib -static -T linker.ld

.PHONY: all clean

all: grahai

grahai: grahai.c linker.ld
	@echo "  CC      user/grahai.c"
	@$(CC) $(CFLAGS) -c grahai.c -o grahai.o
	@echo "  LD      grahai"
	@$(LD) $(LDFLAGS) -o grahai grahai.o

clean:
	@echo "  CLEAN   user/"
	@rm -f grahai grahai.o