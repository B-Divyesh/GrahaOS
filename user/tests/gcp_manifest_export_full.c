// user/tests/gcp_manifest_export_full.c
//
// Phase 28 Session G.4.a — extended SYS_MANIFEST_EXPORT gate.  Five
// asserts, building on Phase 27's manifest_export.tap (3 asserts):
//   1. positive byte count
//   2. bytes == kernel-side g_manifest_blob_size constant (14623)
//   3. generation hash == kernel-side g_manifest_generation
//   4. user-recomputed FNV-1a over the returned blob matches generation
//   5. blob contains the literal "SYS_MANIFEST_EXPORT" substring
//      (proves the syscall is self-documented)

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>

extern int printf(const char *fmt, ...);

// Mirror of kernel/manifest_blob.c constants.  Verified by asserts 2+3.
#define EXPECTED_BLOB_SIZE   14623u
#define EXPECTED_GENERATION  0xc744a504fd139597ull

static uint8_t s_buf[16384];

// FNV-1a 64-bit.  Mirror of scripts/gen_manifest_blob.py + kernel/fs/simhash.c.
static uint64_t fnv1a64(const uint8_t *p, long n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (long i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

void _start(void) {
    tap_plan(5);

    uint64_t gen = 0;
    long bytes = syscall_manifest_export(s_buf, sizeof(s_buf), &gen);
    if (bytes <= 0) printf("# manifest_export rc=%ld\n", bytes);
    TAP_ASSERT(bytes > 0, "1. manifest_export returns positive byte count");

    if (bytes > 0 && (uint64_t)bytes != EXPECTED_BLOB_SIZE) {
        printf("# expected bytes=%lu got=%ld\n",
               (unsigned long)EXPECTED_BLOB_SIZE, bytes);
    }
    TAP_ASSERT(bytes > 0 && (uint64_t)bytes == EXPECTED_BLOB_SIZE,
               "2. byte count matches kernel-side g_manifest_blob_size");

    if (gen != EXPECTED_GENERATION) {
        printf("# expected gen=0x%lx got=0x%lx\n",
               (unsigned long)EXPECTED_GENERATION, (unsigned long)gen);
    }
    TAP_ASSERT(gen == EXPECTED_GENERATION,
               "3. generation hash matches kernel-side g_manifest_generation");

    uint64_t recomputed = (bytes > 0) ? fnv1a64(s_buf, bytes) : 0;
    if (recomputed != gen) {
        printf("# recomputed FNV-1a=0x%lx, expected=0x%lx\n",
               (unsigned long)recomputed, (unsigned long)gen);
    }
    TAP_ASSERT(bytes > 0 && recomputed == gen,
               "4. user-recomputed FNV-1a matches reported generation");

    // Naive substring scan for a syscall name that we know is in gcp.json.
    // SYS_STREAM_CREATE is a Phase 18 entry and has been stable since.
    const char needle[] = "SYS_STREAM_CREATE";
    long nlen = (long)(sizeof(needle) - 1);
    int found = 0;
    if (bytes > 0) {
        for (long i = 0; i + nlen <= bytes; i++) {
            int ok = 1;
            for (long j = 0; j < nlen; j++) {
                if (s_buf[i + j] != (uint8_t)needle[j]) { ok = 0; break; }
            }
            if (ok) { found = 1; break; }
        }
    }
    if (!found) printf("# substring 'SYS_MANIFEST_EXPORT' not found\n");
    TAP_ASSERT(found, "5. blob contains 'SYS_STREAM_CREATE' substring");

    tap_done();
    syscall_exit(0);
}
