# Makefile for GrahaOS Userland Programs (Phase 6c)

# Inherit from parent Makefile (which exports PREFIX + TARGET).
# When invoked standalone (`cd user && make`), fall back to a local
# toolchain at <repo>/toolchain so cloned repos work without env setup.
PREFIX ?= $(abspath $(CURDIR)/../toolchain)
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
PROGRAMS := grahai gash libctest sbrk_test printf_test spawntest cantest metatest eventtest nettest httptest dnstest aitest fdtest pipetest simtest clustertest ktest klog memstat caps auditq can-ctl fsversions ulimit mallocbomb schedbench schedbench_worker init drvctl drivers/e1000d drivers/ahcid blkctl netd ifconfig ping snapshot txnctl assertpledge wasm fbd/fbd wasmd/wasmd wasmd/wasmd_worker tui_demo tui_anim tui_demo_full gsh

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
             tests/snaptest \
             tests/snap_stress_cycle \
             tests/snap_cow_storm \
             tests/snap_restore_self \
             tests/txn_buffer_send \
             tests/txn_basic_commit tests/txn_basic_abort \
             tests/txn_nested_basic tests/txn_nest_limit \
             tests/cap_system \
             tests/pledge_narrow_exec \
             tests/wasmtest \
             tests/wasm_e2e_run \
             tests/wasm_fault_trap \
             tests/wasm_load_reject \
             tests/wasm_concurrent_serial \
             tests/vsnprintftest \
             tests/console_init \
             tests/keyboard_alt \
             tests/fbd_render \
             tests/tui_render \
             tests/console_sprite \
             tests/console_gfx \
             tests/audit_stream \
             tests/audit_plan_codes \
             tests/manifest_export \
             tests/rlimit_syscall_rate \
             tests/cap_inherit \
             tests/cap_recursive_inheritance \
             tests/cap_recursive_inherit_child \
             tests/inject_kmalloc_fail \
             tests/inject_pmm_fail \
             tests/inject_chan_fail \
             tests/inject_spinlock_timeout \
             tests/soak_inject_apply \
             tests/gcp_manifest_export_full \
             tests/gsh_completion \
             tests/gsh_chrome \
             tests/ai_txn_rollback \
             tests/spawn_argv \
             tests/audit_query_since \
             tests/spawn_handles_inherit \
             tests/txn_replay_order \
             tests/txn_abort_drops \
             tests/txn_exit_cleanup \
             tests/txn_commit_retry \
             tests/txn_concurrent_abort \
             tests/txn_buffer_overflow \
             tests/txn_child_abort_parent_commit \
             tests/txn_fault_during_replay \
             tests/console_read_input \
             tests/console_attach_map \
             tests/fb_mmio_map \
             tests/vsync_wait \
             tests/dirty_rect \
             tests/sprite_anim \
             tests/cell_grid_atomic \
             tests/mouse_basic \
             tests/font_full_sweep \
             tests/cpu_affinity \
             tests/spinlock_timeout \
             tests/rate_quota \
             tests/gash_txn_commit \
             tests/gash_txn_abort \
             tests/gash_txn_abort_fs_revert \
             tests/grahai_txn_commit \
             tests/grahai_txn_abort \
             tests/txn_stress_basic tests/txn_stress_nested \
             tests/txn_stress_state_machine \
             tests/netd_arp tests/netd_ipv4 tests/netd_tcp tests/netd_dhcp \
             tests/netd_dns tests/netd_service \
             tests/libhttp_parse \
             tests/tcp_fuzz \
             tests/gcp_manifest \
             tests/libtls \
             tests/libtls_handshake \
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
# Phase 24a PRE: blk_micro_latency — measurement ruler for the 7-layer IPC
# optimization stack (W1 doorbell IPI, W2 chan_send fastpath, W3 batching,
# W5 SPSC ring, W6 zero-copy DMA, W7 same-CPU affinity, W8 lockless audit).
# Profiling tool, NOT in gate. Run interactively from
# `gash> ktest blk_micro_latency` after each layer commit; capture printf
# output to problems/phase24a/baselines.json.
TAP_TESTS += tests/blk_micro_latency
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
# Phase 27 Block A (Stage A5): libtui userspace TUI primitives library.
# Linked into apps that drive the virtual consoles (tui_demo, gsh in P28).
LIBTUI := libtui/libtui.a
# Phase 29 Session B (FU28.A): libtls userspace TLS 1.2/1.3 client.
# Substrate-only landing: provides the public API surface (libtls_init /
# connect / send / recv / close) over a stub that returns -ENOSYS.  Once
# vendor/bearssl/ is wired (scripts/vendor-bearssl.sh), libtls links
# against libbearssl.a and libhttp swaps off libtls-mg.a.
LIBTLS_NEW := libtls/libtls.a

