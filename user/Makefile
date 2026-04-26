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
PROGRAMS := grahai gash libctest sbrk_test printf_test spawntest cantest metatest eventtest nettest httptest dnstest aitest fdtest pipetest simtest clustertest ktest klog memstat caps auditq can-ctl fsversions ulimit mallocbomb schedbench schedbench_worker init drvctl drivers/e1000d drivers/ahcid blkctl netd ifconfig ping

# Phase 12: TAP 1.4 test binaries. Each lives at user/tests/NAME.c
# and is copied to initrd as /bin/tests/NAME.tap by the top-level
# Makefile. Every TAP test links against libtap.a + libc.a.
TAP_TESTS := tests/libtap_selftest tests/sentinel_pass \
             tests/libctest tests/simtest tests/clustertest tests/pipetest \
             tests/fdtest tests/metatest tests/spawntest \
             tests/nettest tests/httptest tests/dnstest \
             tests/ktest_discovery tests/ktest_capture tests/cmdline_parse \
             tests/klog_basic tests/klog_stress tests/panic_test tests/kpf_test \
             tests/slab_basic tests/kheap_basic tests/percpu_basic tests/mem_stress \
             tests/captest_v2 tests/pledgetest tests/audittest \
             tests/cantest_v2 tests/canstress \
             tests/chantest tests/vmotest tests/streamtest \
             tests/fstest_v2 tests/schedtest tests/rlimittest \
             tests/userdrv tests/e1000dtest \
             tests/chantest_named \
             tests/netd_arp tests/netd_ipv4 tests/netd_tcp tests/netd_dhcp \
             tests/netd_dns tests/netd_service \
             tests/libhttp_parse \
             tests/tcp_fuzz \
             tests/gcp_manifest \
             tests/libtls \
             tests/tcp_stress_1000 \
             tests/userdrv_respawn_stress
# Phase 23 S7.1: ahcid_register builds but is kept OUT of the gate
# sequence intentionally — it spawns /bin/ahcid which would compete with
# the kernel-resident AHCI driver for hardware ownership. The full cutover
# (kernel strip + channel-mediated FS I/O) is the production close item.
# Run interactively from `gash> ahcid_register` after boot.
TAP_TESTS += tests/ahcid_register
# Phase 23 P23.deferred.4: stress harnesses. Built and copied to bin/tests/
# but NOT in the gate manifest:
#   - blk_stress_kheap         — sustained FS load + kheap delta < 500 KB
#   - blk_stress_random_read   — 4 KB random-read p99 latency benchmark
#   - userdrv_respawn_100      — 100-cycle e1000d spawn-bind-kill stress
# Each runs interactively from `gash> ktest <name>`. Meaningful only when
# the channel-mediated path (Stage 2 cutover) is active for the FS-touching
# tests; the userdrv_respawn_100 test runs against the existing framework.
TAP_TESTS += tests/blk_stress_kheap
TAP_TESTS += tests/blk_stress_random_read
TAP_TESTS += tests/userdrv_respawn_100
# Phase 23 P23.deferred.1 cutover validation: ahcid_basic_io exercises the
# end-to-end I/O path (connect → handshake with VMO handle → BLK_OP_READ
# → response → DMA verification). Wraps cap_deactivate("disk") around the
# spawn so kernel-resident AHCI doesn't fight ahcid for the controller.
# Built + copied to bin/tests/; gate inclusion validated separately.
TAP_TESTS += tests/ahcid_basic_io
# Phase 16: cantest (Phase 8b) and eventtest (Phase 8d) removed — both used
# SYS_CAP_ACTIVATE / SYS_CAP_WATCH which now return -EDEPRECATED. cantest_v2
# replaces cantest's coverage using token-taking syscalls; Phase 17 channels
# supersede eventtest's notification API. canstress cycles the keyboard cap
# 100× to catch stuck IRQs / state drift.

# Phase 12 work unit 15: sentinel_fail is only built when the meta-test
# of the harness is being run. `make test-sentinel-meta` sets this.
ifeq ($(WITH_SENTINEL_FAIL),1)
TAP_TESTS += tests/sentinel_fail
endif

AR := $(PREFIX)/bin/$(TARGET)-ar
LIBTAP := libtap.a
# Phase 22 Stage A: userspace named-channel helper library. Linked into
# every binary that talks to netd (ifconfig, ping, httptest, grahai, ...)
# and — internally — into netd itself for its publish flow.
LIBNET := libnet/libnet.a
# Phase 22 Stage B: netd's protocol modules (Ethernet + ARP + IPv4/checksum).
# Bundled as a static archive so (a) netd links them without per-binary
# dependency bookkeeping and (b) tests/netd_*.tap can reuse the exact same
# objects the daemon runs against (no duplicate implementations).
LIBNETD := libnetd.a
# Phase 22 Stage D U17b: HTTP/1.1 client (plaintext; HTTPS via libtls-mg in U19).
LIBHTTP := libhttp/libhttp.a

