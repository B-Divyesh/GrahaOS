// kernel/pid_hash.h
//
// Phase 20 — PID → task_t* lookup via open-chained hash + doubly-linked
// global enumeration list.
//
// The pre-Phase-20 scheduler used a fixed `task_ptrs[MAX_TASKS=64]` array
// indexed linearly. That capped the system at 64 concurrent processes and
// scanned O(N) on every lookup. Phase 20 replaces it with:
//
//   - `task_hashtable[PID_HASH_BUCKETS]` — 2048 buckets, open-chained via
//     task_t.hash_next. Expected load factor at 10 240 tasks is 5 (a fast
//     chain walk per lookup; ~99% of chains are ≤ 8 entries long).
//
//   - `task_global_list` — doubly-linked via task_t.global_next/prev. Used
//     for O(N) enumeration by psinfo, the 1 Hz epoch tick, and any external
//     observer that needs to see every live task.
//
// Both structures are protected by `g_pid_hash_lock`. Lookups take the lock
// briefly; enumeration callers either snapshot the list under the lock or
// tolerate concurrent mutation. Insert/remove is called only from the task
// allocator (sched_create_task / sched_create_user_process / spawn) and the
// reaper (sched_reap_zombie) — never on the hot schedule() path.
//
// Non-goals:
//  - Lock-free reads. A spin of ~50 ns every sys_kill / sys_wait / ps is
//    fine for MVP.
//  - Shrinkable table. 2048 × 8 bytes = 16 KiB — small enough to live in BSS.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "sync/spinlock.h"

struct task_struct;

// PID_HASH_BUCKETS must be a power of 2 for the `pid & (N-1)` shortcut.
#define PID_HASH_BUCKETS 2048u

// ---------------------------------------------------------------------------
// Lifecycle. Called from main.c during kmain init, after sched_init() but
// before the first sched_create_task.
// ---------------------------------------------------------------------------
void pid_hash_init(void);

// ---------------------------------------------------------------------------
// Insert a task into the hash table and append to the global enumeration
// list. The task's id MUST be set before calling. Asserts (in debug builds)
// that the task is not already present.
// ---------------------------------------------------------------------------
void pid_hash_insert(struct task_struct *t);

// ---------------------------------------------------------------------------
// Remove a task from both the hash bucket and the global list. Caller
// MUST hold no scheduler locks (acquires g_pid_hash_lock internally).
// ---------------------------------------------------------------------------
void pid_hash_remove(struct task_struct *t);

// ---------------------------------------------------------------------------
// Look up a task by id. Returns NULL if not present. The returned pointer
// is only guaranteed to be valid while the caller can prove the task hasn't
// been freed — most callers rely on scheduler-level invariants (e.g.,
// sched_reap_zombie only runs on tasks in state ZOMBIE, so a non-ZOMBIE
// result is safe for the caller to use synchronously).
// ---------------------------------------------------------------------------
struct task_struct *pid_hash_lookup(int id);

// ---------------------------------------------------------------------------
// Enumeration. Invokes `fn(task, ctx)` for each task in the global list.
// Walks under g_pid_hash_lock; `fn` MUST NOT acquire scheduler locks or do
// disk I/O. Appropriate for short in-memory computations (psinfo snapshot,
// epoch tick budget refill).
// ---------------------------------------------------------------------------
typedef void (*pid_hash_enum_fn)(struct task_struct *t, void *ctx);
void pid_hash_enumerate(pid_hash_enum_fn fn, void *ctx);

// ---------------------------------------------------------------------------
// Count of live tasks (size of the global list). Informational / psinfo.
// ---------------------------------------------------------------------------
uint32_t pid_hash_count(void);
