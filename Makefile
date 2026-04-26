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
initrd.tar: userland etc/motd.txt etc/plan.json etc/gcp.json
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
	@# Phase 22 closeout (G6): offline 1000-socket TCP stress test.
	@cp user/tests/tcp_stress_1000  initrd_root/bin/tests/tcp_stress_1000.tap
	@# Phase 23 S1: userdrv MMIO/IRQ/chan synchronous-cleanup stress (P22.G.4).
	@cp user/tests/userdrv_respawn_stress initrd_root/bin/tests/userdrv_respawn_stress.tap
	@# Phase 23 S7.1: ahcid registration smoke test (interactive only;
	@# spawns ahcid which competes with kernel AHCI — not in gate sequence).
	@cp user/tests/ahcid_register   initrd_root/bin/tests/ahcid_register.tap
	@# Phase 23 P23.deferred.4: stress harnesses. Built + copied; NOT in
	@# the gate manifest. Run interactively via `gash> ktest <name>`.
	@cp user/tests/blk_stress_kheap        initrd_root/bin/tests/blk_stress_kheap.tap
	@cp user/tests/blk_stress_random_read  initrd_root/bin/tests/blk_stress_random_read.tap
	@cp user/tests/userdrv_respawn_100     initrd_root/bin/tests/userdrv_respawn_100.tap
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
	@echo "daemon=bin/e1000d:net_server,sys_control,sys_query,ipc_send,ipc_recv" >> initrd_root/etc/init.conf
	@echo "daemon=bin/netd:net_server,net_client,ipc_send,ipc_recv,sys_query,fs_read,compute,time" >> initrd_root/etc/init.conf
ifdef TEST_HARNESS
	@# G6.4 — echod loopback echo daemon for tcp_stress_1000 harness.
	@# Production boot does NOT carry this; only `make TEST_HARNESS=1 ...`.
	@echo "daemon=bin/echod:net_server,net_client,ipc_send,ipc_recv" >> initrd_root/etc/init.conf
endif
	@echo "autorun=bin/gash" >> initrd_root/etc/init.conf
	@# Phase 22 closeout (G1.4): ship the Mozilla NSS root CA bundle so
	@# libtls-mg can validate TLS server certificates.  ~224 KiB / 146 roots.
	@mkdir -p initrd_root/etc/tls
	@cp user/libtls-mg/assets/trust.pem initrd_root/etc/tls/trust.pem
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
	@# Phase 22 closeout (G6): 1000-socket TCP stress.
	@echo "tcp_stress_1000" >> initrd_root/bin/tests/manifest.txt
	@# Phase 23 S1: userdrv_respawn_stress builds and is available as
	@# bin/tests/userdrv_respawn_stress.tap, run interactively via
	@# `gash> ktest userdrv_respawn_stress`. NOT in the gate sequence —
	@# 10 back-to-back spawn-kill cycles of e1000d temporarily corrupt
	@# kheap/PMM accounting in ways subsequent tests trip on (gate
	@# isolation needs Phase 23 S1 production cutover before this test
	@# can run inline).
	@# Phase 23 P23.deferred.2 (partial): ahci_restore_after_userdrv_death
	@# saves PxCLB/PxFB at port_rebase and restores them when an AHCI-class
	@# userdrv owner dies. This makes the kernel-resident ahci_read/write
	@# functional again after ahcid_register's spawn-kill cycle. Useful for
	@# interactive use (`gash> ktest ahcid_register; gash> ktest fstest_v2`
	@# now works back-to-back). Moving ahcid_register INTO the gate
	@# manifest, however, exposes additional state-coupling (e1000dtest
	@# subsequently saw a flake) — the full gate move requires the
	@# Phase 23 production cutover to land first. For now ahcid_register
	@# stays interactive-only; the mitigation lives in the kernel ready
	@# for the cutover to leverage.
	@echo "vmotest" >> initrd_root/bin/tests/manifest.txt
	@echo "streamtest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 19: versioned GrahaFS v2.
	@echo "fstest_v2" >> initrd_root/bin/tests/manifest.txt
	@echo "schedtest" >> initrd_root/bin/tests/manifest.txt
	@echo "rlimittest" >> initrd_root/bin/tests/manifest.txt
	@# Phase 21: userdrv substrate tests (kernel-side framework only;
	@# full e1000d daemon test in e1000dtest below).
	@echo "userdrv" >> initrd_root/bin/tests/manifest.txt
	@# Phase 21.1: e1000dtest — end-to-end test for the userspace e1000d
	@# daemon + e1000_proxy. Spawns /bin/e1000d, asserts ANNOUNCE round-trip,
	@# kills + respawns to verify the death-cleanup path.
	@cp user/tests/e1000dtest        initrd_root/bin/tests/e1000dtest.tap
	@echo "e1000dtest" >> initrd_root/bin/tests/manifest.txt
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