.PHONY: all clean libc

all: libc $(LIBTAP) $(LIBNET) $(LIBNETD) $(LIBHTTP) $(LIBTLS) $(PROGRAMS) $(TAP_TESTS)

# Build libc first
libc:
	@echo "Building libc..."
	@$(MAKE) -C ../libc

# Phase 12: TAP producer library. Linked into every tests/*.tap binary.
$(LIBTAP): libtap.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 22 Stage A/C: libnet.a holds the named-channel helpers (publish_service,
# connect_service_with_retry, service_accept, Stage A) + the GCP message
# types + client helpers added in Stage C (libnet_msg.o).
$(LIBNET): libnet/libnet.o libnet/libnet_msg.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 22 Stage B: libnetd.a — netd's protocol modules. Currently Ethernet
# framing + ARP (RFC 826) + IPv4 helpers (RFC 1071 checksum + dotted-quad
# parse). Subsequent sub-units add netd_ipv4.o's packet path, netd_icmp.o,
# netd_udp.o, netd_tcp.o, netd_dhcp.o.
$(LIBNETD): netd_eth.o netd_arp.o netd_ipv4.o netd_icmp.o netd_udp.o netd_tcp.o netd_dhcp.o netd_dns.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 22 Stage D U17b: libhttp.a — HTTP/1.1 client built on libnet TCP.
# Weak-links libtls_* symbols so it compiles without libtls-mg.a; the HTTPS
# path short-circuits to -EPROTO when the TLS library is absent.
$(LIBHTTP): libhttp/libhttp.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 22 Stage D U18: libtls-mg.a — Mongoose-backed TLS 1.3 client. The
# Stage D landing ships the public API + shim architecture + the
# deterministic extraction tooling in user/libtls-mg/extract.py; the
# extracted Mongoose core (mongoose_tls_core.c) is a placeholder until the
# full ~8 kLOC extraction lands. Callers that pull libhttp get the shim +
# core linked in automatically so HTTPS paths compile even when the real
# TLS code is not yet present.
LIBTLS := libtls-mg/libtls-mg.a
# Phase 22 closeout (G1.3): mg_stubs.o provides trip-wire stubs for the
# Mongoose HTTP / event-loop functions that the amalgamated mongoose.c
# references but TLS-only code never reaches.  Linker resolves them so
# libtls-mg.a actually links into clients.
$(LIBTLS): libtls-mg/libtls.o libtls-mg/libtls_shim.o libtls-mg/mongoose_tls_core.o libtls-mg/mg_stubs.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 12: generic rule for tests/NAME: tests/NAME.o libtap.a libc.a
tests/%: tests/%.o $(LIBTAP) $(LIBC)
	@echo "Linking TAP test: $@"
	@$(LD) $(LDFLAGS) -o $@ tests/$*.o $(LIBTAP) $(LIBC)

# Phase 22 Stage B: netd_arp / netd_ipv4 also link libnetd.a so they can
# drive ARP / Ethernet / IPv4 / ICMP / UDP helpers directly without
# spawning the daemon.
tests/netd_arp: tests/netd_arp.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_arp.o $(LIBNETD) $(LIBTAP) $(LIBC)

tests/netd_ipv4: tests/netd_ipv4.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_ipv4.o $(LIBNETD) $(LIBTAP) $(LIBC)

tests/netd_tcp: tests/netd_tcp.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_tcp.o $(LIBNETD) $(LIBTAP) $(LIBC)

tests/netd_dhcp: tests/netd_dhcp.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_dhcp.o $(LIBNETD) $(LIBTAP) $(LIBC)

# Phase 22 Stage C: DNS wire helpers (netd_dns.c) — pure unit tests,
# linked only against libnetd. No channel / daemon deps.
tests/netd_dns: tests/netd_dns.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_dns.o $(LIBNETD) $(LIBTAP) $(LIBC)

# Phase 22 Stage E U22: tcp_fuzz exercises netd_tcp.c against an adversarial
# segment corpus + RFC 5961 hardening + 1000-SYN flood + pool exhaustion.
# Pure libnetd test; no daemon, no wire.
tests/tcp_fuzz: tests/tcp_fuzz.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/tcp_fuzz.o $(LIBNETD) $(LIBTAP) $(LIBC)

# Phase 22 closeout (G4.3): gcp_manifest validates etc/gcp.json against the
# kernel runtime manifest + the 5 retired-syscall ENOSYS contract.
tests/gcp_manifest: tests/gcp_manifest.o $(LIBTAP) $(LIBC)
	@echo "Linking TAP test: $@"
	@$(LD) $(LDFLAGS) -o $@ tests/gcp_manifest.o $(LIBTAP) $(LIBC)

# Phase 22 closeout (G1.6): libtls.tap exercises the libtls-mg.a crypto
# primitives (SHA-256, X25519) against RFC vectors.  --whole-archive on
# $(LIBTLS) so the linker pulls in mongoose_tls_core.o despite the test
# code only having weak references to libtls_connect (via libhttp's decl).
tests/libtls: tests/libtls.o $(LIBTAP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libtls): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/libtls.o --whole-archive $(LIBTLS) --no-whole-archive $(LIBNET) $(LIBTAP) $(LIBC)

# Phase 22 closeout (G6): offline 1000-socket stress test against
# netd_tcp.c.  In-process — no daemons, no wire.  Live-stack 1000-conn
# harness is `make TEST_HARNESS=1 test-stress-1000` (qemu-interactive).
tests/tcp_stress_1000: tests/tcp_stress_1000.o $(LIBTAP) $(LIBNETD) $(LIBC)
	@echo "Linking TAP test (with libnetd): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/tcp_stress_1000.o $(LIBNETD) $(LIBTAP) $(LIBC)

# Phase 22 Stage C: /sys/net/service wire layout + libnet helper coverage.
# Links libnet (for libnet_msg.o pack/unpack) + libtap + libc.
tests/netd_service: tests/netd_service.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libnet): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/netd_service.o $(LIBNET) $(LIBTAP) $(LIBC)

