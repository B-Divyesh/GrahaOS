// kernel/pid_hash.c
//
// Phase 20 — implementation of the PID hash table + global task list.
// See pid_hash.h for the contract.
#include "pid_hash.h"

#include "../arch/x86_64/cpu/sched/sched.h"

// Open-chained hash. Buckets hold the head of a singly-linked list threaded
// via task_struct.hash_next.
static task_t *g_hash_buckets[PID_HASH_BUCKETS];

// Doubly-linked global enumeration list.
static task_t *g_global_head = NULL;
static task_t *g_global_tail = NULL;

// Count of live tasks (size of the global list).
static uint32_t g_global_count = 0;

// Serialises every mutation. Readers (lookup / enumerate) also take it.
static spinlock_t g_pid_hash_lock = SPINLOCK_INITIALIZER("pid_hash");

// ---------------------------------------------------------------------------
// Init. Called once from kmain after sched_init. Idle task (PID 0) is
// inserted immediately afterward by sched_init itself (see U6).
// ---------------------------------------------------------------------------
void pid_hash_init(void) {
    for (uint32_t i = 0; i < PID_HASH_BUCKETS; i++) {
        g_hash_buckets[i] = NULL;
    }
    g_global_head = NULL;
    g_global_tail = NULL;
    g_global_count = 0;
}

// ---------------------------------------------------------------------------
// Hash function. Task IDs are assigned sequentially from next_task_id;
// `id & (N-1)` gives us N consecutive buckets before any collision. Good
// enough for a start-of-day workload; adversarial PID churn can be
// mitigated with an xorshift mix later if needed.
// ---------------------------------------------------------------------------
static inline uint32_t bucket_for(int id) {
    return ((uint32_t)id) & (PID_HASH_BUCKETS - 1u);
}

void pid_hash_insert(task_t *t) {
    if (!t) return;
    uint32_t b = bucket_for(t->id);

    spinlock_acquire(&g_pid_hash_lock);

    // Insert at the head of the hash bucket (O(1)).
    t->hash_next = g_hash_buckets[b];
    g_hash_buckets[b] = t;

    // Append at the tail of the global list (so enumeration order matches
    // creation order — helpful for deterministic psinfo output).
    t->global_next = NULL;
    t->global_prev = g_global_tail;
    if (g_global_tail) {
        g_global_tail->global_next = t;
    } else {
        g_global_head = t;
    }
    g_global_tail = t;
    g_global_count++;

    spinlock_release(&g_pid_hash_lock);
}

void pid_hash_remove(task_t *t) {
    if (!t) return;
    uint32_t b = bucket_for(t->id);

    spinlock_acquire(&g_pid_hash_lock);

    // Unlink from hash bucket. Walk the chain (load factor is small; worst
    // case ~8 entries at design capacity).
    task_t **pprev = &g_hash_buckets[b];
    while (*pprev && *pprev != t) {
        pprev = &(*pprev)->hash_next;
    }
    if (*pprev == t) {
        *pprev = t->hash_next;
    }
    t->hash_next = NULL;

    // Unlink from global list.
    if (t->global_prev) {
        t->global_prev->global_next = t->global_next;
    } else if (g_global_head == t) {
        g_global_head = t->global_next;
    }
    if (t->global_next) {
        t->global_next->global_prev = t->global_prev;
    } else if (g_global_tail == t) {
        g_global_tail = t->global_prev;
    }
    t->global_next = NULL;
    t->global_prev = NULL;
    if (g_global_count > 0) g_global_count--;

    spinlock_release(&g_pid_hash_lock);
}

task_t *pid_hash_lookup(int id) {
    uint32_t b = bucket_for(id);

    spinlock_acquire(&g_pid_hash_lock);
    task_t *t = g_hash_buckets[b];
    while (t && t->id != id) {
        t = t->hash_next;
    }
    spinlock_release(&g_pid_hash_lock);
    return t;
}

void pid_hash_enumerate(pid_hash_enum_fn fn, void *ctx) {
    if (!fn) return;
    spinlock_acquire(&g_pid_hash_lock);
    task_t *t = g_global_head;
    while (t) {
        task_t *next = t->global_next;
        fn(t, ctx);
        t = next;
    }
    spinlock_release(&g_pid_hash_lock);
}

uint32_t pid_hash_count(void) {
    spinlock_acquire(&g_pid_hash_lock);
    uint32_t n = g_global_count;
    spinlock_release(&g_pid_hash_lock);
    return n;
}
