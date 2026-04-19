// kernel/log.c
// Phase 13: klog ring implementation. See log.h for the ABI contract.
//
// Design notes:
//   - The ring lives in BSS. Size is 16384 * 256 B = 4 MiB. Current
//     kernel BSS headroom is ~9 MiB so we fit with margin.
//   - Writes are protected by a single spinlock. The lock is held for
//     < 1 microsecond per write (a memcpy plus two atomic bumps).
//     Concurrency was benchmarked in user/tests/klog_stress at 10 000
//     writes across 4 children — no contention visible to userspace.
//   - Every write marks the destination entry with KLOG_GUARD_BIT
//     before copying fields in, then clears the bit after the last
//     byte of `message` is written. The oops path reads the ring
//     WITHOUT the lock (all other CPUs are halted), so it must skip
//     any entry whose guard bit is still set.
//   - `next_seq` is a free-running u64; readers use it to detect drops
//     after a wrap by observing seq gaps.

#include "log.h"
#include "vsnprintf.h"
#include "sync/spinlock.h"

#include "../arch/x86_64/drivers/serial/serial.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/smp.h"

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

// --- Ring state -------------------------------------------------------

// The ring lives in BSS. `entries` is zero-initialised, which leaves
// every slot's guard bit cleared — so even a fresh read before any
// writer lands returns "empty, no valid entries" cleanly.
static klog_entry_t g_entries[KLOG_RING_ENTRIES];

static struct {
    uint64_t    head;            // total entries written since boot
    uint64_t    next_seq;        // next seq to stamp (== head + 1 of first ever)
    uint64_t    dropped_panic;   // failed-to-write due to panic re-entry
    bool        initialized;
    bool        mirror_to_serial;
    spinlock_t  lock;
} g_ring = {
    .head = 0,
    .next_seq = 1,
    .dropped_panic = 0,
    .initialized = false,
    .mirror_to_serial = false,
    .lock = SPINLOCK_INITIALIZER("klog_ring"),
};

// Pre-klog_init drops. Bumped atomically before the ring is usable.
// A retrospective "dropped N early-boot messages" entry is emitted
// once klog_init() runs, if this is nonzero.
static volatile uint64_t g_early_drops = 0;

// --- Subsystem registry -----------------------------------------------

static const char *g_subsys_names[KLOG_MAX_SUBSYS];

static const char *const g_kernel_subsys_names[] = {
    [SUBSYS_CORE]    = "CORE",
    [SUBSYS_MM]      = "MM",
    [SUBSYS_SCHED]   = "SCHED",
    [SUBSYS_SYSCALL] = "SYSCALL",
    [SUBSYS_VFS]     = "VFS",
    [SUBSYS_FS]      = "FS",
    [SUBSYS_NET]     = "NET",
    [SUBSYS_CAP]     = "CAP",
    [SUBSYS_DRV]     = "DRV",
    [SUBSYS_TEST]    = "TEST",
    [SUBSYS_AUDIT]   = "AUDIT",
};

static const char *const g_level_names[6] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

// --- External globals we sample --------------------------------------

extern volatile uint64_t g_timer_ticks;  // arch/x86_64/cpu/interrupts.c

// --- Helpers ----------------------------------------------------------

static uint64_t current_ns(void) {
    // LAPIC timer fires at 100 Hz (10 ms per tick). Coarse — sub-tick
    // resolution requires TSC calibration which Phase 13 doesn't need.
    return g_timer_ticks * 10000000ULL;
}

static int16_t current_pid(void) {
    task_t *t = sched_get_current_task();
    if (!t) return -1;
    return (int16_t)t->id;
}

static uint16_t current_cpu(void) {
    return (uint16_t)smp_get_current_cpu();
}

// emit_dec_padded was folded into mirror_entry_to_serial's local line
// buffer as part of the Phase 15b console-atomicity fix. Left intentionally
// removed; if panic/oops path needs it, re-introduce with a local buffer.

