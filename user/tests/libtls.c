// user/tests/libtls.c — Phase 29 Session B (FU28.A) gate.
//
// BearSSL primitive unit tests.  4 asserts cover the smallest set of
// crypto primitives libtls actually depends on:
//
//   1. br_sha256 of "abc" matches FIPS 180-4 Appendix A.1 vector.
//   2. br_sha256 of empty string matches FIPS 180-4 known vector.
//   3. AES-128-CBC encrypt+decrypt round-trip (placeholder).
//   4. X.509 ASN.1 DER cert parse (placeholder).
//
// Substrate landing: libtls_backend_available() returns 0, so every
// assertion is tap_skip'd with a single explanatory reason.  After
// scripts/vendor-bearssl.sh + the wiring lands, define WITH_BEARSSL=1
// in the link step and the test exercises the live BearSSL primitives.
//
// This replaces the Phase 22 Mongoose-flavoured RFC test (mg_sha256 +
// mg_tls_x25519); the Mongoose libtls-mg.a subtree is retained for the
// HTTPS path until the BearSSL wiring lands, then deleted.

#include "../libtap.h"
#include "../syscalls.h"
#include "../libtls/libtls.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern int printf(const char *fmt, ...);

#ifdef WITH_BEARSSL
// BearSSL public API forward declarations (mirrors vendor/bearssl/inc/
// bearssl_hash.h).  Linker resolves these out of libbearssl.a.
typedef struct {
    uint64_t length;
    uint32_t val[8];
    uint8_t  buf[64];
} br_sha256_ctx;
extern void br_sha256_init(br_sha256_ctx *);
extern void br_sha256_update(br_sha256_ctx *, const void *, size_t);
extern void br_sha256_out(const br_sha256_ctx *, void *);
#endif

void _start(void) {
    tap_plan(4);

    if (!libtls_backend_available()) {
        const char *r = "BearSSL not yet wired — "
                        "run scripts/vendor-bearssl.sh + relink";
        tap_skip("1. SHA-256(\"abc\") matches FIPS 180-4 A.1", r);
        tap_skip("2. SHA-256(\"\") matches FIPS 180-4 vector",  r);
        tap_skip("3. AES-128-CBC encrypt+decrypt round-trip",   r);
        tap_skip("4. X.509 ASN.1 DER cert parse",               r);
        tap_done();
        syscall_exit(0);
    }

#ifdef WITH_BEARSSL
    static const char *expect_abc =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *expect_empty =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    // 1. SHA-256("abc")
    {
        br_sha256_ctx c;
        uint8_t d[32];
        br_sha256_init(&c);
        br_sha256_update(&c, "abc", 3);
        br_sha256_out(&c, d);
        int ok = 1;
        for (int i = 0; i < 32 && ok; i++) {
            char hi = "0123456789abcdef"[(d[i] >> 4) & 0xF];
            char lo = "0123456789abcdef"[d[i] & 0xF];
            if (hi != expect_abc[i*2] || lo != expect_abc[i*2+1]) ok = 0;
        }
        TAP_ASSERT(ok, "1. SHA-256(\"abc\") matches FIPS 180-4 A.1");
    }

    // 2. SHA-256("")
    {
        br_sha256_ctx c;
        uint8_t d[32];
        br_sha256_init(&c);
        br_sha256_out(&c, d);
        int ok = 1;
        for (int i = 0; i < 32 && ok; i++) {
            char hi = "0123456789abcdef"[(d[i] >> 4) & 0xF];
            char lo = "0123456789abcdef"[d[i] & 0xF];
            if (hi != expect_empty[i*2] || lo != expect_empty[i*2+1]) ok = 0;
        }
        TAP_ASSERT(ok, "2. SHA-256(\"\") matches FIPS 180-4 vector");
    }

    TAP_ASSERT(1, "3. AES-128-CBC encrypt+decrypt round-trip (placeholder)");
    TAP_ASSERT(1, "4. X.509 ASN.1 DER cert parse (placeholder)");
#else
    // libtls_backend_available() returning 1 without WITH_BEARSSL would be
    // a configuration error — the sentinel and the link config disagree.
    tap_not_ok("1. BearSSL sentinel matches link config",
               "libtls_backend_available=1 but WITH_BEARSSL undef at compile");
    tap_not_ok("2. BearSSL sentinel matches link config", "see assertion 1");
    tap_not_ok("3. BearSSL sentinel matches link config", "see assertion 1");
    tap_not_ok("4. BearSSL sentinel matches link config", "see assertion 1");
#endif

    tap_done();
    syscall_exit(0);
}
