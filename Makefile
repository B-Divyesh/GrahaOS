# Makefile for GrahaOS (Phase 6c - Interactive Shell)

# --- Configuration ---
PREFIX   := /home/atman/GrahaOS/toolchain
TARGET   := x86_64-elf

# Tools
HOST_CC  := gcc
CC       := $(PREFIX)/bin/$(TARGET)-gcc
LD       := $(PREFIX)/bin/$(TARGET)-ld
AS       := $(PREFIX)/bin/$(TARGET)-as
NASM     := nasm
TAR      := tar
LIMINE_DIR := limine

# --- Flags ---
# ADDED: Include path for the new keyboard driver
CFLAGS   := -I. -I./arch/x86_64/drivers/keyboard -I./arch/x86_64/drivers/lapic_timer -I./kernel/net \
            -ffreestanding -fno-stack-protector -fno-pie \
            -mno-red-zone -mcmodel=kernel -g -Wall -Wextra \
            -std=gnu11 -fno-stack-check -fno-PIC -m64  \
            -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2

BUILD_SHA := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
# Phase 13: WITH_DEBUG_SYSCALL gates SYS_DEBUG=1056, which is the
# controlled-panic trigger used by gate tests (panic_test, kernel_pf
# fault injection). On by default during Phase 13; flipped off via
# the exit-criteria build at Phase 13 close so release builds cannot
# userspace-trigger a panic.
WITH_DEBUG_SYSCALL ?= 1
CPPFLAGS := -DLIMINE_API_REVISION=3 -DGRAHAOS_BUILD_SHA=\"$(BUILD_SHA)\" -MMD -MP
ifeq ($(WITH_DEBUG_SYSCALL),1)
CPPFLAGS += -DWITH_DEBUG_SYSCALL=1
endif
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
.PHONY: all clean run help debug info userland test test-sentinel-meta qemu-interactive test-panic test-kernel-pf test-fault-injection

all: grahaos.iso format-disk-if-needed

# Phase 12 test harness — boots QEMU headless with autorun=ktest,
# captures serial, parses TAP, writes summary.json. Exits 0 iff every
# gate test passed.
test:
	@scripts/run_tests.sh

# Prove the harness catches failures. Runs the suite with sentinel_fail
# (a deliberately-failing test) linked in. Expected to exit non-zero.
# The whole pipeline (user build → initrd manifest → ktest → parse_tap.py)
# is re-run with WITH_SENTINEL_FAIL=1. This target SHOULD exit non-zero;
# if it exits 0, the harness is broken and must be fixed before Phase 13.
test-sentinel-meta:
	@WITH_SENTINEL_FAIL=1 $(MAKE) --no-print-directory -C user clean >/dev/null 2>&1; true
	@WITH_SENTINEL_FAIL=1 scripts/run_tests.sh; ec=$$?; \
	 if [ $$ec -eq 0 ]; then \
	    echo "test-sentinel-meta: ERROR — expected non-zero exit, got 0. Harness is broken."; \
	    exit 2; \
	 else \
	    echo "test-sentinel-meta: OK — harness detected the deliberate failure (exit=$$ec)"; \
	    exit 0; \
	 fi

# Phase 13 oops gate test: spawn /bin/panic_test as PID 1, expect a
# parseable ==OOPS== block on serial. Exits 0 iff parse_oops.py is
# happy with the output.
test-panic:
	@scripts/run_panic_test.sh panic_test 1

# Phase 13 kernel page fault gate test: spawn /bin/kpf_test as PID 1,
# which dereferences an unmapped kernel address via SYS_DEBUG. The
# page-fault handler in interrupts.c routes through kpanic_at, so we
# expect an oops with reason ~ "page fault at 0x...".
test-kernel-pf:
	@scripts/run_panic_test.sh kpf_test 1

# Phase 13 fault-injection gate tests: pre-init klog drop + ring wrap.
# Each scenario boots QEMU with a dedicated cmdline knob and greps
# the serial log for the breadcrumb the kernel emits.
test-fault-injection:
	@scripts/run_fault_injection.sh

# Boot the production ISO interactively for manual debugging.
qemu-interactive: all
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
		-drive file=disk.img,format=raw,if=none,id=mydisk \
		-device ich9-ahci,id=ahci \
		-device ide-hd,drive=mydisk,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0

# Build the host formatting tool
scripts/mkfs.gfs: scripts/mkfs.gfs.c kernel/fs/grahafs.h
	@echo "Building host tool: mkfs.gfs..."
	@$(HOST_CC) -o $@ $< -I.

