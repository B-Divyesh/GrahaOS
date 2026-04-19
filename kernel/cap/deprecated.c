// kernel/cap/deprecated.c
// Phase 16 — deprecated-syscall first-hit tracker. See deprecated.h.
#include "deprecated.h"
#include "../sync/spinlock.h"

// The 7 deprecated syscall numbers, in bit order. Index i of this array maps
// to bit i of warned_bitmap. Any other syscall number is unmapped and will
// always trigger an audit (conservative).
static const uint32_t g_deprecated_numbers[DEPRECATED_SYSCALL_BITS] = {
    1031,  // SYS_CAP_ACTIVATE
    1032,  // SYS_CAP_DEACTIVATE
    1033,  // SYS_CAP_REGISTER
    1034,  // SYS_CAP_UNREGISTER
    1038,  // SYS_CAP_WATCH
    1039,  // SYS_CAP_UNWATCH
    1040,  // SYS_CAP_POLL
};

static deprecated_syscall_tracker_t g_tracker[DEPRECATED_TRACKER_SLOTS];
static uint32_t g_next_victim;        // round-robin eviction cursor
static spinlock_t g_tracker_lock;
static bool g_tracker_initialised;

static void ensure_init(void) {
    if (g_tracker_initialised) return;
    for (uint32_t i = 0; i < DEPRECATED_TRACKER_SLOTS; i++) {
        g_tracker[i].pid = -1;
        g_tracker[i].warned_bitmap = 0;
        g_tracker[i]._pad = 0;
    }
    g_next_victim = 0;
    spinlock_init(&g_tracker_lock, "deprecated_tracker");
    g_tracker_initialised = true;
}

// Map a syscall number to a bit position. Returns -1 if not one we track.
static int bit_for_syscall(uint32_t syscall_num) {
    for (int i = 0; i < DEPRECATED_SYSCALL_BITS; i++) {
        if (g_deprecated_numbers[i] == syscall_num) return i;
    }
    return -1;
}

bool deprecated_check_and_audit(int32_t pid, uint32_t syscall_num) {
    ensure_init();

    int bit = bit_for_syscall(syscall_num);
    // Unknown syscalls always audit (shouldn't happen in practice; keeps us
    // loud rather than silent if a new deprecated number appears without the
    // table being updated).
    if (bit < 0) return true;
    const uint16_t mask = (uint16_t)(1u << bit);

    spinlock_acquire(&g_tracker_lock);

    // Look for existing slot.
    for (uint32_t i = 0; i < DEPRECATED_TRACKER_SLOTS; i++) {
        if (g_tracker[i].pid == pid) {
            bool first_hit = (g_tracker[i].warned_bitmap & mask) == 0;
            g_tracker[i].warned_bitmap |= mask;
            spinlock_release(&g_tracker_lock);
            return first_hit;
        }
    }

    // Allocate: first empty, else evict the round-robin victim.
    uint32_t slot = DEPRECATED_TRACKER_SLOTS;
    for (uint32_t i = 0; i < DEPRECATED_TRACKER_SLOTS; i++) {
        if (g_tracker[i].pid == -1) { slot = i; break; }
    }
    if (slot == DEPRECATED_TRACKER_SLOTS) {
        slot = g_next_victim;
        g_next_victim = (g_next_victim + 1) % DEPRECATED_TRACKER_SLOTS;
    }
    g_tracker[slot].pid = pid;
    g_tracker[slot].warned_bitmap = mask;
    g_tracker[slot]._pad = 0;

    spinlock_release(&g_tracker_lock);
    return true;
}

void deprecated_forget_pid(int32_t pid) {
    if (!g_tracker_initialised) return;
    spinlock_acquire(&g_tracker_lock);
    for (uint32_t i = 0; i < DEPRECATED_TRACKER_SLOTS; i++) {
        if (g_tracker[i].pid == pid) {
            g_tracker[i].pid = -1;
            g_tracker[i].warned_bitmap = 0;
        }
    }
    spinlock_release(&g_tracker_lock);
}
