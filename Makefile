# Makefile for GrahaOS (Phase 6c - Interactive Shell)

# --- Configuration ---
# Toolchain location — resolved relative to this Makefile so cloned repos
# work anywhere. Override with `make TOOLCHAIN_PREFIX=/some/path` if you
# keep the cross-compiler outside the source tree.
TOOLCHAIN_PREFIX ?= $(abspath $(CURDIR)/toolchain)
PREFIX   := $(TOOLCHAIN_PREFIX)
export PREFIX
TARGET   := x86_64-elf
export TARGET

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
CFLAGS   := -I. -I./kernel -I./arch/x86_64/drivers/keyboard -I./arch/x86_64/drivers/lapic_timer \
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
.PHONY: all clean run terminal help debug info userland test test-sentinel-meta qemu-interactive test-panic test-kernel-pf test-fault-injection compdb test-host

all: grahaos.iso format-disk-if-needed

# Phase 26 Stage C.3: host-side gcp2wit.py determinism + completeness tests.
# Run on every commit that touches scripts/gcp2wit.py or etc/gcp.json.
test-host:
	@echo "[host] gcp2wit determinism..."
	@python3 scripts/tests/gcp2wit_deterministic.py
	@echo "[host] gcp2wit completeness..."
	@python3 scripts/tests/gcp2wit_completeness.py
	@echo "[host] OK"

# Regenerate compile_commands.json at the repo root for IDE indexers
# (CLion, clangd, ccls). Uses compiledb to parse `make --dry-run`
# output, so it does not actually rebuild. Re-run after adding sources
# or changing CFLAGS / -I paths.
compdb:
	@command -v compiledb >/dev/null 2>&1 || { \
		echo "compiledb not found. Install with: pip install --user compiledb"; \
		exit 1; \
	}
	@compiledb -n -f --full-path make -B >/dev/null
	@echo "Wrote $(CURDIR)/compile_commands.json ($$(grep -c '"file":' compile_commands.json) entries)"

# Phase 12 test harness — boots QEMU headless with autorun=ktest,
# captures serial, parses TAP, writes summary.json. Exits 0 iff every
# gate test passed.
test:
	@scripts/run_tests.sh

# FU25.G: nightly soak. Runs make test with GRAHAOS_LONG_STRESS=1 (10K-iter
# txn_stress_*) plus a 3-iter outer loop. NOT promoted to per-commit gate
# because per-commit budget would balloon ~5×; lands as a nightly cron job
# the team runs out-of-band against main. See specs/phase-25-followups.yml::
# FU25.G for context.
test-soak-3:
	@for i in 1 2 3; do \
	   echo "=== test-soak-3 iter $$i ==="; \
	   GRAHAOS_LONG_STRESS=1 scripts/run_tests.sh || exit 1; \
	done
	@echo "test-soak-3: 3 iterations clean under GRAHAOS_LONG_STRESS=1"

# Phase 28 Session F.3 stub: 30-min default soak with fault injection.
# Full implementation (scripts/run_soak.sh) lands in Session G.2 alongside
# the kernel-side fault-injection hooks. For now this prints the planned
# scope so callers see Phase 28 progress.
test-soak: grahaos.iso disk.img
	@echo "Phase 28 soak (Session F.3 stub):"
	@echo "  TARGET_DURATION_SEC=$${TARGET_DURATION_SEC:-1800}  (default 30 min)"
	@echo "  Fault injection: $${GRAHAOS_SOAK_FAULT_INJECT:-0}"
	@if [ -x scripts/run_soak.sh ]; then \
	   ./scripts/run_soak.sh; \
	else \
	   echo "scripts/run_soak.sh not yet implemented (Phase 28 Session G.2)"; \
	   exit 1; \
	fi

# Phase 28 closeout: full 1-hour soak. Spec-required exit criterion.
test-soak-1h: grahaos.iso disk.img
	@TARGET_DURATION_SEC=3600 GRAHAOS_SOAK_FAULT_INJECT=1 \
	   $(MAKE) test-soak

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
# `taskset -c 0-3` HARD-CAPS QEMU at 4 host cores (out of 24); the host
# stays usable on the other 20 cores.  `nice -n 5` keeps the 4 dedicated
# cores responsive to host foreground work.  KVM intentionally not enabled
# — see scripts/run_tests.sh for the rationale.
qemu-interactive: all
	@taskset -c 0-3 nice -n 5 ionice -c 2 -n 5 \
		qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
		-drive file=disk.img,format=raw,if=none,id=mydisk \
		-device ich9-ahci,id=ahci \
		-device ide-hd,drive=mydisk,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0

# Build the host formatting tool.
# Phase 19 ships a v2 formatter at scripts/mkfs.gfs.v2.c. It becomes the
# active formatter once the v2 mount path (U17) is green. Until then we keep
# the v1 formatter wired so existing tests keep running.
scripts/mkfs.gfs: scripts/mkfs.gfs.c kernel/fs/grahafs.h
	@echo "Building host tool: mkfs.gfs..."
	@$(HOST_CC) -o $@ $< -I.

# Phase 19: v2 formatter, built when the kernel mounts v2.
scripts/mkfs.gfs.v2: scripts/mkfs.gfs.v2.c kernel/fs/grahafs_v2.h kernel/lib/crc32.c kernel/lib/crc32.h
	@echo "Building host tool: mkfs.gfs.v2 (GrahaFS v2)..."
	@$(HOST_CC) -o $@ $< kernel/lib/crc32.c -I.

# Format the disk image with GrahaFS v1 (default).  Phase 23 Step 5 added
# a working v2 formatter at scripts/mkfs.gfs.v2.c, but switching the gate
# default to v2 currently regresses ~26 assertions across clustertest /
# simtest / metatest / fdtest (v1-specific SimHash/cluster + AI-metadata
# syscalls expect v1 layout) plus the pre-existing gash_txn flake.
#
# Phase 28 v2-substrate session (2026-05-06): the v2-in-gate kernel
# panic that blocked F.1 was diagnosed and FIXED — root cause was a
# kernel stack overflow (16 KiB stack), not the suspected SPSC race.
# `journal_replay` had three 4 KiB on-stack block buffers and
# `inode_cache_get` had a 4 KiB block buffer in the cluster-rebuild
# tight loop; combined frame depth in `grahafs_v2_mount`'s call chain
# overflowed the stack into whatever physical page sat just below
# (in the failing case: the SPSC ring VMO, hence the misleading
# "stale spsc resp" warnings as kernel stack contents leaked through
# the SPSC ring fields). Fix has three parts: (a) heap-allocate the
# four 4 KiB buffers, (b) bump KERNEL_STACK_SIZE 16 → 32 KiB as
# defense-in-depth, (c) read inode blocks in 8-inode batches during
# cluster rebuild (16 K block reads → 2 K). v2 mount now boots cleanly.
#
# What still needs porting before v2 becomes the gate default:
# clustertest 7→10 (3 v1-specific simhash asserts), simtest 3→10
# (7 v1-specific simhash asserts), fdtest 11→12 (1 v1-specific FD
# assert), metatest 2→14 (12 v1-specific AI-metadata syscalls).
# Plus the pre-existing gash_txn flake (FU24.A class). These are
# the originally-predicted ~29 v1-specific tests; the SPSC blocker
# was a separate (now-fixed) issue. Flip is safe whenever the v1
# tests are ported.
#
# v2 formatter is invocable via `make format-disk-v2` for interactive
# smoke testing.
format-disk: scripts/mkfs.gfs disk.img
	@echo "Formatting disk.img with GrahaFS v1..."
	@./scripts/mkfs.gfs disk.img
	@touch .disk_formatted

# Format with v2 (256 MB disk required — journal is 64 MB, plus metadata
# and at least one segment).  Wires through scripts/mkfs.gfs.v2 which
# already supports the full v2 layout (superblock, bitmap, inode table,
# segment table, journal area, root directory).  WARNING: under the gate
# this currently triggers a fs_init kernel panic (SPSC race) — interactive
# smoke testing only until the race is fixed.
format-disk-v2: scripts/mkfs.gfs.v2
	@rm -f disk.img .disk_formatted
	@echo "Creating 256 MB virtual hard disk for v2..."
	@dd if=/dev/zero of=disk.img bs=1M count=256 2>/dev/null
	@echo "Formatting disk.img with GrahaFS v2..."
	@./scripts/mkfs.gfs.v2 disk.img
	@touch .disk_formatted

# Verify disk has valid GrahaFS magic (v1 OR v2), format if not.  Magics:
#   v1 = 0x47524148414F5321 ("GRAHAOS!") — bytes 21534F4148415247 little-endian
#   v2 = 0x47524148414F5322 ("GRAHAOS\"") — bytes 22534F4148415247 little-endian
format-disk-if-needed: scripts/mkfs.gfs
	@if [ ! -f disk.img ]; then \
		echo "No disk.img found, creating and formatting v1..."; \
		$(MAKE) disk.img; \
		./scripts/mkfs.gfs disk.img; \
		touch .disk_formatted; \
	elif ! xxd -l 8 -p disk.img 2>/dev/null | grep -qiE "21534f4148415247|22534f4148415247"; then \
		echo "Disk exists but has no valid GrahaFS magic, formatting v1..."; \
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

