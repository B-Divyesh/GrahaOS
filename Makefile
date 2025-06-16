# Makefile for GrahaOS (Phase 1 - Graphics Library)

# --- Configuration ---
PREFIX   := /home/atman/GrahaOS/toolchain
TARGET   := x86_64-elf

# Tools
CC       := $(PREFIX)/bin/$(TARGET)-gcc
LD       := $(PREFIX)/bin/$(TARGET)-ld
LIMINE_DIR := limine

# --- Flags ---
# Compiler flags for kernel development
CFLAGS   := -Ikernel -Idrivers -ffreestanding -fno-stack-protector -fno-pie \
            -mno-red-zone -mcmodel=kernel -g -Wall -Wextra \
            -std=gnu11 -fno-stack-check -fno-PIC -m64 \
            -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2

# Preprocessor flags
CPPFLAGS := -DLIMINE_API_REVISION=3 -MMD -MP

# Linker flags (for direct ld usage)
LDFLAGS  := -T linker.ld -nostdlib -static -z max-page-size=0x1000 \
            --build-id=none

# --- Source Files ---
# Find all C sources in kernel and drivers directories
C_SOURCES := $(wildcard kernel/*.c) $(wildcard drivers/*/*.c)
C_OBJECTS := $(C_SOURCES:.c=.o)
OBJECTS   := $(C_OBJECTS)

# Dependency files
DEPS := $(C_OBJECTS:.o=.d)

# --- Build Targets ---
.PHONY: all clean run help

# Default target
all: grahaos.iso

# Create the bootable ISO image
grahaos.iso: kernel/kernel.elf limine.conf
	@echo "Creating ISO image..."
	@rm -rf iso_root
	@mkdir -p iso_root/boot/limine
	@cp kernel/kernel.elf iso_root/boot/
	@cp limine.conf iso_root/boot/limine/
	@cp $(LIMINE_DIR)/limine-bios.sys iso_root/boot/limine/
	@cp $(LIMINE_DIR)/limine-bios-cd.bin iso_root/boot/limine/
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/boot/limine/
	@mkdir -p iso_root/EFI/BOOT
	@cp $(LIMINE_DIR)/BOOTX64.EFI iso_root/EFI/BOOT/
	@cp $(LIMINE_DIR)/BOOTIA32.EFI iso_root/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o grahaos.iso
	@$(LIMINE_DIR)/limine bios-install grahaos.iso
	@rm -rf iso_root
	@echo "Build complete: grahaos.iso"

# Link the kernel using LD directly (as requested)
kernel/kernel.elf: $(OBJECTS) linker.ld
	@echo "Linking kernel with LD..."
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Compile C source files
%.o: %.c
	@echo "Compiling: $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Include dependency files
-include $(DEPS)

# Run the OS in QEMU
run: grahaos.iso
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M

# Clean up build artifacts
clean:
	@echo "Cleaning up..."
	@rm -rf $(OBJECTS) $(DEPS) kernel/kernel.elf grahaos.iso iso_root

# Help target
help:
	@echo "Available targets:"
	@echo "  all      - Build the OS ISO (default)"
	@echo "  run      - Build and run in QEMU"
	@echo "  clean    - Clean build artifacts"
	@echo "  help     - Show this help message"
