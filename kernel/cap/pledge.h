// kernel/cap/pledge.h
//
// Phase 15b — Per-task pledge mask (the second authority axis).
//
// A capability token (Phase 15a) answers "what CAN this process do right now?"
// A pledge mask answers "what WILL this process ever do again?" Pledges are
// monotonically narrowing: a process that drops PLEDGE_FS_WRITE can never call
// a write syscall again for the rest of its lifetime. Only SYS_SPAWN (creating
// a new process with explicit pledge_subset) resets the mask.
//
// 16 bits total. 12 classes are defined; bits 12..15 are reserved and must be
// zero in any user-provided mask. PLEDGE_ALL = 0x0FFF is the spawn default.
// PLEDGE_NONE = 0 is invalid (every running process needs at least COMPUTE +
// TIME to loop and sleep).
//
// The hot path — pledge_allows() — is a header-only inline: one AND + one
// compare. Every sensitive syscall handler calls it at entry.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Packed-struct wrapper so the type is distinct from a bare uint16_t and the
// compiler catches accidental arithmetic (Phase 15a used the same trick for
// cap_token_t).
typedef struct {
    uint16_t raw;
} pledge_mask_t;

// ---------------------------------------------------------------------------
// Class bit positions (0..11). Reserved: 12..15.
// ---------------------------------------------------------------------------
#define PLEDGE_CLASS_FS_READ       0
#define PLEDGE_CLASS_FS_WRITE      1
#define PLEDGE_CLASS_NET_CLIENT    2
#define PLEDGE_CLASS_NET_SERVER    3
#define PLEDGE_CLASS_SPAWN         4
#define PLEDGE_CLASS_IPC_SEND      5
#define PLEDGE_CLASS_IPC_RECV      6
#define PLEDGE_CLASS_SYS_QUERY     7
#define PLEDGE_CLASS_SYS_CONTROL   8
#define PLEDGE_CLASS_AI_CALL       9
#define PLEDGE_CLASS_COMPUTE      10
#define PLEDGE_CLASS_TIME         11
// Phase 21: device-owning daemons.
#define PLEDGE_CLASS_STORAGE_SERVER  12  // AHCI / NVMe daemon (Phase 23)
#define PLEDGE_CLASS_INPUT_SERVER    13  // keyboard / mouse daemon (Phase 27)

// Bit values as masks (for direct OR into pledge_mask_t.raw).
#define PLEDGE_FS_READ     (1u << PLEDGE_CLASS_FS_READ)
#define PLEDGE_FS_WRITE    (1u << PLEDGE_CLASS_FS_WRITE)
#define PLEDGE_NET_CLIENT  (1u << PLEDGE_CLASS_NET_CLIENT)
#define PLEDGE_NET_SERVER  (1u << PLEDGE_CLASS_NET_SERVER)
#define PLEDGE_SPAWN       (1u << PLEDGE_CLASS_SPAWN)
#define PLEDGE_IPC_SEND    (1u << PLEDGE_CLASS_IPC_SEND)
#define PLEDGE_IPC_RECV    (1u << PLEDGE_CLASS_IPC_RECV)
#define PLEDGE_SYS_QUERY   (1u << PLEDGE_CLASS_SYS_QUERY)
#define PLEDGE_SYS_CONTROL (1u << PLEDGE_CLASS_SYS_CONTROL)
#define PLEDGE_AI_CALL     (1u << PLEDGE_CLASS_AI_CALL)
#define PLEDGE_COMPUTE     (1u << PLEDGE_CLASS_COMPUTE)
#define PLEDGE_TIME        (1u << PLEDGE_CLASS_TIME)
#define PLEDGE_STORAGE_SERVER  (1u << PLEDGE_CLASS_STORAGE_SERVER)
#define PLEDGE_INPUT_SERVER    (1u << PLEDGE_CLASS_INPUT_SERVER)

// Phase 21: bits 12..13 are now in use; bits 14..15 still reserved.
#define PLEDGE_RESERVED_MASK 0xC000u   // Bits 14..15; must be zero.
#define PLEDGE_ALL           0x3FFFu   // All 14 classes.
#define PLEDGE_NONE          0x0000u   // Invalid for a running process.

// Default minimum a process must retain: COMPUTE | TIME. Spec prohibits
// PLEDGE_NONE as a target mask, so pledge_narrow(..., 0) returns -EINVAL.
#define PLEDGE_MIN_ALLOWED   (PLEDGE_COMPUTE | PLEDGE_TIME)

// Forward declaration so pledge.h doesn't have to pull in sched.h (which pulls
// in handle_table.h, etc.). Call sites that actually invoke pledge_allows()
// must have task_t's full definition visible (they all go through sched.h).
// The matching tagged-struct definition lives in sched.h.
struct task_struct;
typedef struct task_struct task_t;

// ---------------------------------------------------------------------------
// Hot path: does `mask` permit class `c`? One AND + one compare.
// ---------------------------------------------------------------------------
static inline bool pledge_mask_allows(pledge_mask_t mask, uint8_t class_bit) {
    return (mask.raw & (uint16_t)(1u << class_bit)) != 0;
}

// Convenience: given a task pointer, check its current pledge_mask. Requires
// the caller to have included a header that fully defines task_t (typically
// sched.h). Implemented as a macro to avoid the circular-include issue.
#define pledge_allows(task_ptr, class_bit) \
    pledge_mask_allows((task_ptr)->pledge_mask, (class_bit))

// ---------------------------------------------------------------------------
// Lifecycle operations (implemented in pledge.c).
// ---------------------------------------------------------------------------

// Initialise a freshly-allocated task's pledge mask to `initial`. Called from
// sched_init / sched_create_task / sched_create_user_process / sched_spawn.
// Does NOT take the pledge_lock (task is not yet reachable).
void pledge_init(task_t *task, pledge_mask_t initial);

// SYS_PLEDGE handler. Validates: `new` has no reserved bits set, `new` != 0,
// and `(current & new) == new` (strict subset). Takes task->pledge_lock,
// atomically stores the new mask, emits AUDIT_PLEDGE_NARROW on success, emits
// AUDIT_CAP_VIOLATION on widen/invalid attempts. Returns 0 on success,
// CAP_V2_EPERM for widen, CAP_V2_EINVAL for bad input.
int pledge_narrow(task_t *task, pledge_mask_t new_mask);

// Pretty-print a mask as a CSV of class names into `buf` (guaranteed
// zero-terminated). Returns the number of bytes written (excluding NUL).
// Used by auditq's --json mode and test helpers.
int pledge_mask_describe(pledge_mask_t mask, char *buf, int buflen);