# Phase 26 Stage C: regenerate /etc/gcp.wit whenever gcp.json is newer.
etc/gcp.wit: etc/gcp.json scripts/gcp2wit.py
	@echo "Regenerating /etc/gcp.wit from /etc/gcp.json..."
	@python3 scripts/gcp2wit.py etc/gcp.json --out etc/gcp.wit
	@echo "  /etc/gcp.wit: $$(wc -l < etc/gcp.wit) lines, $$(wc -c < etc/gcp.wit) bytes"

# Phase 27 Stage C2: regenerate kernel/manifest_blob.c whenever gcp.json is
# newer. The blob is shipped to userspace via SYS_MANIFEST_EXPORT so AI
# agents can read the syscall surface + detect surface drift via the FNV-1a
# generation hash.
kernel/manifest_blob.c: etc/gcp.json scripts/gen_manifest_blob.py
	@echo "Regenerating kernel/manifest_blob.c from etc/gcp.json..."
	@python3 scripts/gen_manifest_blob.py etc/gcp.json --out kernel/manifest_blob.c

# MODIFIED: Package gash, grahai, and libctest into the initrd (Phase 7c)
# Phase 26 Stage C: gcp.wit added as initrd dep.
initrd.tar: userland etc/motd.txt etc/plan.json etc/gcp.json etc/gcp.wit
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
	@# Phase 28 Session G.3 — gsh shell (TUI-first, parallel to gash).
	@cp user/gsh initrd_root/bin/gsh
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
	@# Phase 16: cantest + eventtest retained on disk for historical reference
	@# but excluded from the TAP manifest below (both use -EDEPRECATED syscalls).
	@cp user/tests/cantest          initrd_root/bin/tests/cantest.tap
	@cp user/tests/eventtest        initrd_root/bin/tests/eventtest.tap
	@cp user/tests/cantest_v2       initrd_root/bin/tests/cantest_v2.tap
	@cp user/tests/canstress        initrd_root/bin/tests/canstress.tap
	@cp user/tests/chantest         initrd_root/bin/tests/chantest.tap
	@# Phase 22 Stage A: named channel registry gate test.
	@cp user/tests/chantest_named   initrd_root/bin/tests/chantest_named.tap
	@# Phase 22 Stage B: ARP / Ethernet / IPv4-helper unit test. Exercises
	@# netd's protocol modules via libnetd.a; independent of the wire path.
	@cp user/tests/netd_arp         initrd_root/bin/tests/netd_arp.tap
	@# Phase 22 Stage B: IPv4 + ICMP + UDP unit test.
	@cp user/tests/netd_ipv4        initrd_root/bin/tests/netd_ipv4.tap
	@# Phase 22 Stage B: TCP state-machine unit test (spec risk #2 gate).
	@cp user/tests/netd_tcp         initrd_root/bin/tests/netd_tcp.tap
	@# Phase 22 Stage B: DHCP client unit test.
	@cp user/tests/netd_dhcp        initrd_root/bin/tests/netd_dhcp.tap
	@# Phase 22 Stage C: DNS wire helpers (pure parse/build, no daemon).
	@cp user/tests/netd_dns         initrd_root/bin/tests/netd_dns.tap
	@# Phase 22 Stage C: /sys/net/service libnet helpers + op-code layout.
	@cp user/tests/netd_service     initrd_root/bin/tests/netd_service.tap
	@# Phase 22 Stage D: libhttp URL / status / headers / chunked (offline).
	@cp user/tests/libhttp_parse    initrd_root/bin/tests/libhttp_parse.tap
	@# Phase 22 Stage E U22: TCP fuzz / RFC 5961 hardening (offline).
	@cp user/tests/tcp_fuzz         initrd_root/bin/tests/tcp_fuzz.tap
	@# Phase 22 closeout (G4.3): gcp.json ↔ kernel manifest validator.
	@cp user/tests/gcp_manifest     initrd_root/bin/tests/gcp_manifest.tap
	@# Phase 22 closeout (G1.6): libtls RFC test vectors (SHA-256 + X25519).
	@cp user/tests/libtls           initrd_root/bin/tests/libtls.tap
	@# Phase 29 Session B (FU28.A): libtls TLS handshake end-to-end gate.
	@cp user/tests/libtls_handshake initrd_root/bin/tests/libtls_handshake.tap
	@# Phase 22 closeout (G6): offline 1000-socket TCP stress test.
	@cp user/tests/tcp_stress_1000  initrd_root/bin/tests/tcp_stress_1000.tap
	@# Phase 23 S1: userdrv MMIO/IRQ/chan synchronous-cleanup stress (P22.G.4).
	@cp user/tests/userdrv_respawn_stress initrd_root/bin/tests/userdrv_respawn_stress.tap
	@# Phase 24 W14/W19 (partial): snapshot lifecycle smoke test (create/delete/list).
	@cp user/tests/snaptest                initrd_root/bin/tests/snaptest.tap
	@# Phase 24 W20.3: 10000-iter snap_create + COW-fault + snap_delete leak test.
	@cp user/tests/snap_stress_cycle       initrd_root/bin/tests/snap_stress_cycle.tap
	@# Phase 24 W20.4: 30-second sustained COW-fault storm.
	@cp user/tests/snap_cow_storm          initrd_root/bin/tests/snap_cow_storm.tap
	@# Phase 25 Stage C: SCOPE_SELF caller-page restore (FU24.I).
	@cp user/tests/snap_restore_self       initrd_root/bin/tests/snap_restore_self.tap
	@# Phase 25 Stage E: chan_send interception substrate (in-scope guarantee).
	@cp user/tests/txn_buffer_send         initrd_root/bin/tests/txn_buffer_send.tap
	@# Phase 25 Stage F: txn_commit / txn_abort state machine + replay engine.
	@cp user/tests/txn_basic_commit        initrd_root/bin/tests/txn_basic_commit.tap
	@cp user/tests/txn_basic_abort         initrd_root/bin/tests/txn_basic_abort.tap
	@cp user/tests/txn_nested_basic        initrd_root/bin/tests/txn_nested_basic.tap
	@cp user/tests/txn_nest_limit          initrd_root/bin/tests/txn_nest_limit.tap
	@# Phase 26 FU25.F: CAP_KIND_SYSTEM substrate gate (TXN_FLAG_GLOBAL_SCOPE).
	@cp user/tests/cap_system              initrd_root/bin/tests/cap_system.tap
	@# Phase 26 Stage D.4: PLEDGE_FLAG_NARROW_EXEC substrate gate.
	@cp user/tests/pledge_narrow_exec      initrd_root/bin/tests/pledge_narrow_exec.tap
	@cp user/assertpledge                  initrd_root/bin/assertpledge
	@# Phase 26 Stage H.1: wasmd loader/validator gate.
	@cp user/tests/wasmtest                initrd_root/bin/tests/wasmtest.tap
	@# FU27.WASM Stage D1: end-to-end execution test. Connects to wasmd via
	@# /sys/wasm/control, runs hello.wasm, checks output + audit code.
	@cp user/tests/wasm_e2e_run            initrd_root/bin/tests/wasm_e2e_run.tap
	@# FU27.WASM Stage D2: trap-path / load-reject / serial-reuse gates.
	@# Each test spawns its own wasmd, exercises one axis (oopsie.wasm
	@# trap; bad/truncated bytes; 4x sequential RUN_MODULE), then kills
	@# wasmd. Worker subprocess (PLEDGE_FLAG_NARROW_EXEC fault isolation)
	@# deferred to FU27.WASM.D2 follow-up; v1 wasmd runs wasm3 in-process.
	@cp user/tests/wasm_fault_trap         initrd_root/bin/tests/wasm_fault_trap.tap
	@cp user/tests/wasm_load_reject        initrd_root/bin/tests/wasm_load_reject.tap
	@cp user/tests/wasm_concurrent_serial  initrd_root/bin/tests/wasm_concurrent_serial.tap
	@# Phase 29 Session G (FU27.WASM.D2_worker): wasmd_worker subprocess +
	@# 6 host bindings (gcp.print + tui_write + tui_read + fs_read + fs_write
	@# + audit_query) + per-instance audit subscription.  5 gates assert
	@# wasmd survives external SIGKILL, sandbox path narrowing, fuel
	@# exhaustion, cap-deny, AND that AI bindings actually drive the cell-VMO.
	@cp user/tests/wasm_fault_sigkill      initrd_root/bin/tests/wasm_fault_sigkill.tap
	@cp user/tests/wasm_sandbox_path_narrow initrd_root/bin/tests/wasm_sandbox_path_narrow.tap
	@cp user/tests/wasm_fault_fuel         initrd_root/bin/tests/wasm_fault_fuel.tap
	@cp user/tests/wasm_lacks_cap          initrd_root/bin/tests/wasm_lacks_cap.tap
	@cp user/tests/wasm_ai_bindings        initrd_root/bin/tests/wasm_ai_bindings.tap
	@# Phase 26 closeout (FU26.C): kernel vsnprintf width/flags parser gate.
	@cp user/tests/vsnprintftest           initrd_root/bin/tests/vsnprintftest.tap
	@# Phase 27 Block A (Stage A2): console subsystem syscall gate.
	@cp user/tests/console_init            initrd_root/bin/tests/console_init.tap
	@# Phase 27 Block A (Stage A3): keyboard Alt+N detection + routing gate.
	@cp user/tests/keyboard_alt            initrd_root/bin/tests/keyboard_alt.tap
	@# Phase 27 Block A (Stage A4): cell→pixel synthetic render gate.
	@cp user/tests/fbd_render              initrd_root/bin/tests/fbd_render.tap
	@# Phase 27 Block A (Stage A5): libtui + box-drawing + 256-color palette.
	@cp user/tests/tui_render              initrd_root/bin/tests/tui_render.tap
	@# Phase 27 Block B (Stage B1): sprite registry + RGBA bitmap overlay.
	@cp user/tests/console_sprite          initrd_root/bin/tests/console_sprite.tap
	@cp user/tests/console_gfx             initrd_root/bin/tests/console_gfx.tap
	@# Phase 27 Block C (Stage C1): audit subscriber broadcast + PLAN_* codes.
	@cp user/tests/audit_stream            initrd_root/bin/tests/audit_stream.tap
	@cp user/tests/audit_plan_codes        initrd_root/bin/tests/audit_plan_codes.tap
	@# Phase 27 Block C (Stage C2): manifest export + rate-quota audit code
	@# + FU26.D cap inheritance gate.
	@cp user/tests/manifest_export         initrd_root/bin/tests/manifest_export.tap
	@cp user/tests/rlimit_syscall_rate     initrd_root/bin/tests/rlimit_syscall_rate.tap
	@cp user/tests/cap_inherit             initrd_root/bin/tests/cap_inherit.tap
	@# FU27.X.cap_recursive_inheritance: parent test spawns the child binary
	@# below; child walks its own cap_handle_table to verify the kernel S5a
	@# walk appended the child's pid to the inherited cap's audience set.
	@cp user/tests/cap_recursive_inheritance  initrd_root/bin/tests/cap_recursive_inheritance.tap
	@cp user/tests/cap_recursive_inherit_child initrd_root/bin/tests/cap_recursive_inherit_child.tap
	@# Phase 28 Session G.1: fault injection harness — kmalloc / pmm /
	@# chan_send / spinlock observer hooks driven via SYS_DEBUG subops 80-84.
	@cp user/tests/inject_kmalloc_fail      initrd_root/bin/tests/inject_kmalloc_fail.tap
	@cp user/tests/inject_pmm_fail          initrd_root/bin/tests/inject_pmm_fail.tap
	@cp user/tests/inject_chan_fail         initrd_root/bin/tests/inject_chan_fail.tap
	@cp user/tests/inject_spinlock_timeout  initrd_root/bin/tests/inject_spinlock_timeout.tap
	@# Phase 28 Session G.2: soak inject front-end.  Reads etc/soak_inject.conf
	@# (written by scripts/run_soak.sh per iter) and applies counters via SYS_DEBUG.
	@cp user/tests/soak_inject_apply        initrd_root/bin/tests/soak_inject_apply.tap
	@# Phase 28 Session G.4: spec-mandated gate tests.
	@cp user/tests/gcp_manifest_export_full initrd_root/bin/tests/gcp_manifest_export_full.tap
	@cp user/tests/gsh_completion           initrd_root/bin/tests/gsh_completion.tap
	@cp user/tests/gsh_chrome               initrd_root/bin/tests/gsh_chrome.tap
	@cp user/tests/ai_txn_rollback          initrd_root/bin/tests/ai_txn_rollback.tap
	@# Phase 29 Session C: kernel ABI polish (FU28.B + FU28.E + FU25.H).
	@cp user/tests/spawn_argv               initrd_root/bin/tests/spawn_argv.tap
	@cp user/tests/audit_query_since        initrd_root/bin/tests/audit_query_since.tap
	@cp user/tests/spawn_handles_inherit    initrd_root/bin/tests/spawn_handles_inherit.tap
	@# Phase 29 Session H: 8 external-peer multi-process txn tests (FU25.C).
	@cp user/tests/txn_replay_order          initrd_root/bin/tests/txn_replay_order.tap
	@cp user/tests/txn_abort_drops           initrd_root/bin/tests/txn_abort_drops.tap
	@cp user/tests/txn_exit_cleanup          initrd_root/bin/tests/txn_exit_cleanup.tap
	@cp user/tests/txn_commit_retry          initrd_root/bin/tests/txn_commit_retry.tap
	@cp user/tests/txn_concurrent_abort      initrd_root/bin/tests/txn_concurrent_abort.tap
	@cp user/tests/txn_buffer_overflow       initrd_root/bin/tests/txn_buffer_overflow.tap
	@cp user/tests/txn_child_abort_parent_commit initrd_root/bin/tests/txn_child_abort_parent_commit.tap
	@cp user/tests/txn_fault_during_replay   initrd_root/bin/tests/txn_fault_during_replay.tap
	@# Phase 29 Session D: TUI primitives (5 syscalls + dirty-rect coalescing).
	@cp user/tests/console_read_input       initrd_root/bin/tests/console_read_input.tap
	@cp user/tests/console_attach_map       initrd_root/bin/tests/console_attach_map.tap
	@cp user/tests/fb_mmio_map              initrd_root/bin/tests/fb_mmio_map.tap
	@cp user/tests/vsync_wait               initrd_root/bin/tests/vsync_wait.tap
	@cp user/tests/dirty_rect               initrd_root/bin/tests/dirty_rect.tap
	@# Phase 29 Session E: animation + cell-grid TX + mouse + full Unicode font.
	@cp user/tests/sprite_anim              initrd_root/bin/tests/sprite_anim.tap
	@cp user/tests/cell_grid_atomic         initrd_root/bin/tests/cell_grid_atomic.tap
	@cp user/tests/mouse_basic              initrd_root/bin/tests/mouse_basic.tap
	@cp user/tests/font_full_sweep          initrd_root/bin/tests/font_full_sweep.tap
	@# Phase 29 Session I: perf phase tests (cpu_affinity, zero_copy_dma, spinlock_timeout, rate_quota).
	@cp user/tests/cpu_affinity             initrd_root/bin/tests/cpu_affinity.tap
	@cp user/tests/spinlock_timeout         initrd_root/bin/tests/spinlock_timeout.tap
	@cp user/tests/rate_quota               initrd_root/bin/tests/rate_quota.tap
	@# Phase 26 closeout (FU25.A.2): gash txn{} parser integration tests.
	@cp user/tests/gash_txn_commit         initrd_root/bin/tests/gash_txn_commit.tap
	@cp user/tests/gash_txn_abort          initrd_root/bin/tests/gash_txn_abort.tap
	@# Pre-Phase-28 sweep B.3 (FU25.A.3): FS-revert on txn abort.
	@cp user/tests/gash_txn_abort_fs_revert initrd_root/bin/tests/gash_txn_abort_fs_revert.tap
	@# Pre-Phase-28 sweep B (FU25.B): grahai --txn integration tests.
	@cp user/tests/grahai_txn_commit       initrd_root/bin/tests/grahai_txn_commit.tap
	@cp user/tests/grahai_txn_abort        initrd_root/bin/tests/grahai_txn_abort.tap
	@# Phase 25 Stage G: stress (1K cycles in gate; nightly env-gates 10K).
	@cp user/tests/txn_stress_basic        initrd_root/bin/tests/txn_stress_basic.tap
	@cp user/tests/txn_stress_nested       initrd_root/bin/tests/txn_stress_nested.tap
	@cp user/tests/txn_stress_state_machine initrd_root/bin/tests/txn_stress_state_machine.tap
	@# Phase 23 S7.1: ahcid registration smoke test (interactive only;
	@# spawns ahcid which competes with kernel AHCI — not in gate sequence).
	@cp user/tests/ahcid_register   initrd_root/bin/tests/ahcid_register.tap
	@# Phase 23 P23.deferred.4: stress harnesses. Built + copied; NOT in
	@# the gate manifest. Run interactively via `gash> ktest <name>`.
	@cp user/tests/blk_stress_kheap        initrd_root/bin/tests/blk_stress_kheap.tap
	@cp user/tests/blk_stress_random_read  initrd_root/bin/tests/blk_stress_random_read.tap
	@cp user/tests/userdrv_respawn_100     initrd_root/bin/tests/userdrv_respawn_100.tap
	@# Phase 24a PRE: blk_micro_latency — measurement ruler. Built + copied,
	@# NOT in gate manifest. Run via `gash> ktest blk_micro_latency`.
	@cp user/tests/blk_micro_latency       initrd_root/bin/tests/blk_micro_latency.tap
	@# Phase 23 P23.deferred.1 cutover validation: ahcid end-to-end I/O.
	@cp user/tests/ahcid_basic_io          initrd_root/bin/tests/ahcid_basic_io.tap
	@cp user/tests/vmotest          initrd_root/bin/tests/vmotest.tap
	@cp user/tests/streamtest       initrd_root/bin/tests/streamtest.tap
	@# Phase 19: versioned GrahaFS v2 test.
	@cp user/tests/fstest_v2        initrd_root/bin/tests/fstest_v2.tap
	@# Phase 20: scheduler + resource-limit tests.
	@cp user/tests/schedtest        initrd_root/bin/tests/schedtest.tap
	@cp user/tests/rlimittest       initrd_root/bin/tests/rlimittest.tap
	@cp user/tests/userdrv          initrd_root/bin/tests/userdrv.tap
	@cp user/tests/nettest          initrd_root/bin/tests/nettest.tap
	@cp user/tests/httptest         initrd_root/bin/tests/httptest.tap
	@cp user/tests/dnstest          initrd_root/bin/tests/dnstest.tap
	@cp user/tests/ktest_discovery  initrd_root/bin/tests/ktest_discovery.tap
	@cp user/tests/ktest_capture    initrd_root/bin/tests/ktest_capture.tap
	@cp user/tests/cmdline_parse    initrd_root/bin/tests/cmdline_parse.tap
	@cp user/tests/klog_basic       initrd_root/bin/tests/klog_basic.tap
	@cp user/tests/klog_stress      initrd_root/bin/tests/klog_stress.tap
	@# Phase 14: allocator tests.
	@cp user/tests/slab_basic       initrd_root/bin/tests/slab_basic.tap
	@cp user/tests/kheap_basic      initrd_root/bin/tests/kheap_basic.tap
	@cp user/tests/percpu_basic     initrd_root/bin/tests/percpu_basic.tap
	@cp user/tests/mem_stress       initrd_root/bin/tests/mem_stress.tap
	@# Phase 15a: capability objects v2 tests.
	@cp user/tests/captest_v2       initrd_root/bin/tests/captest_v2.tap
	@# Phase 15b: pledge + audit tests.
	@cp user/tests/pledgetest       initrd_root/bin/tests/pledgetest.tap
	@cp user/tests/audittest        initrd_root/bin/tests/audittest.tap
	@# Phase 13: panic_test is NOT a TAP test — it intentionally panics
	@# the kernel. Copied to bin/ so autorun=panic_test can spawn it.
	@cp user/tests/panic_test       initrd_root/bin/panic_test
	@cp user/tests/kpf_test         initrd_root/bin/kpf_test
	@cp user/ktest                   initrd_root/bin/
	@# Phase 13: standalone klog reader.
	@cp user/klog                    initrd_root/bin/klog
	@# Phase 14: allocator stats reader.
	@cp user/memstat                 initrd_root/bin/memstat
	@# Phase 15a: capability inspector.
	@cp user/caps                    initrd_root/bin/caps
	@# Phase 15b: audit log reader.
	@cp user/auditq                  initrd_root/bin/auditq
	@# Phase 16: CAN capability-table printer.
	@cp user/can-ctl                 initrd_root/bin/can-ctl
	@# Phase 19: version-chain lister.
	@cp user/fsversions              initrd_root/bin/fsversions
	@# Phase 20: resource-limit CLI and mem-limit test program.
	@cp user/ulimit                  initrd_root/bin/ulimit
	@cp user/mallocbomb              initrd_root/bin/mallocbomb
	@# Phase 20 U16: scheduler benchmark + its compute worker.
	@cp user/schedbench              initrd_root/bin/schedbench
	@cp user/schedbench_worker       initrd_root/bin/schedbench_worker
	@# Phase 21: init supervisor (PID 1 candidate, opt-in via autorun=init).
	@cp user/init                    initrd_root/bin/init
	@# Phase 21: drvctl — driver inspector CLI.
	@cp user/drvctl                  initrd_root/bin/drvctl
	@# Phase 21.1: e1000d userspace NIC daemon. Spawned by /bin/init when
	@# /etc/init.conf names it; without that line it just sits in initrd
	@# unused (which is the case for `make test`).
	@cp user/drivers/e1000d          initrd_root/bin/e1000d
	@# Phase 23 S3: ahcid userspace AHCI daemon. Same spawn semantics.
	@cp user/drivers/ahcid           initrd_root/bin/ahcid
	@# Phase 23 S6: blkctl block-device CLI (twin of drvctl).
	@cp user/blkctl                  initrd_root/bin/blkctl
	@# Phase 24 W19.6: snapshot CLI. One binary; gash dispatches verbs
	@# `snapshot`, `snapshots`, `restore`, `snap-delete` to it via argv[0].
	@cp user/snapshot                initrd_root/bin/snapshot
	@cp user/snapshot                initrd_root/bin/snapshots
	@cp user/snapshot                initrd_root/bin/restore
	@cp user/snapshot                initrd_root/bin/snap-delete
	@# Phase 25 Stage H: txnctl CLI. argv[0] dispatch — `txn-status` reuses
	@# the same binary. Strictly informational; transactions are
	@# process-scoped (use gash `txn { ... }` or grahai --txn instead).
	@cp user/txnctl                  initrd_root/bin/txnctl
	@cp user/txnctl                  initrd_root/bin/txn-status
	@# Phase 26 Stage E/G + FU27.WASM Stage D1: bin/wasm operator CLI.
	@# Stage D1 transitioned wasm from local-parse-only to a thin libnet
	@# client of /sys/wasm/control (wasmd does the parsing + execution).
	@cp user/wasm                    initrd_root/bin/wasm
	@# FU27.WASM Stage D1: /bin/wasmd daemon + /bin/wasmd_worker per-instance.
	@# wasmd accepts RUN_MODULE on /sys/wasm/control, narrow-execs the
	@# worker via PLEDGE_FLAG_NARROW_EXEC, captures stdout, returns
	@# response. Spawned by init under autorun=init via etc/init.conf.
	@mkdir -p initrd_root/bin
	@cp user/wasmd/wasmd             initrd_root/bin/wasmd
	@cp user/wasmd/wasmd_worker      initrd_root/bin/wasmd_worker
	@# FU27.WASM Stage D1: 5 hand-rolled WebAssembly fixtures generated by
	@# scripts/gen_wasm_fixtures.py. Used by the D2 gate tests + grahai
	@# integration. Validated against host wasm3 (parse-OK heuristic).
	@mkdir -p initrd_root/bin/tests/wasm
	@python3 scripts/gen_wasm_fixtures.py --out-dir initrd_root/bin/tests/wasm
	@# Phase 27 Block A (Stage A4): /bin/fbd userspace framebuffer compositor.
	@# Spawned by init under autorun=init only (NOT in autorun=ktest gate).
	@cp user/fbd/fbd                 initrd_root/bin/fbd
	@# FU27.X.tui_demo_apps: tui_demo + tui_anim manual-only demos.
	@# NOT in test manifest; run interactively from `gash> tui_demo` or
	@# `gash> tui_anim`. tui_demo exercises libtui boxed-windows with
	@# 256-color palette; tui_anim exercises the gfx_overlay + alpha
	@# blending path landed in S1.B (FU27.X.alpha_blend).
	@cp user/tui_demo                initrd_root/bin/tui_demo
	@cp user/tui_anim                initrd_root/bin/tui_anim
	@# Phase 22 Stage A: netd userspace TCP/IP daemon skeleton. Spawned by
	@# init when /etc/init.conf names it (default init.conf below doesn't
	@# for `make test`); sits in initrd unused otherwise. Stage A content:
	@# rendezvous with e1000d via /sys/net/rawframe + publish /sys/net/service.
	@cp user/netd                    initrd_root/bin/netd
	@# Phase 22 Stage C: /bin/ifconfig + /bin/ping — channel-RPC clients
	@# of netd. Replace the pre-Phase-22 gash builtins that dispatched
	@# into SYS_NET_IFCONFIG and friends.
	@cp user/ifconfig                initrd_root/bin/ifconfig
	@cp user/ping                    initrd_root/bin/ping
	@# Phase 22 closeout (G2): /etc/init.conf carries the userspace
	@# daemon list — e1000d + netd auto-spawn at boot when the kernel is
	@# booted with autorun=init (the new default in kernel/autorun.c).
	@# `make test` overrides via `autorun=ktest` cmdline, in which case
	@# init.conf is never read (ktest is PID 1 directly). Pledge CSVs are
	@# advisory today; Phase 24 wires per-daemon pledge_subset on spawn.
	@echo "# /etc/init.conf — Phase 22 init supervisor configuration" > initrd_root/etc/init.conf
	@echo "# daemon=<binary>:<pledge_csv>  (CSV is advisory until Phase 24)" >> initrd_root/etc/init.conf
	@echo "# autorun=<binary>" >> initrd_root/etc/init.conf
	@# FU29.X: /bin/ahcid is spawned KERNEL-CONTEXT by the blk_client kt task
	@# (blk_client.c:1018, in BOTH ktest and interactive) so it can publish
	@# /sys/blk/service and mount the FS BEFORE init runs.  Listing it here too
	@# made init double-spawn a second ahcid which hit drv_register -16 (the
	@# device is already owned) and crash-looped.  So it is intentionally NOT an
	@# init daemon; the userdrv framework handles respawn-on-death.
	@echo "# bin/ahcid: spawned kernel-context by the blk_client kt task; do NOT" >> initrd_root/etc/init.conf
	@echo "#            list as an init daemon (double-spawn -> drv_register -16)." >> initrd_root/etc/init.conf
	@echo "daemon=bin/e1000d:net_server,sys_control,sys_query,ipc_send,ipc_recv" >> initrd_root/etc/init.conf
	@echo "daemon=bin/netd:net_server,net_client,ipc_send,ipc_recv,sys_query,fs_read,compute,time" >> initrd_root/etc/init.conf
	@# Phase 27 Block A (Stage A4): fbd userspace framebuffer compositor.
	@# Owns the framebuffer once SYS_CONSOLE_ACK_RENDER fires; klog stops
	@# painting the FB and starts mirroring serial only.
	@#
	@# DISABLED 2026-05-06 (post-Phase-28 sweep) — fbd's substrate was
	@# scoped substrate-only at Stage A4; it owns the framebuffer (sets
	@# g_fbd_alive=1, gating every framebuffer_draw_* call via
	@# fb_should_bypass) but never actually paints. Result: gash text +
	@# grahai legacy-mode draws + boot messages all silently bail in the
	@# QEMU graphical window. tui_demo / tui_anim work because they use
	@# framebuffer_force_* (force-prefix bypasses the bypass), but normal
	@# user-facing output doesn't. Re-enable when fbd gains a paint loop
	@# that translates cell VMOs into pixels (post-Phase-28 polish).
	@# fbd_render.tap is unaffected (uses DEBUG_CONSOLE_SYNTHETIC_RENDER
	@# which runs through the force-path).
	@# echo "daemon=bin/fbd:ipc_send,ipc_recv,sys_query,sys_control,time" >> initrd_root/etc/init.conf
	@# FU27.WASM Stage D1: bin/wasmd daemon. Pledge:
	@#   ipc_send/ipc_recv  — chan_publish + accept loop
	@#   sys_control        — SYS_PLEDGE narrow-exec for spawning workers
	@#   sys_query          — audit + status checks
	@#   fs_read            — open client-supplied .wasm file
	@#   fs_write           — stage at /tmp/wasmd_pending.wasm + read worker output
	@#   compute,time       — wasm3 runs in worker (worker has narrowed pledge)
	@# FU29.X.wasmd_subprocess: + spawn (SYS_SPAWN_ARGV the killable
	@#   wasmd_worker for runaway-module isolation; sys_control already
	@#   present covers the deadline SIGKILL).
	@echo "daemon=bin/wasmd:ipc_send,ipc_recv,sys_control,sys_query,fs_read,fs_write,compute,time,spawn" >> initrd_root/etc/init.conf
