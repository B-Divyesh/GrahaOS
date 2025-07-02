# Makefile for GrahaOS (Phase 6a - Filesystem Syscalls)

# --- Configuration ---
PREFIX   := /home/atman/GrahaOS/toolchain
TARGET   := x86_64-elf

# Tools
CC       := $(PREFIX)/bin/$(TARGET)-gcc
LD       := $(PREFIX)/bin/$(TARGET)-ld
AS       := $(PREFIX)/bin/$(TARGET)-as
NASM     := nasm
TAR      := tar
LIMINE_DIR := limine

# --- Flags ---
# Compiler flags for kernel development
CFLAGS   := -I. -ffreestanding -fno-stack-protector -fno-pie \
            -mno-red-zone -mcmodel=kernel -g -Wall -Wextra \
            -std=gnu11 -fno-stack-check -fno-PIC -m64 \
            -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2

# Preprocessor flags
CPPFLAGS := -DLIMINE_API_REVISION=3 -MMD -MP

# NASM flags for 64-bit ELF
NASMFLAGS := -f elf64 -g -F dwarf

# Linker flags (for direct ld usage)
LDFLAGS  := -T linker.ld -nostdlib -static -z max-page-size=0x1000 \
            --build-id=none

# --- Source Files ---
# MODIFIED: Added kernel/fs to the search path
C_SOURCES := $(shell find kernel drivers arch -name "*.c" -type f | sort -u)

# Find all assembly sources in arch directory (.S files for NASM)
ASM_SOURCES := $(shell find arch -name "*.S" -type f | sort -u)

# Object files with unique names to avoid conflicts
C_OBJECTS   := $(C_SOURCES:.c=.o)
ASM_OBJECTS := $(ASM_SOURCES:.S=.asm.o)

# Combine all objects
OBJECTS     := $(C_OBJECTS) $(ASM_OBJECTS)

# Dependency files
DEPS := $(C_OBJECTS:.o=.d)

# --- Build Targets ---
.PHONY: all clean run help debug info userland

# Default target - now depends on initrd
all: grahaos.iso

# Create the bootable ISO image
grahaos.iso: kernel/kernel.elf initrd.tar limine.conf
	@echo "Creating ISO image..."
	@rm -rf iso_root
	@mkdir -p iso_root/boot
	@cp kernel/kernel.elf iso_root/boot/
	@cp initrd.tar iso_root/boot/
	@cp limine.conf iso_root/
	@cp $(LIMINE_DIR)/limine-bios.sys iso_root/
	@cp $(LIMINE_DIR)/limine-bios-cd.bin iso_root/
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/
	@mkdir -p iso_root/EFI/BOOT
	@cp $(LIMINE_DIR)/BOOTX64.EFI iso_root/EFI/BOOT/
	@cp $(LIMINE_DIR)/BOOTIA32.EFI iso_root/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J -b limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o grahaos.iso
	@$(LIMINE_DIR)/limine bios-install grahaos.iso
	@rm -rf iso_root
	@echo "Build complete: grahaos.iso"

# MODIFIED: Recipe to create the initrd with the new structure
initrd.tar: userland etc/motd.txt
	@echo "Creating initrd..."
	@rm -rf initrd_root
	@mkdir -p initrd_root/bin
	@mkdir -p initrd_root/etc
	@cp user/grahai initrd_root/bin/
	@cp etc/motd.txt initrd_root/etc/
	@$(TAR) -cf initrd.tar -C initrd_root bin etc
	@rm -rf initrd_root
	@echo "initrd.tar created successfully"

# Build user-space programs
userland:
	@echo "Building user programs..."
	@$(MAKE) -C user PREFIX=$(PREFIX) TARGET=$(TARGET)

# Link the kernel using LD directly (as requested)
kernel/kernel.elf: $(OBJECTS) linker.ld
	@echo "Linking kernel with LD..."
	@echo "Objects to link: $(words $(OBJECTS)) files"
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Compile C source files
%.o: %.c
	@echo "Compiling C: $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Assemble assembly files using NASM (Intel syntax)
%.asm.o: %.S
	@echo "Assembling with NASM: $<"
	@mkdir -p $(dir $@)
	@$(NASM) $(NASMFLAGS) $< -o $@

# Include dependency files
-include $(DEPS)

# Run the OS in QEMU
run: grahaos.iso
	@echo "Starting QEMU..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M

# Debug version with GDB support
debug: grahaos.iso
	@echo "Starting QEMU with GDB support..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -s -S

# Clean up build artifacts (updated to include user programs and initrd)
clean:
	@echo "Cleaning up..."
	@rm -rf $(C_OBJECTS) $(ASM_OBJECTS) $(DEPS) kernel/kernel.elf grahaos.iso iso_root initrd.tar initrd_root
	@$(MAKE) -C user clean

# Show detailed build information
info:
	@echo "=== Build Information ==="
	@echo "C Sources found: $(words $(C_SOURCES)) files"
	@echo "ASM Sources found: $(words $(ASM_SOURCES)) files"
	@echo "Total Objects: $(words $(OBJECTS)) files"
	@echo ""
	@echo "C Sources:"
	@for src in $(C_SOURCES); do echo "  $$src"; done
	@echo ""
	@echo "ASM Sources:"
	@for src in $(ASM_SOURCES); do echo "  $$src"; done
	@echo ""
	@echo "Objects to be created:"
	@for obj in $(OBJECTS); do echo "  $$obj"; done
	@echo ""
	@echo "=== File Check ==="
	@echo "Directory structure:"
	@find arch kernel drivers -type f 2>/dev/null || echo "core directories not found"

# Help target
help:
	@echo "Available targets:"
	@echo "  all      - Build the OS ISO with initrd (default)"
	@echo "  userland - Build user-space programs"
	@echo "  run      - Build and run in QEMU"
	@echo "  debug    - Build and run in QEMU with GDB support"
	@echo "  clean    - Clean build artifacts"
	@echo "  info     - Show build information"
	@echo "  help     - Show this help message"