# Format the disk image with GrahaFS
format-disk: scripts/mkfs.gfs disk.img
	@echo "Formatting disk.img with GrahaFS..."
	@./scripts/mkfs.gfs disk.img
	@touch .disk_formatted

# Verify disk has valid GrahaFS magic, format if not
format-disk-if-needed: scripts/mkfs.gfs
	@if [ ! -f disk.img ]; then \
		echo "No disk.img found, creating and formatting..."; \
		$(MAKE) disk.img; \
		./scripts/mkfs.gfs disk.img; \
		touch .disk_formatted; \
	elif ! xxd -l 8 -p disk.img 2>/dev/null | grep -qi "21534f4148415247"; then \
		echo "Disk exists but has no valid GrahaFS magic, formatting..."; \
		./scripts/mkfs.gfs disk.img; \
		touch .disk_formatted; \
	else \
		echo "Using existing formatted disk.img (GrahaFS magic verified)"; \
	fi

disk.img:
	@echo "Creating virtual hard disk..."
	@dd if=/dev/zero of=disk.img bs=1M count=128 2>/dev/null
	@echo "Virtual disk created: disk.img (128MB)"

grahaos.iso: kernel/kernel.elf initrd.tar limine.conf disk.img
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

# MODIFIED: Package gash, grahai, and libctest into the initrd (Phase 7c)
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
	@if [ ! -f user/libctest ]; then \
		echo "ERROR: user/libctest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/sbrk_test ]; then \
		echo "ERROR: user/sbrk_test not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/printf_test ]; then \
		echo "ERROR: user/printf_test not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/spawntest ]; then \
		echo "ERROR: user/spawntest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/cantest ]; then \
		echo "ERROR: user/cantest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/metatest ]; then \
		echo "ERROR: user/metatest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/eventtest ]; then \
		echo "ERROR: user/eventtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/nettest ]; then \
		echo "ERROR: user/nettest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/httptest ]; then \
		echo "ERROR: user/httptest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/dnstest ]; then \
		echo "ERROR: user/dnstest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/aitest ]; then \
		echo "ERROR: user/aitest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/fdtest ]; then \
		echo "ERROR: user/fdtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/pipetest ]; then \
		echo "ERROR: user/pipetest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/simtest ]; then \
		echo "ERROR: user/simtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/clustertest ]; then \
		echo "ERROR: user/clustertest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/libtap_selftest ]; then \
		echo "ERROR: user/tests/libtap_selftest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/sentinel_pass ]; then \
		echo "ERROR: user/tests/sentinel_pass not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/libctest ]; then \
		echo "ERROR: user/tests/libctest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/simtest ]; then \
		echo "ERROR: user/tests/simtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/clustertest ]; then \
		echo "ERROR: user/tests/clustertest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/pipetest ]; then \
		echo "ERROR: user/tests/pipetest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/fdtest ]; then \
		echo "ERROR: user/tests/fdtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/metatest ]; then \
		echo "ERROR: user/tests/metatest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/spawntest ]; then \
		echo "ERROR: user/tests/spawntest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/cantest ]; then \
		echo "ERROR: user/tests/cantest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/eventtest ]; then \
		echo "ERROR: user/tests/eventtest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/nettest ]; then \
		echo "ERROR: user/tests/nettest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/httptest ]; then \
		echo "ERROR: user/tests/httptest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/dnstest ]; then \
		echo "ERROR: user/tests/dnstest not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/ktest_discovery ]; then \
		echo "ERROR: user/tests/ktest_discovery not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/ktest_capture ]; then \
		echo "ERROR: user/tests/ktest_capture not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/tests/cmdline_parse ]; then \
		echo "ERROR: user/tests/cmdline_parse not found!"; \
		exit 1; \
	fi
	@if [ ! -f user/ktest ]; then \
		echo "ERROR: user/ktest not found!"; \
		exit 1; \
	fi
	@cp user/grahai initrd_root/bin/
	@cp user/gash initrd_root/bin/
	@cp user/libctest initrd_root/bin/
	@cp user/sbrk_test initrd_root/bin/
	@cp user/printf_test initrd_root/bin/
	@cp user/spawntest initrd_root/bin/
	@cp user/cantest initrd_root/bin/
	@cp user/metatest initrd_root/bin/
	@cp user/eventtest initrd_root/bin/
	@cp user/nettest initrd_root/bin/
	@cp user/httptest initrd_root/bin/
	@cp user/dnstest initrd_root/bin/
	@cp user/aitest initrd_root/bin/
	@cp user/fdtest initrd_root/bin/
	@cp user/pipetest initrd_root/bin/
	@cp user/simtest initrd_root/bin/
	@cp user/clustertest initrd_root/bin/
	@mkdir -p initrd_root/bin/tests
	@cp user/tests/libtap_selftest initrd_root/bin/tests/libtap_selftest.tap
	@cp user/tests/sentinel_pass    initrd_root/bin/tests/sentinel_pass.tap
	@cp user/tests/libctest         initrd_root/bin/tests/libctest.tap
	@cp user/tests/simtest          initrd_root/bin/tests/simtest.tap
	@cp user/tests/clustertest      initrd_root/bin/tests/clustertest.tap
	@cp user/tests/pipetest         initrd_root/bin/tests/pipetest.tap
	@cp user/tests/fdtest           initrd_root/bin/tests/fdtest.tap
	@cp user/tests/metatest         initrd_root/bin/tests/metatest.tap
	@cp user/tests/spawntest        initrd_root/bin/tests/spawntest.tap
	@cp user/tests/cantest          initrd_root/bin/tests/cantest.tap
	@cp user/tests/eventtest        initrd_root/bin/tests/eventtest.tap
	@cp user/tests/nettest          initrd_root/bin/tests/nettest.tap
	@cp user/tests/httptest         initrd_root/bin/tests/httptest.tap
	@cp user/tests/dnstest          initrd_root/bin/tests/dnstest.tap
	@cp user/tests/ktest_discovery  initrd_root/bin/tests/ktest_discovery.tap
	@cp user/tests/ktest_capture    initrd_root/bin/tests/ktest_capture.tap
	@cp user/tests/cmdline_parse    initrd_root/bin/tests/cmdline_parse.tap
	@cp user/tests/klog_basic       initrd_root/bin/tests/klog_basic.tap
	@cp user/tests/klog_stress      initrd_root/bin/tests/klog_stress.tap
	@# Phase 13: panic_test is NOT a TAP test — it intentionally panics
	@# the kernel. Copied to bin/ so autorun=panic_test can spawn it.
	@cp user/tests/panic_test       initrd_root/bin/panic_test
	@cp user/tests/kpf_test         initrd_root/bin/kpf_test
	@cp user/ktest                   initrd_root/bin/
	@# Phase 13: standalone klog reader.
	@cp user/klog                    initrd_root/bin/klog
	@# Phase 12: test manifest — one name per line, loaded by ktest.
	@# Keep in sync with TAP_TESTS in user/Makefile.
	@echo "# GrahaOS TAP test manifest (Phase 12) — one test name per line" > initrd_root/bin/tests/manifest.txt
	@echo "sentinel_pass" >> initrd_root/bin/tests/manifest.txt
	@echo "libtap_selftest" >> initrd_root/bin/tests/manifest.txt
	@echo "libctest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 12: clustertest MUST run before simtest. clustertest's
	@# Test 2 asserts a new cluster is created when cl_alpha gets
	@# SimHashed; if simtest already populated the cluster table with
	@# nearby leaders, cl_alpha is absorbed into an existing cluster.
	@# Fresh disk (reformat in run_tests.sh) keeps simtest's Test 8
	@# ("find_similar on file without SimHash returns -2") valid in
	@# this reordered layout.
	@echo "clustertest" >> initrd_root/bin/tests/manifest.txt
	@echo "simtest" >> initrd_root/bin/tests/manifest.txt
	@echo "pipetest" >> initrd_root/bin/tests/manifest.txt
	@echo "fdtest" >> initrd_root/bin/tests/manifest.txt
	@echo "metatest" >> initrd_root/bin/tests/manifest.txt
	@echo "spawntest" >> initrd_root/bin/tests/manifest.txt
	@echo "cantest" >> initrd_root/bin/tests/manifest.txt
	@echo "eventtest" >> initrd_root/bin/tests/manifest.txt
	@echo "nettest" >> initrd_root/bin/tests/manifest.txt
	@echo "httptest" >> initrd_root/bin/tests/manifest.txt
	@echo "dnstest" >> initrd_root/bin/tests/manifest.txt
	@echo "ktest_discovery" >> initrd_root/bin/tests/manifest.txt
	@echo "ktest_capture" >> initrd_root/bin/tests/manifest.txt
	@echo "cmdline_parse" >> initrd_root/bin/tests/manifest.txt
	@# Phase 13: klog syscalls round-trip + stress tests.
	@echo "klog_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "klog_stress" >> initrd_root/bin/tests/manifest.txt
	@# Work unit 15: sentinel_fail is included ONLY when invoked via
	@# `make test-sentinel-meta` (which sets WITH_SENTINEL_FAIL=1).
	@if [ "$$WITH_SENTINEL_FAIL" = "1" ]; then \
		if [ ! -f user/tests/sentinel_fail ]; then \
			echo "ERROR: user/tests/sentinel_fail not built"; exit 1; \
		fi; \
		cp user/tests/sentinel_fail initrd_root/bin/tests/sentinel_fail.tap; \
		echo "sentinel_fail" >> initrd_root/bin/tests/manifest.txt; \
		echo "  → sentinel_fail included (meta-test mode)"; \
	fi
	@cp etc/motd.txt initrd_root/etc/
	@cp etc/plan.json initrd_root/etc/
	@if [ -f api_keys.md ]; then \
		grep '^GEMINI_API_KEY=' api_keys.md | sed 's/^GEMINI_API_KEY=//' > initrd_root/etc/ai.conf; \
	else echo "" > initrd_root/etc/ai.conf; fi
	@echo "Contents of initrd_root before tar:"
	@find initrd_root -type f -ls
	@$(TAR) -cf initrd.tar -C initrd_root bin bin/tests etc
	@echo "Verifying tar contents:"
	@$(TAR) -tf initrd.tar
	@rm -rf initrd_root
	@echo "initrd.tar created successfully with gash, grahai, libctest, and spawntest"

