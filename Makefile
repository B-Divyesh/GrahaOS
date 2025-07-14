# Makefile for GrahaOS (Phase 6c - Interactive Shell)

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
# ADDED: Include path for the new keyboard driver
CFLAGS   := -I. -I./arch/x86_64/drivers/keyboard -ffreestanding -fno-stack-protector -fno-pie \
            -mno-red-zone -mcmodel=kernel -g -Wall -Wextra \
            -std=gnu11 -fno-stack-check -fno-PIC -m64  \
            -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2

CPPFLAGS := -DLIMINE_API_REVISION=3 -MMD -MP
NASMFLAGS := -f elf64 -g -F dwarf
LDFLAGS  := -T linker.ld -nostdlib -static -z max-page-size=0x1000 \
            --build-id=none

# --- Source Files ---
# MODIFIED: Automatically find the new keyboard.c
C_SOURCES := $(shell find kernel drivers arch -name "*.c" -type f | sort -u)
ASM_SOURCES := $(shell find arch -name "*.S" -type f | sort -u)

C_OBJECTS   := $(C_SOURCES:.c=.o)
ASM_OBJECTS := $(ASM_SOURCES:.S=.asm.o)
OBJECTS     := $(C_OBJECTS) $(ASM_OBJECTS)
DEPS := $(C_OBJECTS:.o=.d)

# --- Build Targets ---
.PHONY: all clean run help debug info userland

all: grahaos.iso

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

# MODIFIED: Package gash into the initrd
initrd.tar: userland etc/motd.txt etc/plan.json
	@echo "Creating initrd..."
	@rm -rf initrd_root
	@mkdir -p initrd_root/bin initrd_root/etc
	@if [ ! -f user/grahai ]; then \
		echo "ERROR: user/grahai not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/gash ]; then \
		echo "ERROR: user/gash not found!"; \
		exit 1; \
	fi
	@cp user/grahai initrd_root/bin/
	@cp user/gash initrd_root/bin/
	@cp etc/motd.txt initrd_root/etc/
	@cp etc/plan.json initrd_root/etc/
	@echo "Contents of initrd_root before tar:"
	@find initrd_root -type f -ls
	@$(TAR) -cf initrd.tar -C initrd_root bin etc
	@echo "Verifying tar contents:"
	@$(TAR) -tf initrd.tar
	@rm -rf initrd_root
	@echo "initrd.tar created successfully with gash and grahai"

userland:
	@echo "Building user programs..."
	@$(MAKE) -C user PREFIX=$(PREFIX) TARGET=$(TARGET)

kernel/kernel.elf: $(OBJECTS) linker.ld
	@echo "Linking kernel with LD..."
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	@echo "Compiling C: $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.asm.o: %.S
	@echo "Assembling with NASM: $<"
	@mkdir -p $(dir $@)
	@$(NASM) $(NASMFLAGS) $< -o $@

-include $(DEPS)

run: grahaos.iso
	@echo "Starting QEMU..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M

debug: grahaos.iso
	@echo "Starting QEMU with GDB support..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -s -S

clean:
	@echo "Cleaning up..."
	@rm -rf $(C_OBJECTS) $(ASM_OBJECTS) $(DEPS) kernel/kernel.elf grahaos.iso iso_root initrd.tar initrd_root
	@$(MAKE) -C user clean

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

help:
	@echo "Available targets:"
	@echo "  all      - Build the OS ISO with initrd (default)"
	@echo "  userland - Build user-space programs"
	@echo "  run      - Build and run in QEMU"
	@echo "  debug    - Build and run in QEMU with GDB support"
	@echo "  clean    - Clean build artifacts"
	@echo "  info     - Show build information"
	@echo "  help     - Show this help message"