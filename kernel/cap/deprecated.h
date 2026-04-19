// kernel/cap/deprecated.h
//
// Phase 16 — tracker for deprecated-syscall first-hit auditing.
//
// The seven legacy CAN syscalls (SYS_CAP_ACTIVATE .. SYS_CAP_POLL, numbers
// 1031-1034 and 1038-1040) all return -EDEPRECATED in Phase 16. Each call is
// audited, but we suppress spam: the very first time a given (pid, syscall)
// pair hits any deprecated handler, an AUDIT_DEPRECATED_SYSCALL entry is
// emitted; every subsequent call from the same pid for the same syscall is
// silent. A pid that has hit seven different deprecated numbers produces
// exactly seven audit entries.
//
// Storage: 64 static slots × 8 bytes = 512 B total. Evicted LRU when full.
// Slots are freed when the pid exits (sched_reap_zombie calls
// deprecated_forget_pid).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Number of slots. 64 covers the reasonable working set of concurrent
// processes that might ever call a deprecated syscall; LRU eviction handles
// the overflow case. Bumpable without API change.
#define DEPRECATED_TRACKER_SLOTS 64

// Number of deprecated syscall numbers we track. Bit 0 = 1031, bit 1 = 1032,
// bit 2 = 1033, bit 3 = 1034, bit 4 = 1038, bit 5 = 1039, bit 6 = 1040.
// 7 bits used of 16. The mapping is fixed; see deprecated.c.
#define DEPRECATED_SYSCALL_BITS 7

typedef struct {
    int32_t  pid;              // -1 = empty slot.
    uint16_t warned_bitmap;    // Bit set = this (pid, syscall) was already audited.
    uint16_t _pad;             // Keep slot 8 bytes.
} deprecated_syscall_tracker_t;

// Check whether this is the first time `pid` has hit deprecated syscall
// `syscall_num`. Returns true if the caller should emit an audit entry now
// (and marks the bit so subsequent calls return false); false if already
// audited. Unknown syscall numbers always return true (treat as new).
// Thread-safe; internally serialised by a spinlock.
bool deprecated_check_and_audit(int32_t pid, uint32_t syscall_num);

// Release any tracker slot held by `pid`. Called from sched_reap_zombie so a
// reaped pid doesn't hold a slot forever. A future pid with the same number
// (unlikely in Phase 16; pids are monotonic) would get a fresh slot.
void deprecated_forget_pid(int32_t pid);