# Phase 22 Stage D U17b: libhttp URL / status / headers / chunked offline coverage.
tests/libhttp_parse: tests/libhttp_parse.o $(LIBTAP) $(LIBHTTP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libhttp): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/libhttp_parse.o $(LIBHTTP) $(LIBNET) $(LIBTAP) $(LIBC)

# Phase 22 Stage E U21a: TAP versions of nettest / httptest / dnstest now
# link libnet (net_query against /sys/net/service) and libhttp (HTTP GET).
# The pre-Phase-22 paths (SYS_NET_IFCONFIG / SYS_NET_STATUS / SYS_HTTP_GET /
# SYS_DNS_RESOLVE) are scheduled for -ENOSYS in Stage F.
tests/nettest: tests/nettest.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libnet): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/nettest.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/httptest: tests/httptest.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libnet): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/httptest.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/dnstest: tests/dnstest.o $(LIBTAP) $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libhttp + libtls + libnet): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/dnstest.o $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBTAP) $(LIBC)

# Build grahai with its JSON parser. Phase 22 Stage E U21c: links
# libhttp + libnet. Phase 22 closeout (G1.5): adds libtls so the HTTPS
# branch in libhttp resolves to the real handshake driver, not the weak
# -EPROTO fallback.  Link order: caller (libhttp) → callee (libtls) →
# transport (libnet) → libc.
grahai: grahai.o json.o $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ grahai.o json.o $(LIBHTTP) --whole-archive $(LIBTLS) --no-whole-archive $(LIBNET) $(LIBC)

# Build gash shell (with JSON parser for AI command). Phase 22 closeout
# (G1.5): adds libtls so `gash> ai ...` actually exercises HTTPS to
# generativelanguage.googleapis.com instead of returning -EPROTO.
# --whole-archive forces the linker to pull libtls-mg.a in even though
# libhttp.c declares libtls_* as weak symbols (which the linker would
# otherwise resolve to NULL and skip the archive).
gash: gash.o json.o $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ gash.o json.o $(LIBHTTP) --whole-archive $(LIBTLS) --no-whole-archive $(LIBNET) $(LIBC)

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

# Phase 22 Stage E U21a: nettest is a libnet/net_query client now —
# replaces the deleted SYS_NET_IFCONFIG path.
nettest: nettest.o $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ nettest.o $(LIBNET) $(LIBC)

# Phase 22 Stage E U21a: httptest swaps SYS_NET_STATUS for libnet/net_query.
httptest: httptest.o $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ httptest.o $(LIBNET) $(LIBC)

# Phase 22 Stage E U21a: dnstest uses libhttp http_get + libnet_dns_resolve.
# Phase 22 closeout (G1.5): adds libtls so any future https://... probe in
# dnstest resolves cleanly without the -EPROTO weak-symbol path.
dnstest: dnstest.o $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ dnstest.o $(LIBHTTP) --whole-archive $(LIBTLS) --no-whole-archive $(LIBNET) $(LIBC)

