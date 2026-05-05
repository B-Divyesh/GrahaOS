/*
 * libc/src/libgcc_shims.c
 *
 * Software implementations of libgcc helpers that GCC may emit calls to
 * when the target CPU baseline doesn't include the matching instruction.
 *
 * Authored for FU27.WASM Stage D0: vendored wasm3 uses __builtin_popcount
 * + __builtin_popcountll. With our `-march=x86-64` baseline (no POPCNT),
 * GCC lowers those builtins to calls to __popcountsi2 / __popcountdi2 from
 * libgcc.a — which we don't link. The shims below resolve the calls in
 * software via Hacker's Delight 5-1 (parallel-summation popcount).
 *
 * Adding -mpopcnt to user CFLAGS would also work but commits the userspace
 * to v2-class hardware. The shim path costs ~10 cycles/call vs 1 for the
 * inline instruction; wasm3 calls popcount only via Wasm's i32.popcnt /
 * i64.popcnt opcodes (rare path), so the difference is unmeasurable.
 */

/* Hacker's Delight 5-1 (parallel-summation), 64-bit. */
int __popcountdi2(unsigned long long x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

/* 32-bit variant — GCC may lower __builtin_popcount(x) to this. */
int __popcountsi2(unsigned int x) {
    x = x - ((x >> 1) & 0x55555555U);
    x = (x & 0x33333333U) + ((x >> 2) & 0x33333333U);
    x = (x + (x >> 4)) & 0x0F0F0F0F0U;
    return (int)((x * 0x01010101U) >> 24);
}