# Phase 29 FU29.B.bearssl_wire: the BearSSL-backed TLS link group.  This
# REPLACES the dead Phase 22 libtls-mg.a ($(LIBTLS)) in every HTTPS consumer
# (grahai / gash / aitest / dnstest + the libtls tests).  --whole-archive
# forces the strong libtls_* symbols out of libtls.a so they resolve
# libhttp's weak references; trust_anchors.o supplies g_grahaos_TAs;
# libbearssl.a provides the crypto engine.  Link order in each consumer:
#   caller (libhttp) -> $(TLS_LINK) -> transport (libnet) -> libc.
# (LIBBEARSSL is defined alongside its build rule further below.)
LIBBEARSSL := ../vendor/bearssl/libbearssl.a
TLS_LINK := --whole-archive $(LIBTLS_NEW) libtls/trust_anchors.o --no-whole-archive $(LIBBEARSSL)
TLS_DEPS := $(LIBTLS_NEW) libtls/trust_anchors.o $(LIBBEARSSL)

.PHONY: all clean libc

all: libc $(LIBTAP) $(LIBNET) $(LIBNETD) $(LIBHTTP) $(LIBTLS) $(LIBBEARSSL) libtls/trust_anchors.o $(LIBTLS_NEW) $(LIBTUI) $(LIBWASM3) $(PROGRAMS) $(TAP_TESTS)

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

# Phase 27 Block A (Stage A5): libtui.a — ncurses-like TUI primitives over
# the Phase 27 console subsystem. Apps link against it; gsh in Phase 28.
$(LIBTUI): libtui/libtui.o
	@echo "Creating $@"
	@$(AR) rcs $@ $^

# Phase 29 Session B (FU28.A): libtls.a — BearSSL-backed TLS 1.2/1.3 client.
# Substrate-only: libtls.c stubs every entry point with -ENOSYS until the
# vendor/bearssl/ wiring lands.  Builds clean either way so the rest of the
# tree keeps compiling.
# libtls.c needs the BearSSL public headers; override the generic %.o rule.
libtls/libtls.o: libtls/libtls.c
	@echo "Compiling user C: $< (BearSSL backend)"
	@$(CC) $(CFLAGS) -I../vendor/bearssl/inc -c $< -o $@

$(LIBTLS_NEW): libtls/libtls.o
	@echo "Creating $@ (libtls — BearSSL backend)"
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

# FU27.WASM Stage D0: vendored wasm3 v0.5.0 (MIT). Built into libwasm3.a
# from vendor/wasm3/source/ — 11 core files (m3_bind, m3_code, m3_compile,
# m3_core, m3_emit, m3_env, m3_exec, m3_function, m3_info, m3_module,
# m3_parse). The host-import shims (m3_api_*.c) are excluded; FU27.WASM
# Stage D1 provides our own host imports at user/wasmd/host_imports.c.
#
# Compile flags worth noting:
#   -Dd_m3HasFloat=0    — disable f32/f64 ops; saves libgcc 64-bit float
#                         helpers we'd otherwise have to shim. Our 5
#                         FU27.WASM fixtures are i32-only.
#   -Dd_m3LogOutput=0   — disable wasm3's verbose printf logging.
#   -Wno-int-to-pointer-cast / -Wno-sign-compare — match upstream
#                         CMakeLists.txt's per-source warning suppressions
#                         (m3_emit.c + m3_env.c).
LIBWASM3 := ../vendor/wasm3/libwasm3.a
WASM3_SRCS := m3_bind m3_code m3_compile m3_core m3_emit m3_env \
              m3_exec m3_function m3_info m3_module m3_parse
