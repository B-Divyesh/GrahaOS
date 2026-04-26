// user/include/ktest_helpers.h — Phase 23 P23.deferred.1 cutover prep.
//
// Static-inline helpers for tests that need to bring up userspace daemons
// in autorun=ktest mode (where init.conf is not run, so daemons must be
// spawned explicitly). Intended use:
//
//   #include "../../include/ktest_helpers.h"
//   int ahcid_pid = ktest_spawn_ahcid();
//   if (ahcid_pid > 0) { /* run FS-touching test */ ktest_kill_ahcid(ahcid_pid); }
//
// Header-only so tests don't need a separate library link.
#pragma once

#include <stdint.h>
#include "../syscalls.h"

// Approximate sleep that doesn't cost a syscall. Tunable per host;
// ~5 ms per 10000 iters on QEMU TCG.
static inline void ktest_helpers_spin_ms(int ms) {
    for (int i = 0; i < ms; i++) {
        for (volatile int j = 0; j < 10000; j++) { (void)j; }
    }
}

// Connect-probe: returns 1 if /sys/blk/service is connectable today.
// Side-effect: drops the test connection if it succeeds, since the caller
// will reconnect with a fresh handle pair.
static inline int ktest_blk_service_up(void) {
    cap_token_u_t wr = {.raw = 0}, rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/blk/service", 16, &wr, &rd);
    if (rc != 0) return 0;
    // Drop our probe handles; close-on-revoke + scope cleanup will reap.
    return 1;
}

// Spawn /bin/ahcid and wait up to ~3 s for /sys/blk/service to publish.
// Returns the ahcid PID on success, 0 if spawn failed, -1 if service
// never came up (caller should kill+wait the ahcid pid in that case via
// ktest_kill_ahcid; non-zero rc handled by caller).
//
// Pre-condition: caller has deactivated the kernel-resident "disk" CAN
// cap so ahcid can drive the controller. The helper does NOT do this
// itself — the caller's test logic decides whether to deactivate
// (some tests want both running concurrently for handle-truncation
// negative tests, etc).
static inline int ktest_spawn_ahcid(void) {
    int pid = syscall_spawn("bin/ahcid");
    if (pid <= 0) return 0;
    for (int t = 0; t < 600; t++) {
        if (ktest_blk_service_up()) return pid;
        ktest_helpers_spin_ms(5);
    }
    return -1;  // Spawned but didn't publish.
}

// Kill ahcid by pid and reap. Best-effort; logs nothing.
static inline void ktest_kill_ahcid(int pid) {
    if (pid <= 0) return;
    (void)syscall_kill(pid, 9);
    int s = 0;
    (void)syscall_wait(&s);
}