// Mirror the entry to serial. Format matches what parse_tap_py and
// dmesg readers expect:
//   [  T.TTTTTTTTT] LEVEL SUBSYS msg\n
// Timestamp is seconds.nanoseconds, always 9 fractional digits.
//
// Phase 15b: build the full line in a local buffer then emit via
// serial_write_n in one locked burst. Without this, concurrent klog
// mirrors and user SYS_WRITE console output interleave byte-by-byte on
// the UART, which corrupted TAP parser input (~10% test flake rate).
// Local buffer is generously sized so every line fits.
static void mirror_entry_to_serial(const klog_entry_t *e) {
    // Max length: "[NNNNNNNN.NNNNNNNNN] LEVEL SUBSYS<msg>\n"
    //           = 1 + 8 + 1 + 9 + 1 + 1 + 5 + 1 + 7 + 1 + KLOG_MSG_LEN + 1
    //          ~= 256 + message body. Use 512 for margin.
    char line[512];
    size_t off = 0;

    #define PUT_CHAR(ch) do { if (off + 1 < sizeof(line)) line[off++] = (char)(ch); } while (0)
    #define PUT_STR(s, n) do { \
        for (size_t _i = 0; _i < (size_t)(n) && off + 1 < sizeof(line); _i++) \
            line[off++] = (s)[_i]; \
    } while (0)

    uint64_t ns = e->ns_timestamp;
    uint64_t secs = ns / 1000000000ULL;
    uint64_t nsec = ns % 1000000000ULL;

    PUT_CHAR('[');
    // Left-pad seconds to 4 chars for visual alignment.
    {
        char tmp[16]; int n = 0; uint64_t v = secs;
        if (v == 0) tmp[n++] = '0';
        while (v > 0 && n < 16) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
        for (int i = n; i < 4; i++) PUT_CHAR(' ');
        while (n > 0) PUT_CHAR(tmp[--n]);
    }
    PUT_CHAR('.');
    // nsec, zero-padded to 9 digits.
    {
        char tmp[16]; int n = 0; uint64_t v = nsec;
        if (v == 0) tmp[n++] = '0';
        while (v > 0 && n < 16) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
        int pad = 9 - n;
        while (pad-- > 0) PUT_CHAR('0');
        while (n > 0) PUT_CHAR(tmp[--n]);
    }
    PUT_CHAR(']');
    PUT_CHAR(' ');

    const char *lvl = klog_level_name(e->level & KLOG_LEVEL_MASK);
    int lvl_len = 0;
    while (lvl[lvl_len]) lvl_len++;
    PUT_STR(lvl, lvl_len);
    for (int i = lvl_len; i < 5; i++) PUT_CHAR(' ');
    PUT_CHAR(' ');

    const char *sub = klog_subsys_name(e->subsystem_id);
    int sub_len = 0;
    while (sub[sub_len]) sub_len++;
    PUT_STR(sub, sub_len);
    for (int i = sub_len; i < 7; i++) PUT_CHAR(' ');
    PUT_CHAR(' ');

    int mlen = 0;
    while (mlen < KLOG_MSG_LEN && e->message[mlen]) mlen++;
    PUT_STR(e->message, mlen);
    PUT_CHAR('\n');

    serial_write_n(line, off);

    #undef PUT_CHAR
    #undef PUT_STR
}

// --- Core write path --------------------------------------------------