WASM3_OBJS := $(addprefix ../vendor/wasm3/obj/,$(addsuffix .o,$(WASM3_SRCS)))
WASM3_CFLAGS := $(CFLAGS) -Dd_m3HasFloat=0 -Dd_m3LogOutput=0 \
                -Wno-int-to-pointer-cast -Wno-sign-compare \
                -I../vendor/wasm3/source

../vendor/wasm3/obj:
	@mkdir -p $@

../vendor/wasm3/obj/%.o: ../vendor/wasm3/source/%.c | ../vendor/wasm3/obj
	@echo "Compiling vendor/wasm3: $<"
	@$(CC) $(WASM3_CFLAGS) -c $< -o $@

$(LIBWASM3): $(WASM3_OBJS)
	@echo "Creating $@ (FU27.WASM Stage D0; 11 core files)"
	@$(AR) rcs $@ $^

# ---------------------------------------------------------------------------
# Phase 29 FU29.B.bearssl_wire: vendored BearSSL 0.6 (MIT) built into
# libbearssl.a from vendor/bearssl/src/.  Modeled on the LIBWASM3 rule above.
#
# The file list is EXPLICIT (not a wildcard): the x86 intrinsic files
# (aes_x86ni*, chacha20_sse2, ghash_pclmul, poly1305_ctmulq) are INCLUDED
# because the "default" engine selectors reference their `_get_vtable()`
# accessors — but at -march=x86-64 their crypto bodies compile to an empty
# CPU-unsupported fallback that returns NULL, so the portable constant-time
# implementations (aes_ct*, chacha20_ct, ghash_ctmul64, poly1305_ctmul) are
# what actually run.  A wildcard would otherwise double-include nothing
# useful; we list every portable + accessor TU verified to link with zero
# undefined symbols against the SSL client engine.
#
# CFLAGS:
#   -Ivendor/bearssl/inc -Ivendor/bearssl/src — public + private headers.
#   -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0  — no host OS entropy source
#       (GrahaOS has no /dev/urandom; entropy is injected from userspace in
#       libtls.c via RDRAND+rdtsc).
#   -Wno-unused — upstream has unused statics under our feature subset.
# (LIBBEARSSL is defined near the top alongside the TLS link group.)
BEARSSL_SRCS := aead/ccm aead/gcm codec/ccopy codec/dec16be \
              codec/dec16le codec/dec32be codec/dec32le codec/dec64be \
              codec/dec64le codec/enc16be codec/enc16le codec/enc32be \
              codec/enc32le codec/enc64be codec/enc64le codec/pemdec \
              codec/pemenc ec/ec_all_m15 ec/ec_all_m31 ec/ec_c25519_i15 \
              ec/ec_c25519_i31 ec/ec_c25519_m15 ec/ec_c25519_m31 ec/ec_curve25519 \
              ec/ec_default ec/ecdsa_atr ec/ecdsa_default_sign_asn1 ec/ecdsa_default_sign_raw \
              ec/ecdsa_default_vrfy_asn1 ec/ecdsa_default_vrfy_raw ec/ecdsa_i31_bits ec/ecdsa_i31_sign_asn1 \
              ec/ecdsa_i31_sign_raw ec/ecdsa_i31_vrfy_asn1 ec/ecdsa_i31_vrfy_raw ec/ec_keygen \
              ec/ec_p256_m15 ec/ec_p256_m31 ec/ec_prime_i15 ec/ec_prime_i31 \
              ec/ec_pubkey ec/ec_secp256r1 ec/ec_secp384r1 ec/ec_secp521r1 \
              hash/dig_oid hash/dig_size hash/ghash_ctmul64 hash/ghash_ctmul \
              hash/ghash_pclmul hash/md5 hash/md5sha1 hash/mgf1 \
              hash/multihash hash/sha1 hash/sha2big hash/sha2small \
              int/i15_add int/i15_bitlen int/i15_decmod int/i15_decode \
              int/i15_decred int/i15_encode int/i15_fmont int/i15_iszero \
              int/i15_moddiv int/i15_modpow2 int/i15_modpow int/i15_montmul \
              int/i15_mulacc int/i15_muladd int/i15_ninv15 int/i15_reduce \
              int/i15_rshift int/i15_sub int/i15_tmont int/i31_add \
              int/i31_bitlen int/i31_decmod int/i31_decode int/i31_decred \
              int/i31_encode int/i31_fmont int/i31_iszero int/i31_moddiv \
              int/i31_modpow2 int/i31_modpow int/i31_montmul int/i31_mulacc \
              int/i31_muladd int/i31_ninv31 int/i31_reduce int/i31_rshift \
              int/i31_sub int/i31_tmont int/i32_div32 int/i62_modpow2 \
              kdf/hkdf mac/hmac mac/hmac_ct rand/hmac_drbg \
              rand/sysrng rsa/rsa_default_modulus rsa/rsa_default_pkcs1_sign rsa/rsa_default_pkcs1_vrfy \
              rsa/rsa_default_priv rsa/rsa_default_privexp rsa/rsa_default_pub rsa/rsa_default_pubexp \
              rsa/rsa_i31_modulus rsa/rsa_i31_pkcs1_sign rsa/rsa_i31_pkcs1_vrfy rsa/rsa_i31_priv \
              rsa/rsa_i31_privexp rsa/rsa_i31_pub rsa/rsa_i31_pubexp rsa/rsa_i62_keygen \
              rsa/rsa_i62_oaep_decrypt rsa/rsa_i62_oaep_encrypt rsa/rsa_i62_pkcs1_sign rsa/rsa_i62_pkcs1_vrfy \
              rsa/rsa_i62_priv rsa/rsa_i62_pub rsa/rsa_pkcs1_sig_pad rsa/rsa_pkcs1_sig_unpad \
              rsa/rsa_ssl_decrypt settings ssl/prf ssl/prf_md5sha1 \
              ssl/prf_sha256 ssl/prf_sha384 ssl/ssl_ccert_single_ec ssl/ssl_ccert_single_rsa \
              ssl/ssl_client ssl/ssl_client_default_rsapub ssl/ssl_client_full ssl/ssl_engine \
              ssl/ssl_engine_default_aescbc ssl/ssl_engine_default_aesccm ssl/ssl_engine_default_aesgcm ssl/ssl_engine_default_chapol \
              ssl/ssl_engine_default_descbc ssl/ssl_engine_default_ec ssl/ssl_engine_default_ecdsa ssl/ssl_engine_default_rsavrfy \
              ssl/ssl_hashes ssl/ssl_hs_client ssl/ssl_io ssl/ssl_keyexport \
              ssl/ssl_lru ssl/ssl_rec_cbc ssl/ssl_rec_ccm ssl/ssl_rec_chapol \
              ssl/ssl_rec_gcm ssl/ssl_scert_single_ec ssl/ssl_scert_single_rsa symcipher/aes_common \
              symcipher/aes_ct64 symcipher/aes_ct64_cbcdec symcipher/aes_ct64_cbcenc symcipher/aes_ct64_ctr \
              symcipher/aes_ct64_ctrcbc symcipher/aes_ct64_dec symcipher/aes_ct64_enc symcipher/aes_ct \
              symcipher/aes_ct_cbcdec symcipher/aes_ct_cbcenc symcipher/aes_ct_ctr symcipher/aes_ct_ctrcbc \
              symcipher/aes_ct_dec symcipher/aes_ct_enc symcipher/aes_x86ni symcipher/aes_x86ni_cbcdec \
              symcipher/aes_x86ni_cbcenc symcipher/aes_x86ni_ctr symcipher/aes_x86ni_ctrcbc symcipher/chacha20_ct \
              symcipher/chacha20_sse2 symcipher/des_ct symcipher/des_ct_cbcdec symcipher/des_ct_cbcenc \
              symcipher/des_support symcipher/des_tab symcipher/des_tab_cbcdec symcipher/des_tab_cbcenc \
              symcipher/poly1305_ctmul32 symcipher/poly1305_ctmul symcipher/poly1305_ctmulq symcipher/poly1305_i15 \
              x509/asn1enc x509/encode_ec_pk8der x509/encode_ec_rawder x509/encode_rsa_pk8der \
              x509/encode_rsa_rawder x509/skey_decoder x509/x509_decoder x509/x509_knownkey \
              x509/x509_minimal x509/x509_minimal_full
