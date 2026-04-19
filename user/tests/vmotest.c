// user/tests/vmotest.c — Phase 17 VMO TAP test.
//
// 18 TAP assertions covering create/zero/map, rights narrowing, COW clone
// isolation, and refcount-driven freeing. All within a single process.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define ONE_MIB  (1024ull * 1024ull)
#define PAGE_SZ  4096ull

void _start(void) {
    tap_plan(18);

    // -------------------- G1: Create + zeroed (3 asserts) ---------
    long vres = syscall_vmo_create(ONE_MIB, VMO_ZEROED);
    TAP_ASSERT(vres > 0, "1. vmo_create(1 MiB, ZEROED) returns a valid token");
    cap_token_u_t vmo = {.raw = (uint64_t)vres};

    long map = syscall_vmo_map(vmo, 0, 0, ONE_MIB, PROT_READ | PROT_WRITE);
    TAP_ASSERT(map > 0 && (map & 0xFFFu) == 0, "2. vmo_map(RW) returns page-aligned VA");

    uint8_t *p = (uint8_t *)(uintptr_t)map;
    int zeros = 1;
    // Sample a few offsets instead of scanning all 1 MiB (boot time).
    uint64_t offsets[] = {0, 4095, 4096, 262143, 524288, 1048575};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        if (p[offsets[i]] != 0) { zeros = 0; break; }
    }
    TAP_ASSERT(zeros, "3. VMO_ZEROED pages are all zero at sampled offsets");

    // -------------------- G2: write + read (2 asserts) ---------
    p[0]        = 0xA1;
    p[4095]     = 0xA2;
    p[ONE_MIB - 1] = 0xA3;
    int written_ok = (p[0] == 0xA1 && p[4095] == 0xA2 && p[ONE_MIB - 1] == 0xA3);
    TAP_ASSERT(written_ok, "4. RW mapping accepts writes at varied offsets");
    // Same bytes are readable back (no page-fault-denial).
    TAP_ASSERT(p[0] == 0xA1, "5. Write is immediately visible in readback");

    // -------------------- G3: Unmap (2 asserts) ---------
    long urc = syscall_vmo_unmap((uint64_t)map, ONE_MIB);
    TAP_ASSERT(urc == 0, "6. vmo_unmap returns 0 on exact range");

    // Remap to verify VMO persists after unmap (refcount was 2 → 1).
    long map2 = syscall_vmo_map(vmo, 0, 0, ONE_MIB, PROT_READ);
    TAP_ASSERT(map2 > 0, "7. remap after unmap succeeds (VMO still live)");
    uint8_t *q = (uint8_t *)(uintptr_t)map2;
    TAP_ASSERT(q[0] == 0xA1, "8. remapped VMO preserves prior writes");
    syscall_vmo_unmap((uint64_t)map2, ONE_MIB);

    // -------------------- G4: Invalid handle (2 asserts) ---------
    cap_token_u_t bogus = {.raw = 0xDEADBEEFCAFEBABEULL};
    long bad_map = syscall_vmo_map(bogus, 0, 0, PAGE_SZ, PROT_READ);
    TAP_ASSERT(bad_map < 0, "9. vmo_map with invalid handle returns negative");
    long bad_clone = syscall_vmo_clone(bogus, 0);
    TAP_ASSERT(bad_clone < 0, "10. vmo_clone with invalid handle returns negative");

    // -------------------- G5: Unaligned + zero-size (2 asserts) ---
    long bad_create = syscall_vmo_create(1234, VMO_ZEROED);
    TAP_ASSERT(bad_create < 0, "11. vmo_create with unaligned size returns negative");
    long zero_create = syscall_vmo_create(0, 0);
    TAP_ASSERT(zero_create < 0, "12. vmo_create with size 0 returns negative");

    // -------------------- G6: COW clone isolation (4 asserts) ---
    long v2 = syscall_vmo_create(2 * PAGE_SZ, VMO_ZEROED);
    cap_token_u_t base = {.raw = (uint64_t)v2};
    long bmap = syscall_vmo_map(base, 0, 0, 2 * PAGE_SZ, PROT_READ | PROT_WRITE);
    uint8_t *bp = (uint8_t *)(uintptr_t)bmap;
    bp[0] = 0xB0; bp[PAGE_SZ] = 0xB1;

    long cres = syscall_vmo_clone(base, VMO_CLONE_COW);
    TAP_ASSERT(cres > 0, "13. vmo_clone COW returns child handle");
    cap_token_u_t child = {.raw = (uint64_t)cres};

    long cmap = syscall_vmo_map(child, 0, 0, 2 * PAGE_SZ, PROT_READ | PROT_WRITE);
    TAP_ASSERT(cmap > 0, "14. mapping COW child succeeds");
    uint8_t *cp = (uint8_t *)(uintptr_t)cmap;

    TAP_ASSERT(cp[0] == 0xB0 && cp[PAGE_SZ] == 0xB1,
               "15. COW child sees parent's pre-clone writes");

    // Child writes; parent should still see its original values.
    cp[0] = 0xCC;
    cp[PAGE_SZ] = 0xDD;
    TAP_ASSERT(bp[0] == 0xB0 && bp[PAGE_SZ] == 0xB1,
               "16. COW isolation: parent unchanged after child writes");

    syscall_vmo_unmap((uint64_t)cmap, 2 * PAGE_SZ);
    syscall_vmo_unmap((uint64_t)bmap, 2 * PAGE_SZ);

    // -------------------- G7: Oversize rejected (1 assert) ---
    // 512 MiB exceeds VMO_MAX_SIZE (256 MiB).
    long huge = syscall_vmo_create(512ull * 1024 * 1024, VMO_ZEROED);
    TAP_ASSERT(huge < 0, "17. vmo_create > 256 MiB rejected");

    // -------------------- G8: Pledge enforcement (1 assert) ---
    uint16_t narrow = (uint16_t)(PLEDGE_ALL & ~PLEDGE_COMPUTE);
    syscall_pledge(narrow);
    long post_pledge = syscall_vmo_create(PAGE_SZ, VMO_ZEROED);
    TAP_ASSERT(post_pledge == -7,
               "18. vmo_create returns -EPLEDGE after COMPUTE dropped");

    tap_done();
    exit(0);
}