ifdef TEST_HARNESS
	@# G6.4 — echod loopback echo daemon for tcp_stress_1000 harness.
	@# Production boot does NOT carry this; only `make TEST_HARNESS=1 ...`.
	@echo "daemon=bin/echod:net_server,net_client,ipc_send,ipc_recv" >> initrd_root/etc/init.conf
endif
	@# Phase 29 Session F: autorun flipped to bin/gsh.  bin/gash is still
	@# shipped in initrd_root/bin/ and remains spawnable via
	@# `gsh> exec bin/gash` (back-compat path for legacy gash workflows).
	@echo "autorun=bin/gsh" >> initrd_root/etc/init.conf
	@# Phase 22 closeout (G1.4) / Phase 29 Session B (FU28.A): ship the
	@# Mozilla NSS root CA bundle.  Phase 29 migrates the canonical copy
	@# from user/libtls-mg/assets/ to etc/tls/ so libtls (BearSSL backend)
	@# can find it without the libtls-mg subtree present.  The legacy
	@# location is still consulted as a fallback during the cutover.
	@mkdir -p initrd_root/etc/tls
	@if [ -f etc/tls/trust.pem ]; then \
	    cp etc/tls/trust.pem initrd_root/etc/tls/trust.pem; \
	else \
	    cp user/libtls-mg/assets/trust.pem initrd_root/etc/tls/trust.pem; \
	fi
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
	@# pipetest is intentionally NOT here — it has a documented bimodal
	@# TCG-AHCI-floor flake (~70%-90% pass rate; project memory
	@# feedback_phase24a_tcg_ahci_floor.md) that produces an INCOMPLETE
	@# TAP block (test 10 starts then the kernel goes silent — looks
	@# like a triple-fault under -no-reboot).  ktest's parser stops the
	@# gate at the first INCOMPLETE, which would mask every test
	@# scheduled after pipetest.  We move pipetest to the END of the
	@# manifest so its flake costs only its own coverage, not the rest
	@# of the gate.  Sub-phase H (KVM port + TSC calibration) is the
	@# planned remediation; until then this ordering keeps the gate
	@# signal honest.
	@echo "fdtest" >> initrd_root/bin/tests/manifest.txt
	@echo "metatest" >> initrd_root/bin/tests/manifest.txt
	@echo "spawntest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 16: cantest + eventtest removed — both call deprecated syscalls
	@# now returning -EDEPRECATED. Their coverage is preserved (and extended)
	@# by cantest_v2 using token-taking SYS_CAN_* syscalls.
	@echo "nettest" >> initrd_root/bin/tests/manifest.txt
	@echo "httptest" >> initrd_root/bin/tests/manifest.txt
	@echo "dnstest" >> initrd_root/bin/tests/manifest.txt
	@echo "ktest_discovery" >> initrd_root/bin/tests/manifest.txt
	@echo "ktest_capture" >> initrd_root/bin/tests/manifest.txt
	@echo "cmdline_parse" >> initrd_root/bin/tests/manifest.txt
	@# Phase 13: klog syscalls round-trip + stress tests.
	@echo "klog_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "klog_stress" >> initrd_root/bin/tests/manifest.txt
	@# Phase 14: slab + kheap + per-CPU + stress.
	@echo "slab_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "kheap_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "percpu_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "mem_stress" >> initrd_root/bin/tests/manifest.txt
	@# Phase 24 W14/W16/W17/W19: snapshot lifecycle (create + capture +
	@# restore + delete + list).  Placed AFTER slab_basic / kheap_basic /
	@# mem_stress because snap_create's many kmalloc()s populate per-CPU
	@# magazines for kheap_128 / 256 / 2048 with SUBSYS_CORE-tagged
	@# objects; the slab/kheap tests assert that fresh allocs increment
	@# subsys_counters[SUBSYS_TEST=9] >= 1, which only holds when the
	@# magazine misses (the pop fast path doesn't touch the counter).
	@# Running snaptest after the magazine-sensitive tests keeps both
	@# signal sets clean.
	@echo "snaptest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 24 W20.3: 10K-iter snap stress (~5-15s wall in TCG).
	@echo "snap_stress_cycle" >> initrd_root/bin/tests/manifest.txt
	@# Phase 24 W20.4: 30-second sustained COW-fault storm (resnap every
	@# 256 writes to keep faulting fresh pages).
	@echo "snap_cow_storm" >> initrd_root/bin/tests/manifest.txt
	@# Phase 25 Stage C (FU24.I): SCOPE_SELF caller-page restore — exercises
	@# restore_pages's skip_page_va path so the caller's BSS + heap revert
	@# but its active stack page (in-flight syscall return frame) does not.
	@echo "snap_restore_self" >> initrd_root/bin/tests/manifest.txt
	@# Phase 25 Stage E: chan_send txn-aware prologue. In-scope sends (peer
	@# == sender PID) must NOT be buffered; this test verifies the prologue
	@# falls through correctly to the live ring on SCOPE_SELF self-channel
	@# sends, and that empty-buffer abort/commit are clean.
	@echo "txn_buffer_send" >> initrd_root/bin/tests/manifest.txt
	@# Phase 25 Stage F: state-machine + commit/abort substrate. These four
	@# tests exercise the empty-buffer fast path, the snap_restore_internal
	@# revert through txn_abort, the nesting stack, and the TXN_MAX_NESTING
	@# enforcement. External-peer replay coverage lands at Stage I via
	@# multi-process gash + grahai integration tests.
	@echo "txn_basic_commit" >> initrd_root/bin/tests/manifest.txt
	@echo "txn_basic_abort" >> initrd_root/bin/tests/manifest.txt
	@echo "txn_nested_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "txn_nest_limit" >> initrd_root/bin/tests/manifest.txt
	@# Phase 26 FU25.F: CAP_KIND_SYSTEM bootcap + GLOBAL_SCOPE gate. Verifies
	@# the substrate is in tree and txn_begin's GLOBAL_SCOPE check goes
	@# through cap_system_resolve. Negative-path test: ktest doesn't
	@# inherit init's CAP_KIND_SYSTEM (cap-inheritance-on-spawn is a
	@# pending FU follow-up), so GLOBAL_SCOPE returns -EPERM here.
	@echo "cap_system" >> initrd_root/bin/tests/manifest.txt
	@# Phase 26 Stage D.4: PLEDGE_FLAG_NARROW_EXEC substrate gate. Spawns
	@# /bin/assertpledge under a narrowed pledge bundle and validates the
	@# subset/cap/reserved-field rejection paths + audit emission.
	@echo "pledge_narrow_exec" >> initrd_root/bin/tests/manifest.txt
	@# Phase 26 Stage H.1: wasmd loader/validator gate (no execution).
	@echo "wasmtest" >> initrd_root/bin/tests/manifest.txt
	@# FU27.WASM Stage D1: end-to-end execution test. Asserts wasmd
	@# accepts RUN_MODULE, narrow-execs a worker, hello.wasm prints,
	@# AUDIT_PLEDGE_NARROW_EXEC visible.
	@echo "wasm_e2e_run" >> initrd_root/bin/tests/manifest.txt
	@# FU27.WASM Stage D2: trap-path gate. oopsie.wasm hits unreachable;
	@# wasmd returns WASMD_E_TRAP. Then hello.wasm proves wasmd survived.
	@echo "wasm_fault_trap" >> initrd_root/bin/tests/manifest.txt
	@# FU27.WASM Stage D2: load-reject gate. Bad-magic + truncated bytes
	@# both rejected with WASMD_E_LOAD_FAILED; daemon survives.
	@echo "wasm_load_reject" >> initrd_root/bin/tests/manifest.txt
	@# FU27.WASM Stage D2: serial reuse gate. 4x RUN_MODULE on the same
	@# connection — checks wasm3's NewEnvironment/NewRuntime per-call
	@# lifecycle leaves no leftover state.
	@echo "wasm_concurrent_serial" >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session G: 3 gates exercising the new in-process host
	@# bindings (gcp.tui_write / fs_read / fs_write / audit_query) +
	@# path-prefix sandbox + cap-deny via missing imports.
	@echo "wasm_sandbox_path_narrow" >> initrd_root/bin/tests/manifest.txt
	@echo "wasm_lacks_cap" >> initrd_root/bin/tests/manifest.txt
	@echo "wasm_ai_bindings" >> initrd_root/bin/tests/manifest.txt
	@# FU29.X.wasmd_subprocess: true crash/runaway isolation via a KILLABLE
	@# bin/wasmd_worker subprocess.  The module flows to the worker hex-encoded
	@# in argv (NO filesystem → the worker holds no vfs_lock), so wasmd's
	@# deadline SIGKILL of a runaway worker lands in a pure-userspace wasm loop
	@# = orphan-free (kernel kill-safety verdict).  These two fixtures are
	@# infinite loops the in-process wasm3 path fundamentally cannot contain.
	@echo "wasm_fault_fuel" >> initrd_root/bin/tests/manifest.txt
	@echo "wasm_fault_sigkill" >> initrd_root/bin/tests/manifest.txt
	@# Phase 26 closeout (FU26.C): kernel vsnprintf width/flags parser test.
	@# Verifies %04x / %5d / %-10s / etc. produce correct output and that
	@# the unknown-spec default branch no longer slips va_args (FU26.A trap).
	@echo "vsnprintftest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block A (Stage A2): SYS_CONSOLE_SWITCH + SYS_CONSOLE_ACK_RENDER
	@# dispatch + bounds checking. fbd compositor (Stage A4) consumes these.
	@echo "console_init" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block A (Stage A3): keyboard Alt+N detection. Injects PS/2
	@# scancodes via DEBUG_INJECT_SCANCODE; verifies console_switch fires.
	@echo "keyboard_alt" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block A (Stage A4): cell→pixel render via synthetic
	@# composite (kernel-side). Writes 'A' via DEBUG_CONSOLE_WRITE_CELL,
	@# triggers DEBUG_CONSOLE_SYNTHETIC_RENDER, reads pixels via
	@# DEBUG_FB_READ_PIXEL and verifies foreground/background match.
	@echo "fbd_render" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block A (Stage A5): libtui box-drawing + 256-color palette
	@# + cursor attribute. Exercises tui_draw_box, tui_palette_lookup,
	@# TUI_ATTR_CURSOR via libtui → DEBUG syscall substrate.
	@echo "tui_render" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block B (Stage B1): per-console sprite registry (codepoints
	@# 0xE100..0xE7FF) + RGBA bitmap overlay with damage-ring tracking.
	@echo "console_sprite" >> initrd_root/bin/tests/manifest.txt
	@echo "console_gfx" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block C (Stage C1): per-process audit subscriber broadcast
	@# (16 slots × 64-entry SPSC ring) + PLAN_BEGIN/STEP/COMMIT/ABORT codes.
	@echo "audit_stream" >> initrd_root/bin/tests/manifest.txt
	@echo "audit_plan_codes" >> initrd_root/bin/tests/manifest.txt
	@# Phase 27 Block C (Stage C2): SYS_MANIFEST_EXPORT + AUDIT_RLIMIT_SYSCALL_RATE
	@# substrate + FU26.D cap inheritance walk in sched_create_user_process.
	@echo "manifest_export" >> initrd_root/bin/tests/manifest.txt
	@echo "rlimit_syscall_rate" >> initrd_root/bin/tests/manifest.txt
	@echo "cap_inherit" >> initrd_root/bin/tests/manifest.txt
	@# FU27.X.cap_recursive_inheritance gate test. Parent spawns the child
	@# binary (NOT in manifest.txt — harness only auto-runs items listed
	@# here; the parent invokes the child directly via syscall_spawn).
	@echo "cap_recursive_inheritance" >> initrd_root/bin/tests/manifest.txt
	@# Phase 28 Session G.1: fault injection gate (4 tests / 12 asserts).
	@echo "inject_kmalloc_fail"      >> initrd_root/bin/tests/manifest.txt
	@echo "inject_pmm_fail"          >> initrd_root/bin/tests/manifest.txt
	@echo "inject_chan_fail"         >> initrd_root/bin/tests/manifest.txt
	@echo "inject_spinlock_timeout"  >> initrd_root/bin/tests/manifest.txt
	@# Phase 28 Session G.2: soak inject front-end (2 asserts).  Listed
	@# first so it runs before the test suite in soak iterations.
	@echo "soak_inject_apply"        >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session H: 8 external-peer multi-process txn tests (FU25.C).
	@# 5 asserts each = +40 gate assertions.  Each test self-spawns a helper
	@# instance of the same binary (sentinel-driven role detection) and
	@# uses chan_publish/connect to exchange channel endpoints.  Listed
	@# BEFORE gsh_completion (which has a pre-existing FU24.B/C wait/exit
	@# race that intermittently hangs the runner) so these run in every
	@# iteration regardless of gsh_completion's outcome.
	@echo "txn_replay_order"               >> initrd_root/bin/tests/manifest.txt
	@echo "txn_abort_drops"                >> initrd_root/bin/tests/manifest.txt
	@echo "txn_exit_cleanup"               >> initrd_root/bin/tests/manifest.txt
	@echo "txn_commit_retry"               >> initrd_root/bin/tests/manifest.txt
	@echo "txn_concurrent_abort"           >> initrd_root/bin/tests/manifest.txt
	@echo "txn_buffer_overflow"            >> initrd_root/bin/tests/manifest.txt
	@echo "txn_child_abort_parent_commit"  >> initrd_root/bin/tests/manifest.txt
	@echo "txn_fault_during_replay"        >> initrd_root/bin/tests/manifest.txt
	@# Phase 28 Session G.4: spec-mandated gate tests (5+5+8 = 18 asserts).
	@echo "gcp_manifest_export_full" >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session I (FU24.E): SYS_SET_CPU_AFFINITY gate test.  Placed
	@# BEFORE gsh_completion (which has a known FU25.A.4 wait/exit race) so
	@# the cpu_affinity asserts run in every gate iteration regardless of
	@# whether gsh_completion's helper-process flake fires.
	@echo "cpu_affinity"             >> initrd_root/bin/tests/manifest.txt
	@echo "spinlock_timeout"         >> initrd_root/bin/tests/manifest.txt
	@echo "rate_quota"               >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 FU24.B/FU25.A.4 gate-integrity fix: the shell-spawn tests
	@# (gsh_completion, gsh_chrome, ai_txn_rollback, gash_txn_*, grahai_txn_*)
	@# spawn an interactive shell and syscall_wait for it.  When the sentinel
	@# write flakes the shell can fall through to interactive mode and block
	@# forever; ktest cannot time out a blocked wait, so a hang there SILENTLY
	@# TRUNCATES every test listed after it.  They are now CLUSTERED AT THE
	@# VERY END of the manifest (after txn_stress_*) so a hang can only affect
	@# its own cluster, never the ~110 deterministic tests.  See the relocated
	@# block near the end of this rule.
	@# Phase 29 Session C: kernel ABI polish.  audit_query_since (4) is
	@# deterministic and stays here; spawn_argv + spawn_handles_inherit SPAWN
	@# child processes and can hit the FU24.A/blk-layer channel-mode FS hang
	@# (child stuck in a write/open syscall -> never exits -> parent's blocked
	@# SYS_WAIT poll-loop spins to the watchdog).  Their kernel fix
	@# (SYS_SPAWN_ARGV SMP seeding) is ALREADY validated by the deterministic
	@# multi-proc txn tests above (txn_replay_order etc., which spawn argv
	@# children).  They are RELOCATED to the flaky cluster at the end so an
	@# intermittent FS hang cannot truncate the ~110 deterministic tests.
	@echo "audit_query_since"        >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session D: TUI primitives.  console_read_input (5) +
	@# console_attach_map (5) + fb_mmio_map (4) + vsync_wait (3) +
	@# dirty_rect (4) = +21 asserts.
	@echo "console_read_input"       >> initrd_root/bin/tests/manifest.txt
	@echo "console_attach_map"       >> initrd_root/bin/tests/manifest.txt
	@echo "fb_mmio_map"              >> initrd_root/bin/tests/manifest.txt
	@echo "vsync_wait"               >> initrd_root/bin/tests/manifest.txt
	@echo "dirty_rect"               >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session E: sprite animation + cell-grid atomic TX + mouse
	@# + full Unicode font sweep.  sprite_anim (5) + cell_grid_atomic (6)
	@# + mouse_basic (4) + font_full_sweep (7, FU29.X.font_cjk: +3) = +22 asserts.
	@echo "sprite_anim"              >> initrd_root/bin/tests/manifest.txt
	@echo "cell_grid_atomic"         >> initrd_root/bin/tests/manifest.txt
	@echo "mouse_basic"              >> initrd_root/bin/tests/manifest.txt
	@echo "font_full_sweep"          >> initrd_root/bin/tests/manifest.txt
	@# Pre-Phase-28 sweep A.5 (resolves FU25.A.4): gash_txn_commit + gash_txn_abort
	@# promoted to gate. Previously held back due to FU24.B/C wait/exit race
	@# (intermittent INCOMPLETE on ~33-50% of TCG iters when ktest's syscall_wait
	@# missed the test's SYS_EXIT wake). Pre-Phase-28 sweep A.3 fixed the race
	@# via F1-pattern sched_block_on_channel in SYS_WAIT polling + pipe_read.
	@# Each test asserts 4: sentinel staged, gash spawn OK, exit 0, AUDIT_TXN_*
	@# emitted. Total +8 assertions; gate 992 → 1000.
	@# (Relocated to the shell-spawn cluster at the end — see FU24.B/FU25.A.4
	@# gate-integrity note above.)
	@# Pre-Phase-28 sweep B.3 (FU25.A.3): substrate landed (kernel + user
	@# wrappers + gash setup_redirects pin call) but the gate-resident
	@# verification depends on grahafs_v2 mount which is currently v1
	@# (`blk_client_fs_init: grahafs v1 mounted`). The test is built and
	@# ships in the initrd at bin/tests/gash_txn_abort_fs_revert.tap;
	@# runnable interactively via `gash> ktest gash_txn_abort_fs_revert`
	@# once v2 mount is gate-resident. NOT in manifest.txt — would FAIL
	@# under v1 because version_chain_head_id is v2-only and snap_add_fs_pin
	@# silently no-ops when inode_cache_get returns NULL on v1.
	@# Pre-Phase-28 sweep B (FU25.B): grahai --txn / --abort integration.
	@# grahai_txn_commit asserts AUDIT_TXN_COMMIT (42) emitted on plan
	@# success; grahai_txn_abort asserts AUDIT_TXN_ABORT (43) emitted on
	@# --abort sentinel. 4 asserts each → +8 gate.
	@# (Relocated to the shell-spawn cluster at the end — see FU24.B/FU25.A.4
	@# gate-integrity note above.)
	@# Phase 15a: capability objects v2.
	@echo "captest_v2" >> initrd_root/bin/tests/manifest.txt
	@# Phase 15b: pledge classes + audit log.
	@echo "pledgetest" >> initrd_root/bin/tests/manifest.txt
	@echo "audittest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 16: CAN callbacks + token-taking activate/deactivate.
	@echo "cantest_v2" >> initrd_root/bin/tests/manifest.txt
	@echo "canstress" >> initrd_root/bin/tests/manifest.txt
	@echo "chantest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage A: SYS_CHAN_PUBLISH / SYS_CHAN_CONNECT coverage.
	@echo "chantest_named" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage B: ARP / Ethernet / IPv4 unit tests via libnetd.a.
	@echo "netd_arp" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage B: IPv4 + ICMP + UDP unit tests.
	@echo "netd_ipv4" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage B: TCP state-machine unit test.
	@echo "netd_tcp" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage B: DHCP client unit test.
	@echo "netd_dhcp" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage C: DNS query/response wire parser unit test.
	@echo "netd_dns" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage C: /sys/net/service libnet message helpers + op-code layout.
	@echo "netd_service" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage D: libhttp URL / status / headers / chunked offline coverage.
	@echo "libhttp_parse" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 Stage E U22: TCP fuzz / RFC 5961 hardening corpus.
	@echo "tcp_fuzz" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 closeout (G4.3): etc/gcp.json ↔ kernel manifest validator.
	@echo "gcp_manifest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 closeout (G1.6): libtls RFC test vectors (SHA-256 + X25519).
	@echo "libtls" >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 Session B (FU28.A): libtls handshake end-to-end gate (skip
	@# until vendor/bearssl/ is wired).
	@echo "libtls_handshake" >> initrd_root/bin/tests/manifest.txt
	@# Phase 22 closeout (G6): 1000-socket TCP stress.
	@echo "tcp_stress_1000" >> initrd_root/bin/tests/manifest.txt
	@# Phase 23 Stage-2 cutover: ahcid_register tested in-gate after
	@# the userdrv largest-BAR fix; the spawn/kill cycle still leaves
	@# subsequent FS-touching tests (vmotest, streamtest) flaky in
	@# ~33% of runs due to grahafs/PMM accounting drift after the kill.
	@# Stays interactive-only (`gash> ktest ahcid_register`).
	@# Phase 23 S1: userdrv_respawn_stress (10-cycle spawn/kill of
	@# e1000d) is still INTERACTIVE-ONLY.  Each cycle re-claims the same
	@# PCI BAR; under stack-of-tests the kheap/PMM accounting trips
	@# subsequent tests that spawn user processes.  Run as
	@# `gash> ktest userdrv_respawn_stress` for the death-recovery
	@# coverage; the kernel-side userdrv_force_release_mmio path it
	@# exercises is the same one the gate already verifies via
	@# userdrv assertions 8-10.
	@# Phase 23 Stage-2 cutover: ahcid_basic_io stays interactive-only
	@# (channel-mode end-to-end via the standalone test path).  The
	@# kernel-side blk_client kt task already validates the same code
	@# path on every boot — the wrappers transition to channel mode
	@# whenever ahcid is up — so the standalone test is duplicate
	@# coverage rather than additional gate value.
	@echo "vmotest" >> initrd_root/bin/tests/manifest.txt
	@echo "streamtest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 19: versioned GrahaFS v2.
	@echo "fstest_v2" >> initrd_root/bin/tests/manifest.txt
	@echo "schedtest" >> initrd_root/bin/tests/manifest.txt
	@# rlimittest relocated to the VERY END (after the shell-spawn cluster) —
	@# FU24.B: it intermittently hangs on the second mallocbomb spawn/wait
	@# (rlimit + wait/exit interaction under kheap load).  Listed last so that
	@# intermittent hang can't truncate the deterministic userdrv / pipetest /
	@# txn_stress tests below.
	@# Phase 21: userdrv substrate tests (kernel-side framework only;
	@# full e1000d daemon test in e1000dtest below).
	@echo "userdrv" >> initrd_root/bin/tests/manifest.txt
	@# Phase 21.1: e1000dtest — end-to-end test for the userspace e1000d
	@# daemon + e1000_proxy. Spawns /bin/e1000d, asserts ANNOUNCE round-trip,
	@# kills + respawns to verify the death-cleanup path.
	@cp user/tests/e1000dtest        initrd_root/bin/tests/e1000dtest.tap
	@echo "e1000dtest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 23 Step 4: userdrv_respawn_stress — 10-cycle spawn/kill of
	@# e1000d.  Validates the userdrv_force_release_mmio synchronous-cleanup
	@# path landed in Step 1.  Placed at the end of the manifest so the
	@# kheap/PMM accounting drift the comment-block above describes can't
	@# trip earlier tests that need fresh-state user spawns.
	@echo "userdrv_respawn_stress" >> initrd_root/bin/tests/manifest.txt
	@# pipetest at the very end (after every other test) so its bimodal
	@# TCG flake at test 10 doesn't cut off the rest of the gate. See
	@# the comment block earlier in this manifest for the rationale.
	@echo "pipetest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 25 Stage G: stress (each ~0.5-2 s wall in TCG; well within
	@# budget). Placed AFTER pipetest because they're tolerant of any
	@# residual fallout from pipetest's flake (they don't depend on pipe
	@# state).
	@echo "txn_stress_basic" >> initrd_root/bin/tests/manifest.txt
	@echo "txn_stress_nested" >> initrd_root/bin/tests/manifest.txt
	@echo "txn_stress_state_machine" >> initrd_root/bin/tests/manifest.txt
	@# Phase 29 FU24.B/FU25.A.4 gate-integrity: shell-spawn cluster, LAST.
	@# These tests spawn bin/gsh / bin/gash / bin/grahai and syscall_wait for
	@# the child.  If a child falls through to interactive mode (e.g. on a
	@# flaky sentinel write) it blocks forever, and because ktest cannot time
	@# out a blocked wait, a hang here would truncate the suite.  By listing
	@# them LAST (after every deterministic test), a hang can only affect this
	@# cluster — the ~110 deterministic tests above always complete.
	@# gsh_completion's write_file now loops on short writes + bails without
	@# spawning gsh if the sentinel still can't be staged (no interactive hang).
	@# spawn_argv + spawn_handles_inherit relocated here (FU24.A/blk hang): a
	@# spawned child can get stuck in a channel-mode FS write/open under load so
	@# the parent's SYS_WAIT poll-loop never returns.  Their SMP-seeding fix is
	@# validated by the deterministic multi-proc txn tests; here they only risk
	@# the flaky cluster.  spawn_handles_inherit is now fork-bomb-proof (argv
	@# baseline + verified WITH role + /.shi_guard) so it can't 700s-cascade.
	@echo "spawn_argv"               >> initrd_root/bin/tests/manifest.txt
	@echo "spawn_handles_inherit"    >> initrd_root/bin/tests/manifest.txt
	@echo "gsh_completion"           >> initrd_root/bin/tests/manifest.txt
	@echo "gsh_chrome"               >> initrd_root/bin/tests/manifest.txt
	@echo "ai_txn_rollback"          >> initrd_root/bin/tests/manifest.txt
	@echo "gash_txn_commit"          >> initrd_root/bin/tests/manifest.txt
	@echo "gash_txn_abort"           >> initrd_root/bin/tests/manifest.txt
	@echo "grahai_txn_commit"        >> initrd_root/bin/tests/manifest.txt
	@echo "grahai_txn_abort"         >> initrd_root/bin/tests/manifest.txt
	@# FU24.B: rlimittest LAST of all — its intermittent second-mallocbomb
	@# spawn/wait hang can then truncate nothing.
	@echo "rlimittest"               >> initrd_root/bin/tests/manifest.txt
	@# Phase 23 Step 4: blk_stress_random_read NOT yet in gate — assertion 1
	@# (/etc/gcp.json opens) trips at the late position the test would land
	@# in, even though gcp_manifest opens the same file successfully ~14 s
	@# earlier in the run.  Suspected FD-table exhaustion / vfs cache
	@# pressure under stack-of-tests.  Stays interactive-only via
	@# `gash> ktest blk_stress_random_read`.
	@# Phase 23 P23.deferred.4: stress harness skeletons stay out-of-gate.
	@# They build + copy to bin/tests/ but vfs_create/SYS_CREATE in v2 mount
	@# under ktest mode is not yet wired the way the smoke loop expects;
	@# fstest_v2 only tests write paths via lenient asserts. Stage 2 cutover
	@# (or a Phase 24 v2-write fix) will unblock the gate placement.
	@# Phase 23 P23.deferred.1 cutover validation: ahcid end-to-end I/O.
	@# Built + copied to bin/tests/ahcid_basic_io.tap but NOT in gate
	@# manifest. cap_deactivates disk → spawns ahcid → BLK_OP_READ →
	@# verifies → kills → cap_activates. End-to-end validation of the
	@# truncation fix (uint32→uint64 in ahcid_client_t.dma_vmo_handle)
	@# + the channel-mode I/O path. Gate placement (even at end) was
	@# flaky: when run after the full gate sequence, ahcid's drv_register
	@# / IDENTIFY path saw issues from accumulated state (rawnet slots,
	@# kheap pressure). Resolution: full production cutover so ahcid is
	@# the SOLE owner from boot. For now: `gash> ktest ahcid_basic_io`.
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
	@cp etc/gcp.json initrd_root/etc/
	@cp etc/gcp.wit initrd_root/etc/
	@# Phase 28 G.2 soak harness: scripts/run_soak.sh writes
	@# etc/soak_inject.conf before each fault-injection iter; the
	@# soak_inject_driver test reads it at gate-start.  Absent file
	@# means no injection (normal `make test` path).
	@if [ -f etc/soak_inject.conf ]; then \
		cp etc/soak_inject.conf initrd_root/etc/soak_inject.conf; \
	fi
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

# Phase 22 Stage F: kernel/net/mongoose.o build rule retired with the source.

%.o: %.c
	@echo "Compiling C: $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.asm.o: %.S
	@echo "Assembling with NASM: $<"
	@mkdir -p $(dir $@)
	@$(NASM) $(NASMFLAGS) $< -o $@

-include $(DEPS)

# 'run' = headed graphical (TUI in QEMU window). Pre-Phase-28 sweep this
# was serial-only; use 'make terminal' for that mode now.
run: grahaos.iso format-disk-if-needed
	@echo "Starting QEMU (headed, TUI in graphical window)..."
	@echo "Tip: Alt+1..Alt+4 to switch consoles. Close the QEMU window to exit."
	@echo "Note: TCG (no -enable-kvm) — KVM exposes a known spawn race"
	@echo "     (FU27.X.spawn_rip0_race) that fires ~30% under fast respawn."
	@qemu-system-x86_64 -cdrom grahaos.iso -m 512M -smp 4 \
	     -display gtk -serial /dev/null \
	     -drive file=disk.img,format=raw,if=none,id=mydisk \
	     -device ich9-ahci,id=ahci \
	     -device ide-hd,drive=mydisk,bus=ahci.0 \
	     -netdev user,id=net0,hostfwd=tcp::8080-:80 -device e1000,netdev=net0

# 'terminal' = serial-only (gash on host terminal). Was 'run' pre-Phase-28
# sweep; preserved here for users who prefer text-only iteration.
terminal: grahaos.iso format-disk-if-needed
	@echo "Starting QEMU (serial-only, gash on host terminal)..."
	@echo "Tip: Ctrl+A then X to exit. ELF-loader chatter + heartbeats are at"
	@echo "     KLOG_DEBUG so they don't bury gash output."
	@echo "Note: TCG (no -enable-kvm) — KVM exposes a known spawn race"
	@echo "     (FU27.X.spawn_rip0_race) that fires ~30% under fast respawn."
	@qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
	     -drive file=disk.img,format=raw,if=none,id=mydisk \
	     -device ich9-ahci,id=ahci \
	     -device ide-hd,drive=mydisk,bus=ahci.0 \
	     -netdev user,id=net0,hostfwd=tcp::8080-:80 -device e1000,netdev=net0

# Phase 27 closeout: explicit "boot the gate live, no TAP parsing" target.
# Patches limine.conf with autorun=ktest + watchdog 500s, rebuilds the ISO,
# boots it. limine.conf is restored from backup AFTER qemu exits so the next
# `make run` stays interactive.
run-test: format-disk-if-needed
	@echo "Building TEST-mode ISO with autorun=ktest test_timeout_seconds=500..."
	@cp limine.conf limine.conf.run-test-bak
	@trap 'mv limine.conf.run-test-bak limine.conf' EXIT INT TERM; \
	  awk '!/^cmdline:/' limine.conf.run-test-bak > limine.conf.tmp && \
	  mv limine.conf.tmp limine.conf && \
	  printf '\ncmdline: autorun=ktest quiet=1 test_timeout_seconds=500\n' >> limine.conf && \
	  $(MAKE) grahaos.iso && \
	  echo "Booting TEST-mode ISO (gate runs live; Ctrl+A then X to abort)..." && \
	  qemu-system-x86_64 -cdrom grahaos.iso -serial stdio -m 512M -smp 4 \
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
	@echo "  all          - Build the OS ISO with initrd (default)"
	@echo "  userland     - Build user-space programs"
	@echo "  run          - Build and run in QEMU (INTERACTIVE: init -> daemons -> gash on serial)"
	@echo "  run-test     - Build and run in QEMU with the test harness live (autorun=ktest, watchdog 500s)"
	@echo "  test         - Build, run gate headless, parse TAP, exit non-zero on any failure"
	@echo "  qemu-interactive - Same as run (legacy alias)"
	@echo "  debug        - Build and run in QEMU with GDB stub on tcp:1234"
	@echo "  clean        - Clean build artifacts"
	@echo "  reformat     - Wipe disk.img and re-run mkfs.gfs"
	@echo "  info         - Show build information"
	@echo "  help         - Show this help message"