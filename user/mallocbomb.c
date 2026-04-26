// user/mallocbomb.c
//
// Phase 20 — tiny test program for AW-20.2 (memory limit enforcement).
//
// Repeatedly calls brk() to grow the heap by one page at a time. When the
// kernel's rlimit_check_mem refuses (-ENOMEM), print the count of successful
// pages and exit 0. The parent (test harness or gash) compares the count
// against the configured RLIMIT_MEM to verify enforcement fires at the
// exact page.

#include "syscalls.h"
#include <stdint.h>

static void print(const char *s) { while (*s) syscall_putc(*s++); }
static void print_u64(uint64_t v) {
    char b[32]; int n = 0;
    if (v == 0) { print("0"); return; }
    while (v > 0 && n < 31) { b[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) { char s[2] = { b[i], 0 }; print(s); }
}

void _start(void) {
    // Query current brk.
    long base = syscall_brk(NULL);
    if (base < 0) {
        print("mallocbomb: brk query failed\n");
        syscall_exit(1);
    }
    uint64_t cur = (uint64_t)base;
    uint64_t n = 0;
    while (1) {
        uint64_t next = cur + 4096;
        long r = syscall_brk((void *)next);
        if (r < 0 || (uint64_t)r != next) {
            // Either -ENOMEM from rlimit_check_mem (r == -12) or pmm_alloc
            // refused. Either way, limit reached.
            break;
        }
        cur = next;
        n++;
    }
    print("mallocbomb: allocated ");
    print_u64(n);
    print(" pages before limit\n");
    syscall_exit(0);
}