BEARSSL_OBJS := $(addprefix ../vendor/bearssl/obj/,$(addsuffix .o,$(BEARSSL_SRCS)))
BEARSSL_CFLAGS := $(CFLAGS) -I../vendor/bearssl/inc -I../vendor/bearssl/src \
                  -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -Wno-unused

../vendor/bearssl/obj/%.o: ../vendor/bearssl/src/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling vendor/bearssl: $<"
	@$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(LIBBEARSSL): $(BEARSSL_OBJS)
	@echo "Creating $@ (FU29.B; BearSSL portable build)"
	@$(AR) rcs $@ $^

# Phase 29 FU29.B.bearssl_wire: generated trust-anchor table (Mozilla CA
# bundle baked into rodata by scripts/gen_trust_anchors.py).  Regenerated
# when etc/tls/trust.pem changes.
TRUST_ANCHORS_SRC := libtls/trust_anchors.c
$(TRUST_ANCHORS_SRC): ../etc/tls/trust.pem ../scripts/gen_trust_anchors.py
	@echo "Generating $@ from etc/tls/trust.pem"
	@python3 ../scripts/gen_trust_anchors.py ../etc/tls/trust.pem --out $@

libtls/trust_anchors.o: $(TRUST_ANCHORS_SRC)
	@echo "Compiling user C: $< (BearSSL trust anchors)"
	@$(CC) $(CFLAGS) -I../vendor/bearssl/inc -c $< -o $@