userland:
	@echo "Building user programs..."
	@$(MAKE) -C user PREFIX=$(PREFIX) TARGET=$(TARGET)

kernel/kernel.elf: $(OBJECTS) linker.ld
	@echo "Linking kernel with LD..."
	@$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

# Mongoose: compile with warnings suppressed and SSE2 enabled (has float code)
kernel/net/mongoose.o: kernel/net/mongoose.c
	@echo "Compiling C: $< (vendored, warnings suppressed)"
	@mkdir -p $(dir $@)
	@$(CC) $(filter-out -mno-80387 -mno-mmx -mno-sse -mno-sse2,$(CFLAGS)) $(CPPFLAGS) -w -c $< -o $@

%.o: %.c
	@echo "Compiling C: $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.asm.o: %.S
	@echo "Assembling with NASM: $<"
	@mkdir -p $(dir $@)
	@$(NASM) $(NASMFLAGS) $< -o $@

-include $(DEPS)

run: grahaos.iso format-disk-if-needed
	@echo "Starting QEMU with persistent disk..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
	     -drive file=disk.img,format=raw,if=none,id=mydisk \
	     -device ich9-ahci,id=ahci \
	     -device ide-hd,drive=mydisk,bus=ahci.0 \
	     -netdev user,id=net0,hostfwd=tcp::8080-:80 -device e1000,netdev=net0 \
	     -d int,cpu_reset -D qemu.log

