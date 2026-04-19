// kernel/log.h
// Phase 13: structured kernel log (klog). Replaces 500+ ad-hoc
// serial_write("[TAG] ...") call sites across kernel/ and arch/.
//
// Core guarantees:
//   - Every entry is a fixed 256-byte record → simple ring indexing.
//   - Strictly monotonic, never-wrapping 64-bit sequence number per
//     entry; a reader detects drops by gaps in seq.
//   - Ring is 16384 entries (4 MiB) — ~16 minutes at 16 entries/s
//     typical, ~4 s under stress. Oldest is overwritten on wrap.
//   - Per-entry "in-flight" guard bit (high bit of `level`) lets
//     kpanic safely dump the ring without holding the spinlock.
//
// This header is shared kernel-internally; user-space sees klog entries
// only via the SYS_KLOG_READ syscall (state.h declares the ABI copy).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- Level values -----------------------------------------------------
// Low nibble holds the actual level 0..5. The high bit (0x80) is the
// in-flight guard flag reserved for the writer; readers MUST mask with
// KLOG_LEVEL_MASK before comparing.
#define KLOG_TRACE        0
#define KLOG_DEBUG        1
#define KLOG_INFO         2
#define KLOG_WARN         3
#define KLOG_ERROR        4
#define KLOG_FATAL        5

#define KLOG_LEVEL_MASK   0x0F
#define KLOG_GUARD_BIT    0x80

// --- Subsystem ids ----------------------------------------------------
// 0..9 are kernel-reserved and pre-registered at klog_init time.
// 10..255 are user-registerable via klog_register_subsystem() OR
// supplied verbatim by SYS_KLOG_WRITE (which rejects anything < 10).
#define SUBSYS_CORE       0
#define SUBSYS_MM         1
#define SUBSYS_SCHED      2
#define SUBSYS_SYSCALL    3
#define SUBSYS_VFS        4
#define SUBSYS_FS         5
#define SUBSYS_NET        6
#define SUBSYS_CAP        7
#define SUBSYS_DRV        8
#define SUBSYS_TEST       9
// Phase 15b: dedicated subsystem for persistent audit log (separate from klog).
#define SUBSYS_AUDIT     10

#define KLOG_MAX_SUBSYS       256
#define KLOG_FIRST_USER_SUBSYS 11

// --- Record layout ----------------------------------------------------
// 256 bytes exactly. Fields are laid out for natural alignment and
// cacheline friendliness. Do NOT reorder without bumping the oops
// frame magic — parse_oops.py expects this layout.
#define KLOG_MSG_LEN       224

typedef struct klog_entry {
    uint64_t ns_timestamp;    //  0  monotonic nanoseconds since boot
    uint64_t seq;             //  8  strictly monotonic, never wraps
    uint16_t cpu_id;          // 16  emitting CPU (0..31)
    int16_t  pid;             // 18  task pid, -1 pre-scheduler, -2 IRQ
    uint8_t  level;           // 20  KLOG_* (with optional guard bit)
    uint8_t  subsystem_id;    // 21  SUBSYS_* (0..255)
    uint8_t  reserved[4];     // 22  Phase 15b audit correlation id etc
    char     message[KLOG_MSG_LEN]; // 26  null-terminated formatted text
    uint8_t  _pad[6];         // 250 padding to 256 bytes
} klog_entry_t;                // total 256

_Static_assert(sizeof(klog_entry_t) == 256,
               "klog_entry_t must be exactly 256 bytes");

// --- Ring -------------------------------------------------------------
// The ring is a kernel-internal singleton. log.c owns the storage.

#define KLOG_RING_ENTRIES  16384   // 2^14 → index mask = KLOG_RING_ENTRIES - 1
#define KLOG_RING_MASK     (KLOG_RING_ENTRIES - 1)

// Default for `mirror_to_serial`. Stays `1` while Phase 13 is in
// development so developers can still watch serial. Phase 13 exit
// drops this to `0`; verified by a boot-time regression test.
#ifndef KLOG_MIRROR_DEFAULT
#define KLOG_MIRROR_DEFAULT 1
#endif

// --- Public API -------------------------------------------------------
void klog_init(void);

// Emit a record. Thread-safe, interrupt-safe when called from a normal
// IRQ ISR (uses a spinlock that saves/restores RFLAGS). If the ring is
// not yet initialised, bumps g_early_drops silently.
void klog(uint8_t level, uint8_t subsys, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Emit a record whose message is already fully formed (no format pass).
// Used by the SYS_KLOG_WRITE handler so userspace cannot trip kernel
// vsnprintf on a pathological format string.
void klog_raw(uint8_t level, uint8_t subsys, int16_t pid,
              const char *msg, size_t msg_len);

// Copy recent entries matching the level mask into a user buffer.
// Returns number of entries written, or a negative errno on a
// copy_to_user failure.
//
//   level_mask  bitmap, bit N = include level N. 0 means "all levels".
//   tail_count  walk at most this many most-recent entries (0 = all
//               currently in ring; max 16384).
//   user_buf    destination, must be aligned for klog_entry_t.
//   buf_cap     buffer size in bytes. Truncation is silent — caller
//               gets a count ≤ buf_cap / sizeof(klog_entry_t).
int klog_read_filtered(uint8_t level_mask, uint32_t tail_count,
                       void *user_buf, size_t buf_cap);

// Register a name for a user-range subsystem (10..255). Returns 0 on
// success, -1 on out-of-range or collision. Names are not copied —
// the pointer must remain valid for the kernel's lifetime (so pass
// string literals or long-lived globals).
int klog_register_subsystem(uint8_t subsys, const char *name);

// Disable mirror-to-serial. Irreversible within a boot; used at Phase
// 13 exit after we have validated the boot-time regression is <500 ms.
void klog_disable_mirror(void);

// Runtime accessor for /bin/klog and integration tests.
void klog_get_stats(uint64_t *total_written, uint64_t *dropped_panic,
                    uint64_t *early_drops, uint64_t *next_seq_out);

// Subsystem name lookup, used by both the serial mirror and /bin/klog.
// Returns a string like "CORE" or "USR42"; never NULL.
const char *klog_subsys_name(uint8_t subsys);

// Level name lookup. Returns "TRACE".."FATAL".
const char *klog_level_name(uint8_t level);

// Test/introspection hook. Returns head index (absolute, not wrapped).
uint64_t klog_head_absolute(void);

// Internal: panic path uses this to iterate the last N entries WITHOUT
// acquiring the ring lock. The guard bit on each entry tells the caller
// whether the entry is half-written; those must be skipped.
const klog_entry_t *klog_ring_raw(void);