# Phase 26 Stage H.1: wasmtest links additionally against the loader.o
# from user/wasmd/src/loader.c. Override of the generic TAP rule.
# Phase 27 Stage A5: tui_render.tap also needs libtui.
tests/tui_render: tests/tui_render.o $(LIBTUI) $(LIBTAP) $(LIBC)
	@echo "Linking TAP test: $@ (with libtui + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/tui_render.o $(LIBTUI) $(LIBTAP) $(LIBC)

# FU27.X.tui_demo_apps: tui_demo + tui_anim are user binaries (NOT TAP
# tests; manual demos run from gash). Link against libtui + libc.
tui_demo: tui_demo.o $(LIBTUI) $(LIBC)
	@echo "Linking user program: $@ (with libtui + libc)"
	@$(LD) $(LDFLAGS) -o $@ tui_demo.o $(LIBTUI) $(LIBC)

tui_anim: tui_anim.o $(LIBTUI) $(LIBC)
	@echo "Linking user program: $@ (with libtui + libc)"
	@$(LD) $(LDFLAGS) -o $@ tui_anim.o $(LIBTUI) $(LIBC)

# Phase 29 Session E: tui_demo_full demonstrates animation, atomic TX, mouse.
tui_demo_full: tui_demo_full.o $(LIBTUI) $(LIBC)
	@echo "Linking user program: $@ (with libtui + libc)"
	@$(LD) $(LDFLAGS) -o $@ tui_demo_full.o $(LIBTUI) $(LIBC)

# Phase 29 Session E gate tests — sprite_anim + cell_grid_atomic +
# mouse_basic + font_full_sweep.  No libtui link needed (they use syscall
# wrappers directly).