debug: grahaos.iso format-disk-if-needed
	@echo "Starting QEMU with GDB support..."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 -s -S \
	     -drive file=disk.img,format=raw,if=none,id=mydisk \
	     -device ich9-ahci,id=ahci \
	     -device ide-hd,drive=mydisk,bus=ahci.0 \
	     -netdev user,id=net0,hostfwd=tcp::8080-:80 -device e1000,netdev=net0


debug-monitor: grahaos.iso format-disk
	@echo "Starting QEMU with monitor..."
	@echo "Press Ctrl+Alt+2 for QEMU monitor"
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
	    -monitor stdio -d int,cpu_reset -D qemu.log \
	    -drive file=disk.img,format=raw,if=none,id=mydisk \
	    -device ich9-ahci,id=ahci \
	    -device ide-hd,drive=mydisk,bus=ahci.0

clean:
	@echo "Cleaning up..."
	@rm -rf $(C_OBJECTS) $(ASM_OBJECTS) $(DEPS) kernel/kernel.elf grahaos.iso iso_root initrd.tar initrd_root disk.img scripts/mkfs.gfs .disk_formatted
	@$(MAKE) -C user clean

reformat: scripts/mkfs.gfs
	@rm -f disk.img .disk_formatted
	@$(MAKE) disk.img
	@$(MAKE) format-disk

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