# Phase 22 Stage E U21b: aitest uses libhttp http_post against Gemini
# (HTTPS).  Phase 22 closeout (G1.5): libtls now linked via --whole-archive
# so the weak-symbol fallback in libhttp resolves to the shim's real impl.
aitest: aitest.o $(LIBHTTP) $(LIBTLS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ aitest.o $(LIBHTTP) --whole-archive $(LIBTLS) --no-whole-archive $(LIBNET) $(LIBC)

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

# Phase 12: TAP test runner (PID 1 in autorun mode).
ktest: ktest.o $(LIBTAP) $(LIBC)
	@echo "Linking user program: $@ (with libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ ktest.o $(LIBTAP) $(LIBC)

# Phase 13: standalone /bin/klog reader.
klog: klog.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ klog.o $(LIBC)

# Phase 14: /bin/memstat — allocator stats reader.
memstat: memstat.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ memstat.o $(LIBC)

# Phase 15a: /bin/caps — capability inspector.
caps: caps.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ caps.o $(LIBC)

# Phase 15b: /bin/auditq — audit log reader.
auditq: auditq.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ auditq.o $(LIBC)

# Phase 16: /bin/can-ctl — capability-network table printer.
can-ctl: can-ctl.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ can-ctl.o $(LIBC)

# Phase 19: /bin/fsversions — version-chain lister.
fsversions: fsversions.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ fsversions.o $(LIBC)

# Phase 21: /bin/init — supervisor / PID 1 candidate. Reads /etc/init.conf,
# spawns daemons + the interactive child, respawns daemons on death.
init: init.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ init.o $(LIBC)

# Phase 21: /bin/drvctl — driver control CLI (list/info/kill).
drvctl: drvctl.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ drvctl.o $(LIBC)

# Phase 21.1: /bin/e1000d — userspace E1000 NIC driver daemon.
# Links libdriver.o (shared substrate helpers) + libc + Phase 22 libnet.a
# (for the new /sys/net/rawframe publish path, running in parallel with the
# Phase 21.1 kernel-proxy path until Stage F retires the proxy).
drivers/e1000d: drivers/e1000d.o libdriver.o $(LIBNET) $(LIBC)
	@echo "Linking driver daemon: $@ (with libdriver + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ drivers/e1000d.o libdriver.o $(LIBNET) $(LIBC)

# Phase 23 S3: /bin/ahcid — userspace AHCI driver daemon. Mirrors e1000d
# pattern: drv_register, drv_mmio_map, drv_irq_wait main loop. Publishes
# /sys/blk/service for kernel blk_client + tests + blkctl to connect.
drivers/ahcid: drivers/ahcid.o libdriver.o $(LIBC)
	@echo "Linking driver daemon: $@ (with libdriver + libc)"
	@$(LD) $(LDFLAGS) -o $@ drivers/ahcid.o libdriver.o $(LIBC)

# Phase 23 S6: /bin/blkctl — block-device CLI, twin of drvctl from Phase 21.
blkctl: blkctl.o $(LIBC)
	@echo "Linking blkctl"
	@$(LD) $(LDFLAGS) -o $@ blkctl.o $(LIBC)

# Phase 22 Stage A/B: /bin/netd — userspace TCP/IP daemon. Stage A proves
# publish/connect rendezvous with e1000d. Stage B layers ARP (already
# shipped in libnetd.a) + IPv4 + ICMP + UDP + TCP-Reno + DHCP.
netd: netd.o $(LIBNET) $(LIBNETD) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libnetd + libc)"
	@$(LD) $(LDFLAGS) -o $@ netd.o $(LIBNET) $(LIBNETD) $(LIBC)

# Phase 22 Stage C: /bin/ifconfig — channel-RPC client (net_query_req) that
# replaces the old SYS_NET_IFCONFIG syscall path. Minimal libnet + libc only.
ifconfig: ifconfig.o $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ ifconfig.o $(LIBNET) $(LIBC)

# Phase 22 Stage C: /bin/ping — ICMP echo client over /sys/net/service.
# Links libnetd for netd_ipv4_parse_dotted helper.
ping: ping.o $(LIBNET) $(LIBNETD) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libnetd + libc)"
	@$(LD) $(LDFLAGS) -o $@ ping.o $(LIBNET) $(LIBNETD) $(LIBC)

# Phase 20: /bin/ulimit — resource-limit CLI (pledge-gated via SETRLIMIT).
ulimit: ulimit.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ ulimit.o $(LIBC)

# Phase 20: /bin/mallocbomb — tiny brk-until-fail test program for AW-20.2.
mallocbomb: mallocbomb.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ mallocbomb.o $(LIBC)

# Phase 20: /bin/schedbench — scheduler balance + throughput benchmark.
schedbench: schedbench.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ schedbench.o $(LIBC)

# Phase 20: /bin/schedbench_worker — tight compute loop used by schedbench.
schedbench_worker: schedbench_worker.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ schedbench_worker.o $(LIBC)

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f *.o $(PROGRAMS) $(LIBTAP) tests/*.o $(TAP_TESTS)