static void klog_write_formatted(uint8_t level, uint8_t subsys, int16_t pid,
                                 const char *msg, size_t msg_len) {
    if (!g_ring.initialized) {
        __atomic_add_fetch(&g_early_drops, 1, __ATOMIC_RELAXED);
        return;
    }

    // Clamp what we copy to fit in the fixed-size message slot; leave
    // room for the null terminator.
    if (msg_len >= KLOG_MSG_LEN) msg_len = KLOG_MSG_LEN - 1;

    spinlock_acquire(&g_ring.lock);

    uint64_t slot = g_ring.head & KLOG_RING_MASK;
    klog_entry_t *e = &g_entries[slot];

    // Stamp the guard bit BEFORE any field write so any concurrent
    // unlocked reader (the panic path) sees "in flight".
    e->level = (uint8_t)((level & KLOG_LEVEL_MASK) | KLOG_GUARD_BIT);

    e->ns_timestamp = current_ns();
    e->seq          = g_ring.next_seq;
    e->cpu_id       = current_cpu();
    e->pid          = pid;
    e->subsystem_id = subsys;
    e->reserved[0]  = 0;
    e->reserved[1]  = 0;
    e->reserved[2]  = 0;
    e->reserved[3]  = 0;

    // Copy message bytes.
    size_t i;
    for (i = 0; i < msg_len; i++) e->message[i] = msg[i];
    e->message[i] = '\0';
    // Zero the tail of the message slot so leftover bytes from a
    // previous occupant of this ring slot don't leak.
    for (i = msg_len + 1; i < KLOG_MSG_LEN; i++) e->message[i] = '\0';

    // Final store: clear the guard bit. Any reader from this point
    // sees a consistent entry.
    e->level = (uint8_t)(level & KLOG_LEVEL_MASK);

    g_ring.head++;
    g_ring.next_seq++;

    // Snapshot for the optional mirror pass, done OUTSIDE the lock
    // so slow serial transmit doesn't block other CPUs. Copy now so
    // the entry we serialise matches what's in the ring even if the
    // ring later wraps over the slot.
    klog_entry_t snapshot;
    bool do_mirror = g_ring.mirror_to_serial;
    if (do_mirror) snapshot = *e;

    spinlock_release(&g_ring.lock);

    if (do_mirror) mirror_entry_to_serial(&snapshot);
}

// --- Public API -------------------------------------------------------

void klog_init(void) {
    // Populate kernel subsystem names before accepting any writes.
    for (size_t i = 0; i < sizeof(g_kernel_subsys_names) /
                         sizeof(g_kernel_subsys_names[0]); i++) {
        g_subsys_names[i] = g_kernel_subsys_names[i];
    }

    g_ring.mirror_to_serial = (KLOG_MIRROR_DEFAULT != 0);
    g_ring.initialized = true;

    // Emit an opening breadcrumb so boot logs have a visible
    // transition from "raw serial_write chatter" to "klog is live".
    klog(KLOG_INFO, SUBSYS_CORE, "klog ring online (16384 x 256 B)");

    // Retrospective drop summary, if any early-boot writes raced
    // against initialisation. This is a one-line post-hoc note; we
    // do NOT try to resurrect the original message text.
    uint64_t drops = __atomic_load_n(&g_early_drops, __ATOMIC_RELAXED);
    if (drops > 0) {
        klog(KLOG_WARN, SUBSYS_CORE,
             "dropped %lu early-boot messages (before klog_init)", drops);
    }
}

void klog(uint8_t level, uint8_t subsys, const char *fmt, ...) {
    char buf[192];

    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) n = 0;
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;

    klog_write_formatted(level, subsys, current_pid(), buf, len);
}

void klog_raw(uint8_t level, uint8_t subsys, int16_t pid,
              const char *msg, size_t msg_len) {
    if (!msg) { msg = ""; msg_len = 0; }
    klog_write_formatted(level, subsys, pid, msg, msg_len);
}

int klog_register_subsystem(uint8_t subsys, const char *name) {
    if (subsys < KLOG_FIRST_USER_SUBSYS) return -1;
    if (!name) return -1;
    // Collision check — once registered, stays registered for this boot.
    if (g_subsys_names[subsys] != NULL) return -1;
    g_subsys_names[subsys] = name;
    return 0;
}