# Phase 27 Stage B1: console_sprite.tap links libtui (for TUI_SPRITE_BASE).
tests/console_sprite: tests/console_sprite.o $(LIBTUI) $(LIBTAP) $(LIBC)
	@echo "Linking TAP test: $@ (with libtui + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/console_sprite.o $(LIBTUI) $(LIBTAP) $(LIBC)

tests/wasmtest: tests/wasmtest.o wasmd/src/loader.o $(LIBTAP) $(LIBC)
	@echo "Linking TAP test (with wasmd loader): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/wasmtest.o wasmd/src/loader.o $(LIBTAP) $(LIBC)

# FU27.WASM Stage D1: wasm_e2e_run.tap exercises the full wasmd pipeline:
# connects to /sys/wasm/control via libnet, sends RUN_MODULE, waits for
# response, asserts hello.wasm output + AUDIT_PLEDGE_NARROW_EXEC.
tests/wasm_e2e_run: tests/wasm_e2e_run.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_e2e_run.o $(LIBNET) $(LIBTAP) $(LIBC)

# FU27.WASM Stage D2 (this stage): trap-path / load-reject / serial-reuse
# gates. Each test spawns its own wasmd, exercises one fault or stress
# axis, then kills wasmd to keep later tests undisturbed.
tests/wasm_fault_trap: tests/wasm_fault_trap.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_fault_trap.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_load_reject: tests/wasm_load_reject.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_load_reject.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_concurrent_serial: tests/wasm_concurrent_serial.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_concurrent_serial.o $(LIBNET) $(LIBTAP) $(LIBC)

# Phase 29 Session G (FU27.WASM.D2_worker): 5 fault-isolation + AI-bindings
# gates.  Each spawns its own wasmd, exercises one fault axis, then kills
# wasmd to keep subsequent tests undisturbed.
tests/wasm_fault_sigkill: tests/wasm_fault_sigkill.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_fault_sigkill.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_sandbox_path_narrow: tests/wasm_sandbox_path_narrow.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_sandbox_path_narrow.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_fault_fuel: tests/wasm_fault_fuel.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_fault_fuel.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_lacks_cap: tests/wasm_lacks_cap.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_lacks_cap.o $(LIBNET) $(LIBTAP) $(LIBC)

tests/wasm_ai_bindings: tests/wasm_ai_bindings.o $(LIBTAP) $(LIBNET) $(LIBC)
	@echo "Linking TAP test: $@ (with libnet + libtap + libc)"
	@$(LD) $(LDFLAGS) -o $@ tests/wasm_ai_bindings.o $(LIBNET) $(LIBTAP) $(LIBC)

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

# Phase 29 FU29.B.bearssl_wire: libtls.tap exercises real BearSSL crypto
# primitives (RFC SHA-256 vectors).  libtls_backend_available() now returns
# 1, so the test runs the live br_sha256_* path.  Compiled with -DWITH_BEARSSL
# + the BearSSL include path so the br_sha256_ctx forward decls resolve.
tests/libtls.o: tests/libtls.c
	@echo "Compiling user C: $< (BearSSL primitives test)"
	@$(CC) $(CFLAGS) -I../vendor/bearssl/inc -DWITH_BEARSSL -c $< -o $@

tests/libtls: tests/libtls.o $(LIBTAP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libtls — BearSSL): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/libtls.o $(TLS_LINK) $(LIBNET) $(LIBTAP) $(LIBC)

# Phase 29 FU29.B.bearssl_wire: libtls_handshake.tap exercises a full TLS
# handshake against scripts/run_tls_test_server.sh.  SKIPS in the automated
# gate (no TLS server reachable under SLIRP); real asserts when run
# interactively against a launched test server.
tests/libtls_handshake: tests/libtls_handshake.o $(LIBTAP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libtls — BearSSL): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/libtls_handshake.o $(TLS_LINK) $(LIBNET) $(LIBTAP) $(LIBC)

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