void klog_disable_mirror(void) {
    spinlock_acquire(&g_ring.lock);
    g_ring.mirror_to_serial = false;
    spinlock_release(&g_ring.lock);
}

void klog_get_stats(uint64_t *total_written, uint64_t *dropped_panic,
                    uint64_t *early_drops, uint64_t *next_seq_out) {
    spinlock_acquire(&g_ring.lock);
    if (total_written) *total_written = g_ring.head;
    if (dropped_panic) *dropped_panic = g_ring.dropped_panic;
    if (next_seq_out)  *next_seq_out  = g_ring.next_seq;
    spinlock_release(&g_ring.lock);
    if (early_drops) {
        *early_drops = __atomic_load_n(&g_early_drops, __ATOMIC_RELAXED);
    }
}

const char *klog_subsys_name(uint8_t subsys) {
    const char *n = g_subsys_names[subsys];
    if (n) return n;
    // Return a stable fallback for unregistered subsys ids. Caller
    // may hold the pointer past the next klog call, so use static.
    static char fallback[8];
    fallback[0] = 'U'; fallback[1] = 'S'; fallback[2] = 'R';
    uint8_t v = subsys;
    if (v >= 100) {
        fallback[3] = (char)('0' + v / 100); v %= 100;
        fallback[4] = (char)('0' + v / 10);  v %= 10;
        fallback[5] = (char)('0' + v);
        fallback[6] = '\0';
    } else if (v >= 10) {
        fallback[3] = (char)('0' + v / 10);  v %= 10;
        fallback[4] = (char)('0' + v);
        fallback[5] = '\0';
    } else {
        fallback[3] = (char)('0' + v);
        fallback[4] = '\0';
    }
    return fallback;
}

const char *klog_level_name(uint8_t level) {
    uint8_t l = level & KLOG_LEVEL_MASK;
    if (l > KLOG_FATAL) return "?????";
    return g_level_names[l];
}

uint64_t klog_head_absolute(void) {
    return __atomic_load_n(&g_ring.head, __ATOMIC_RELAXED);
}

const klog_entry_t *klog_ring_raw(void) {
    return g_entries;
}

// --- Read path --------------------------------------------------------

int klog_read_filtered(uint8_t level_mask, uint32_t tail_count,
                       void *user_buf, size_t buf_cap) {
    if (!user_buf) return -1;
    if (!g_ring.initialized) return 0;

    klog_entry_t *dst = (klog_entry_t *)user_buf;
    size_t dst_max = buf_cap / sizeof(klog_entry_t);
    if (dst_max == 0) return 0;

    // Snapshot under the lock so head doesn't slide while we're
    // computing the walk range.
    spinlock_acquire(&g_ring.lock);
    uint64_t head = g_ring.head;
    spinlock_release(&g_ring.lock);

    // How many entries are available? Capped at ring capacity.
    uint64_t available = head;
    if (available > KLOG_RING_ENTRIES) available = KLOG_RING_ENTRIES;

    // tail_count = 0 means "every entry currently in the ring".
    uint64_t walk = tail_count;
    if (walk == 0 || walk > available) walk = available;

    // Starting absolute index, oldest-first, so the user buffer ends
    // up in chronological order.
    uint64_t start = head - walk;

    size_t copied = 0;
    for (uint64_t i = start; i < head && copied < dst_max; i++) {
        const klog_entry_t *src = &g_entries[i & KLOG_RING_MASK];

        // Skip in-flight entries. Another writer is actively mutating
        // this slot; the oops path in particular might see this and
        // we want predictable results.
        if (src->level & KLOG_GUARD_BIT) continue;

        // Filter by level bitmap. level_mask == 0 means "include all".
        if (level_mask != 0) {
            uint8_t lv = src->level & KLOG_LEVEL_MASK;
            if (((level_mask >> lv) & 1u) == 0) continue;
        }

        dst[copied] = *src;
        copied++;
    }

    return (int)copied;
}