tests/dnstest: tests/dnstest.o $(LIBTAP) $(LIBHTTP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking TAP test (with libhttp + libtls/BearSSL + libnet): $@"
	@$(LD) $(LDFLAGS) -o $@ tests/dnstest.o $(LIBHTTP) $(TLS_LINK) $(LIBNET) $(LIBTAP) $(LIBC)

# Build grahai with its JSON parser. Phase 22 Stage E U21c: links
# libhttp + libnet. Phase 29 FU29.B: HTTPS branch now resolves to the real
# BearSSL handshake driver (was the dead libtls-mg weak -EPROTO fallback).
grahai: grahai.o json.o $(LIBHTTP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls/BearSSL + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ grahai.o json.o $(LIBHTTP) $(TLS_LINK) $(LIBNET) $(LIBC)

# Build gash shell (with JSON parser for AI command). Phase 22 closeout
# (G1.5): adds libtls so `gash> ai ...` actually exercises HTTPS to
# generativelanguage.googleapis.com instead of returning -EPROTO.
# --whole-archive forces the linker to pull libtls-mg.a in even though
# libhttp.c declares libtls_* as weak symbols (which the linker would
# otherwise resolve to NULL and skip the archive).
gash: gash.o json.o $(LIBHTTP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls/BearSSL + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ gash.o json.o $(LIBHTTP) $(TLS_LINK) $(LIBNET) $(LIBC)

# Phase 28 Session G.3 — gsh shell (full rewrite, TUI-first, parallel
# to gash).  Phase 29 Session F links libtui for the chrome rendering
# (frame + title + status footer + cap sidebar via mapped cell-VMO).
gsh: gsh.o $(LIBTUI) $(LIBC)
	@echo "Linking user program: $@ (with libtui + libc)"
	@$(LD) $(LDFLAGS) -o $@ gsh.o $(LIBTUI) $(LIBC)

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
dnstest: dnstest.o $(LIBHTTP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls/BearSSL + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ dnstest.o $(LIBHTTP) $(TLS_LINK) $(LIBNET) $(LIBC)

# Phase 22 Stage E U21b: aitest uses libhttp http_post against Gemini
# (HTTPS).  Phase 22 closeout (G1.5): libtls now linked via --whole-archive
# so the weak-symbol fallback in libhttp resolves to the shim's real impl.
aitest: aitest.o $(LIBHTTP) $(TLS_DEPS) $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libhttp + libtls/BearSSL + libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ aitest.o $(LIBHTTP) $(TLS_LINK) $(LIBNET) $(LIBC)

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

# Phase 26 Stage D.4: tiny binary spawned by pledge_narrow_exec.tap to
# verify post-narrow pledge_mask via SYS_DEBUG / DEBUG_READ_PLEDGE.
assertpledge: assertpledge.o $(LIBC)
	@echo "Linking user program: $@ (with libc)"
	@$(LD) $(LDFLAGS) -o $@ assertpledge.o $(LIBC)

# Phase 26 Stage E/G: bin/wasm operator CLI + minimal loader/validator.
# Real execution is Phase 27+ scope; substrate ships header + import-section
# parser cross-referenced against allowed-cap CSV.
wasm: wasm.o wasmd/src/loader.o $(LIBNET) $(LIBC)
	@echo "Linking user program: $@ (with libnet + libc)"
	@$(LD) $(LDFLAGS) -o $@ wasm.o wasmd/src/loader.o $(LIBNET) $(LIBC)

# FU27.WASM Stage D1: wasmd daemon — accepts RUN_MODULE on
# /sys/wasm/control, validates via Phase 26 loader, runs wasm3 IN-PROCESS,
# returns captured stdout. v1 chooses simplicity over fault isolation;
# Stage D2 fault tests will reintroduce wasmd_worker via narrow-exec.
wasmd/wasmd.o: wasmd/wasmd.c
	@echo "Compiling user C: $< (with wasm3 includes)"
	@$(CC) $(CFLAGS) -I../vendor/wasm3/source -Dd_m3HasFloat=0 -Dd_m3LogOutput=0 \
		-Wno-int-to-pointer-cast -Wno-sign-compare -c $< -o $@

wasmd/wasmd: wasmd/wasmd.o wasmd/src/loader.o $(LIBNET) $(LIBWASM3) $(LIBC)
	@echo "Linking user program: $@ (wasmd daemon — libnet + libwasm3 + libc)"
	@$(LD) $(LDFLAGS) -o $@ wasmd/wasmd.o wasmd/src/loader.o $(LIBNET) $(LIBWASM3) $(LIBC)

# FU27.WASM Stage D1: wasmd_worker — per-instance subprocess spawned by
# wasmd. Reads /tmp/wasmd_pending.wasm, instantiates via wasm3, captures
# stdout from `gcp.print` host import to /tmp/wasmd_output.txt, exits
# with a code reflecting which step succeeded (0=ok, 1=load, 2=link,
# 3=trap, 4+=internal). Links libwasm3 (vendored) + libc.
wasmd/wasmd_worker: wasmd/wasmd_worker.o $(LIBWASM3) $(LIBC)
	@echo "Linking user program: $@ (wasmd_worker — libwasm3 + libc)"
	@$(LD) $(LDFLAGS) -o $@ wasmd/wasmd_worker.o $(LIBWASM3) $(LIBC)

# Phase 27 Block A (Stage A4): /bin/fbd — userspace framebuffer compositor.
# Spawned by init under autorun=init via etc/init.conf. NOT spawned under
# autorun=ktest; the gate test fbd_render.tap exercises the substrate via
# DEBUG syscalls instead. Links libtui for the startup banner (Stage A4
# closeout fix — without a banner the framebuffer goes black after fbd's
# first ACK_RENDER because the cell VMO is zero-filled).
fbd/fbd: fbd/fbd.o $(LIBTUI) $(LIBC)
	@echo "Linking user program: $@ (with libtui + libc)"
	@$(LD) $(LDFLAGS) -o $@ fbd/fbd.o $(LIBTUI) $(LIBC)

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

# Phase 24 W19.6: /bin/snapshot — COW snapshot CLI. Dispatches by argv[0]
# basename so cp'd aliases (snapshots / restore / snap-delete) reuse the
# same binary.
snapshot: snapshot.o $(LIBC)
	@echo "Linking snapshot"
	@$(LD) $(LDFLAGS) -o $@ snapshot.o $(LIBC)

# Phase 25 Stage H: /bin/txnctl — informational CLI for transaction
# substrate. Dispatches by argv[0] basename so cp'd aliases (txn-status)
# reuse the same binary. Transactions are process-scoped, so the CLI
# can only inspect / explain — see comment in user/txnctl.c.
txnctl: txnctl.o $(LIBC)
	@echo "Linking txnctl"
	@$(LD) $(LDFLAGS) -o $@ txnctl.o $(LIBC)

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

# FU27.WASM Stage D1: wasmd_worker.c includes <wasm3.h> from vendored tree.
# Override the generic %.o rule for this file to add -Ivendor/wasm3/source
# and the same -Dd_m3HasFloat=0 used to build libwasm3.a.
wasmd/wasmd_worker.o: wasmd/wasmd_worker.c
	@echo "Compiling user C: $< (with wasm3 includes)"
	@$(CC) $(CFLAGS) -I../vendor/wasm3/source -Dd_m3HasFloat=0 -Dd_m3LogOutput=0 \
		-Wno-int-to-pointer-cast -Wno-sign-compare -c $< -o $@

%.o: %.c
	@echo "Compiling user C: $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning user programs..."
	@rm -f *.o $(PROGRAMS) $(LIBTAP) tests/*.o $(TAP_TESTS)
	@# Phase 29 FU29.B: BearSSL objects + archive + generated trust anchors.
	@rm -rf ../vendor/bearssl/obj
	@rm -f $(LIBBEARSSL) libtls/libtls.o libtls/trust_anchors.o $(LIBTLS_NEW)
	@# Keep libtls/trust_anchors.c (regenerated on demand from trust.pem); it
	@# is cheap to rebuild and removing it forces a Python dependency at the
	@# next build.  Uncomment to scrub it as well:
	@# rm -f libtls/trust_anchors.c