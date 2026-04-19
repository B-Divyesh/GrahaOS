// kernel/audit.c
//
// Phase 15b — audit log writer, flusher, query.
//
// This file provides the enqueue path (audit_write_*) and query helpers. The
// on-disk flusher that drains the queue to /var/audit/YYYY-MM-DD.log is
// implemented below too, but deliberately kept simple for Phase 15b U3:
// the flusher loops and writes entries to klog (SUBSYS_AUDIT) as a
// klog-only fallback. U9 replaces the stub with real grahafs-backed I/O.
#include "audit.h"
#include "cap/token.h"          // CAP_V2_* errno constants used by Phase 17 writers

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "rtc.h"
#include "sync/spinlock.h"
#include "vsnprintf.h"
#include "fs/vfs.h"       // vfs_node_t, ssize_t, vfs_create, vfs_path_to_node
#include "fs/grahafs.h"   // grahafs_read, grahafs_write

// LAPIC tick counter (100 Hz) from arch/x86_64/cpu/interrupts.c.
extern volatile uint64_t g_timer_ticks;

audit_state_t g_audit_state;

// ---------------------------------------------------------------------------
// Internal helpers.
// ---------------------------------------------------------------------------

static uint64_t audit_ns_now(void) {
    return g_timer_ticks * 10000000ULL;  // 10 ms per tick -> ns.
}

static void copy_detail(char dst[192], const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < 191 && src[i] != '\0'; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Build a base audit_entry_t. Caller fills event-specific fields.
static void fill_base(audit_entry_t *e, uint16_t event_type,
                      int32_t subject_pid, uint8_t src) {
    memset(e, 0, sizeof(*e));
    e->magic              = AUDIT_ENTRY_MAGIC;
    e->schema_version     = AUDIT_SCHEMA_VERSION;
    e->event_type         = event_type;
    e->ns_timestamp       = audit_ns_now();
    e->wall_clock_seconds = rtc_now_seconds();
    e->subject_pid        = subject_pid;
    e->object_idx         = 0xFFFFFFFFu;
    e->result_code        = 0;
    e->audit_source       = src;
}

// Pretty name for klog when we haven't yet attached to the FS. Order matches
// AUDIT_* event values.
static const char *event_name(uint16_t ev) {
    static const char *names[AUDIT_EVENT_MAX + 1] = {
        "NONE",
        "CAP_REGISTER",
        "CAP_UNREGISTER",
        "CAP_DERIVE",
        "CAP_REVOKE",
        "CAP_GRANT",
        "CAP_VIOLATION",
        "PLEDGE_NARROW",
        "SPAWN",
        "KILL",
        "FS_WRITE_CRITICAL",
        "MMIO_DIRECT",
        "REBOOT",
        "NET_BIND",
        "AI_INVOKE",
        "CAP_ACTIVATE",
        "CAP_DEACTIVATE",
        "DEPRECATED_SYSCALL",
        "CHAN_SEND",
        "CHAN_RECV",
        "CHAN_TYPE_MISMATCH",
        "VMO_FAULT",
        "HANDLE_TRANSFER",
        "STREAM_OP_REJECTED",
        "STREAM_DESTROY_CANCELED",
    };
    if (ev > AUDIT_EVENT_MAX) return "UNKNOWN";
    return names[ev];
}

// Enqueue into the ring. Never blocks — the ring is overwrite-oldest, so
// producers always make forward progress. If the flusher has fallen so far
// behind that the slot we'd overwrite hasn't been persisted yet, we bump
// `overwritten` so operators can detect data loss after a flusher stall.
static void audit_enqueue(const audit_entry_t *e) {
    audit_queue_t *q = &g_audit_state.queue;

    spinlock_acquire(&q->lock);
    uint32_t slot = (uint32_t)(q->total_written % AUDIT_QUEUE_SIZE);
    q->entries[slot] = *e;
    q->total_written++;
    // If the flusher has fallen behind by more than the ring size, the slot
    // we just wrote was previously occupied by an entry that never reached
    // disk. Advance disk_cursor to the oldest still-valid slot so the flusher
    // skips the lost region on its next pass.
    if (q->total_written - q->disk_cursor > AUDIT_QUEUE_SIZE) {
        q->overwritten += (q->total_written - q->disk_cursor) - AUDIT_QUEUE_SIZE;
        q->disk_cursor = q->total_written - AUDIT_QUEUE_SIZE;
    }
    g_audit_state.next_seq++;
    spinlock_release(&q->lock);

    // Mirror every entry to klog while the flusher is stubbed. U9 will
    // condition this on g_audit_state.fs_attached being false.
    if (!g_audit_state.fs_attached) {
        klog(KLOG_INFO, SUBSYS_AUDIT,
             "audit[%lu] ev=%s pid=%d obj=0x%x rc=%d src=%s %.80s",
             (unsigned long)g_audit_state.next_seq,
             event_name(e->event_type),
             (int)e->subject_pid,
             e->object_idx,
             (int)e->result_code,
             e->audit_source == AUDIT_SRC_SHIM ? "SHIM" : "NATIVE",
             e->detail);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
void audit_init(void) {
    memset(&g_audit_state, 0, sizeof(g_audit_state));
    spinlock_init(&g_audit_state.queue.lock, "audit_queue");
    g_audit_state.flusher_task_id   = -1;
    g_audit_state.current_fd        = -1;
    g_audit_state.current_file      = NULL;
    g_audit_state.current_day_seconds = 0;
    g_audit_state.current_file_size   = 0;
    g_audit_state.current_entry_count = 0;
    g_audit_state.next_seq            = 0;
    g_audit_state.fs_attached         = false;
    klog(KLOG_INFO, SUBSYS_AUDIT, "audit: initialized (queue size=%u)", AUDIT_QUEUE_SIZE);
}

// ---------------------------------------------------------------------------
// On-disk file helpers.
// ---------------------------------------------------------------------------

// Compute the path for a given wall-day. Writes into `out` (buflen>=40).
// Example: format_log_filename(out, 40, 1713398400) -> "/var/audit/2026-04-18.log".
static void format_log_filename(char *out, int buflen, int64_t wall_seconds) {
    // Compute y/m/d from Unix seconds. Algorithm adapted from Hinnant.
    int64_t days = wall_seconds / 86400;
    if (wall_seconds < 0 && (wall_seconds % 86400) != 0) days--;
    // days since 1970-01-01 (Thursday).
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = (int)(yoe + era * 400);
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned d = doy - (153*mp + 2)/5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;
    // ksnprintf doesn't support width specifiers (%04d / %02u), so format
    // the zero-padded fields manually. Buffer layout:
    //   /var/audit/YYYY-MM-DD.log
    //   0         11  15 18      24 (len 24, NUL at 24)
    if (buflen < 25) {
        if (buflen > 0) out[0] = '\0';
        return;
    }
    const char *prefix = "/var/audit/";
    int i = 0;
    while (prefix[i]) { out[i] = prefix[i]; i++; }
    // YYYY (4 digits, zero-pad).
    out[i+3] = (char)('0' + (y % 10)); y /= 10;
    out[i+2] = (char)('0' + (y % 10)); y /= 10;
    out[i+1] = (char)('0' + (y % 10)); y /= 10;
    out[i+0] = (char)('0' + (y % 10));
    i += 4;
    out[i++] = '-';
    out[i++] = (char)('0' + (m / 10));
    out[i++] = (char)('0' + (m % 10));
    out[i++] = '-';
    out[i++] = (char)('0' + (d / 10));
    out[i++] = (char)('0' + (d % 10));
    const char *suffix = ".log";
    int j = 0;
    while (suffix[j]) { out[i++] = suffix[j++]; }
    out[i] = '\0';
}

// Write the 32-byte file header at offset 0. Updates current_entry_count.
static int write_file_header(vfs_node_t *node, int64_t day_seconds,
                             uint32_t entry_count) {
    audit_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.file_magic      = AUDIT_FILE_MAGIC;
    hdr.schema_version  = AUDIT_SCHEMA_VERSION;
    hdr.day_seconds     = day_seconds;
    hdr.entry_count     = entry_count;
    ssize_t w = grahafs_write(node, 0, sizeof(hdr), &hdr);
    return (w == (ssize_t)sizeof(hdr)) ? 0 : -1;
}

// Read back the file header. Returns 0 on success and fills out.
static int read_file_header(vfs_node_t *node, audit_file_header_t *out) {
    ssize_t r = grahafs_read(node, 0, sizeof(*out), out);
    if (r != (ssize_t)sizeof(*out)) return -1;
    if (out->file_magic != AUDIT_FILE_MAGIC) return -1;
    return 0;
}

// Open /var/audit/<today>.log, creating it if missing. Writes a fresh header
// when the file is new. Caches the vfs_node_t * in g_audit_state.
static int open_today_log(int64_t wall_now) {
    char path[48];
    format_log_filename(path, sizeof(path), wall_now);

    vfs_node_t *node = vfs_path_to_node(path);
    bool created = false;
    if (!node) {
        if (vfs_create(path, 0644) < 0) {
            klog(KLOG_ERROR, SUBSYS_AUDIT,
                 "audit: vfs_create(%s) failed", path);
            return -1;
        }
        node = vfs_path_to_node(path);
        if (!node) {
            klog(KLOG_ERROR, SUBSYS_AUDIT,
                 "audit: vfs_path_to_node(%s) failed after create", path);
            return -1;
        }
        created = true;
    }

    audit_file_header_t hdr;
    uint32_t existing = 0;
    if (!created && read_file_header(node, &hdr) == 0) {
        existing = hdr.entry_count;
    } else {
        // Fresh or corrupt header — write a new one.
        if (write_file_header(node, (wall_now / 86400) * 86400, 0) < 0) {
            klog(KLOG_ERROR, SUBSYS_AUDIT,
                 "audit: initial header write failed for %s", path);
            return -1;
        }
    }

    g_audit_state.current_file        = node;
    g_audit_state.current_day_seconds = (wall_now / 86400) * 86400;
    g_audit_state.current_file_size   = AUDIT_FILE_HEADER_SIZE +
                                        (uint64_t)existing * sizeof(audit_entry_t);
    g_audit_state.current_entry_count = existing;
    klog(KLOG_INFO, SUBSYS_AUDIT,
         "audit: attached to %s (existing=%u entries)", path, existing);
    return 0;
}

void audit_attach_fs(void) {
    int64_t wall_now = rtc_now_seconds();
    if (wall_now <= 0) {
        klog(KLOG_WARN, SUBSYS_AUDIT,
             "audit: no wall clock — staying in klog-only mode");
        g_audit_state.fs_attached = false;
        return;
    }
    if (open_today_log(wall_now) != 0) {
        klog(KLOG_WARN, SUBSYS_AUDIT,
             "audit: open_today_log failed — staying in klog-only mode");
        g_audit_state.fs_attached = false;
        return;
    }
    g_audit_state.fs_attached = true;
}

// Write one entry to the current file at the current append offset. Respects
// the GrahaFS Phase 7b 48 KiB file cap: if a write would exceed it, klog a
// WARN once (via dropped counter) and leave the entry in the memory ring.
static int persist_one_entry(const audit_entry_t *e) {
    vfs_node_t *node = (vfs_node_t *)g_audit_state.current_file;
    if (!node) return -1;
    uint64_t off = g_audit_state.current_file_size;
    if (off + sizeof(*e) > 48u * 1024u) {
        // File would exceed GrahaFS' 48 KiB direct-block cap. Phase 19 fixes
        // this. For Phase 15b we stop persisting further entries to this
        // file and rely on the in-memory ring.
        return -2;  // "full"
    }
    ssize_t w = grahafs_write(node, off, sizeof(*e), (void *)e);
    if (w != (ssize_t)sizeof(*e)) return -1;
    g_audit_state.current_file_size = off + sizeof(*e);
    g_audit_state.current_entry_count++;
    return 0;
}

// Update the file header's entry_count (cheap — 32 B write at offset 0).
static void update_header_count(void) {
    vfs_node_t *node = (vfs_node_t *)g_audit_state.current_file;
    if (!node) return;
    (void)write_file_header(node, g_audit_state.current_day_seconds,
                            g_audit_state.current_entry_count);
}

// If the wall-day has advanced, open a new log file and reset the cursors.
static void rotate_if_needed(int64_t wall_now) {
    int64_t today_start = (wall_now / 86400) * 86400;
    if (today_start <= g_audit_state.current_day_seconds) return;
    klog(KLOG_INFO, SUBSYS_AUDIT, "audit: rotating to new day");
    g_audit_state.current_file = NULL;
    (void)open_today_log(wall_now);
}

void audit_flusher_task_entry(void) {
    // Loop forever: drain up to AUDIT_FLUSH_BATCH entries per wake, persist
    // each to disk, yield. On file-full (GrahaFS 48 KiB cap), increments
    // dropped counter and continues — the producer-side ring retains the
    // entry for SYS_AUDIT_QUERY.
    bool warned_full = false;
    for (;;) {
        if (g_audit_state.fs_attached) {
            int64_t now = rtc_now_seconds();
            rotate_if_needed(now);

            audit_queue_t *q = &g_audit_state.queue;
            audit_entry_t batch[AUDIT_FLUSH_BATCH];
            uint32_t batch_n = 0;

            spinlock_acquire(&q->lock);
            while (batch_n < AUDIT_FLUSH_BATCH && q->disk_cursor < q->total_written) {
                uint32_t slot = (uint32_t)(q->disk_cursor % AUDIT_QUEUE_SIZE);
                batch[batch_n++] = q->entries[slot];
                q->disk_cursor++;
            }
            spinlock_release(&q->lock);

            if (batch_n > 0) {
                bool any_written = false;
                for (uint32_t i = 0; i < batch_n; i++) {
                    int r = persist_one_entry(&batch[i]);
                    if (r == -2) {
                        if (!warned_full) {
                            klog(KLOG_WARN, SUBSYS_AUDIT,
                                 "audit: /var/audit log file hit 48 KiB cap (Phase 19 raises this)");
                            warned_full = true;
                        }
                        spinlock_acquire(&q->lock);
                        q->dropped++;
                        spinlock_release(&q->lock);
                    } else if (r < 0) {
                        spinlock_acquire(&q->lock);
                        q->dropped++;
                        spinlock_release(&q->lock);
                    } else {
                        any_written = true;
                    }
                }
                if (any_written) update_header_count();
            }
        }
        // Cooperative yield; LAPIC tick (~10 ms) wakes us eventually.
        asm volatile ("hlt");
    }
}

// ---------------------------------------------------------------------------
// Convenience writers.
// ---------------------------------------------------------------------------
void audit_write_cap_register(uint32_t obj_idx, const char *name, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_REGISTER, /*pid*/ -1, src);
    e.object_idx = obj_idx;
    copy_detail(e.detail, name ? name : "");
    audit_enqueue(&e);
}

void audit_write_cap_unregister(uint32_t obj_idx, const char *name, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_UNREGISTER, /*pid*/ -1, src);
    e.object_idx = obj_idx;
    copy_detail(e.detail, name ? name : "");
    audit_enqueue(&e);
}

void audit_write_cap_derive(int32_t caller_pid, uint32_t parent_idx,
                            uint32_t new_idx, uint64_t rights, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_DERIVE, caller_pid, src);
    e.object_idx      = new_idx;
    e.rights_required = rights;
    char buf[64];
    ksnprintf(buf, sizeof(buf), "parent=0x%x rights=0x%lx",
              parent_idx, (unsigned long)rights);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_cap_revoke(int32_t caller_pid, uint32_t obj_idx,
                            int32_t result, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_REVOKE, caller_pid, src);
    e.object_idx  = obj_idx;
    e.result_code = result;
    audit_enqueue(&e);
}

void audit_write_cap_grant(int32_t caller_pid, int32_t target_pid,
                           uint32_t obj_idx, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_GRANT, caller_pid, src);
    e.object_idx = obj_idx;
    char buf[40];
    ksnprintf(buf, sizeof(buf), "target_pid=%d", (int)target_pid);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_cap_violation(int32_t caller_pid, uint32_t obj_idx,
                               int32_t result,
                               uint64_t rights_required,
                               uint64_t rights_held,
                               const char *detail, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_VIOLATION, caller_pid, src);
    e.object_idx       = obj_idx;
    e.result_code      = result;
    e.rights_required  = rights_required;
    e.rights_held      = rights_held;
    copy_detail(e.detail, detail);
    audit_enqueue(&e);
}

void audit_write_pledge_narrow(int32_t pid, uint16_t old_mask, uint16_t new_mask) {
    audit_entry_t e;
    fill_base(&e, AUDIT_PLEDGE_NARROW, pid, AUDIT_SRC_NATIVE);
    e.pledge_old = old_mask;
    e.pledge_new = new_mask;
    char buf[64];
    ksnprintf(buf, sizeof(buf), "0x%x -> 0x%x",
              (unsigned)old_mask, (unsigned)new_mask);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_pledge_violation(int32_t pid, uint16_t old_mask,
                                  uint16_t new_mask, const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_VIOLATION, pid, AUDIT_SRC_NATIVE);
    e.pledge_old  = old_mask;
    e.pledge_new  = new_mask;
    e.result_code = -1;
    copy_detail(e.detail, detail);
    audit_enqueue(&e);
}

void audit_write_spawn(int32_t parent_pid, int32_t child_pid, const char *path) {
    audit_entry_t e;
    fill_base(&e, AUDIT_SPAWN, parent_pid, AUDIT_SRC_NATIVE);
    e.object_idx = (uint32_t)child_pid;
    copy_detail(e.detail, path ? path : "");
    audit_enqueue(&e);
}

void audit_write_kill(int32_t sender_pid, int32_t target_pid, int32_t signal) {
    audit_entry_t e;
    fill_base(&e, AUDIT_KILL, sender_pid, AUDIT_SRC_NATIVE);
    e.object_idx = (uint32_t)target_pid;
    char buf[40];
    ksnprintf(buf, sizeof(buf), "sig=%d", (int)signal);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_fs_write_critical(int32_t pid, const char *path) {
    audit_entry_t e;
    fill_base(&e, AUDIT_FS_WRITE_CRITICAL, pid, AUDIT_SRC_NATIVE);
    copy_detail(e.detail, path ? path : "");
    audit_enqueue(&e);
}

void audit_write_net_bind(int32_t pid, const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_NET_BIND, pid, AUDIT_SRC_NATIVE);
    copy_detail(e.detail, detail);
    audit_enqueue(&e);
}

void audit_write_ai_invoke(int32_t pid, const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_AI_INVOKE, pid, AUDIT_SRC_NATIVE);
    copy_detail(e.detail, detail);
    audit_enqueue(&e);
}

void audit_write_reboot(int32_t pid) {
    audit_entry_t e;
    fill_base(&e, AUDIT_REBOOT, pid, AUDIT_SRC_NATIVE);
    audit_enqueue(&e);
}

void audit_write_mmio_direct(int32_t pid, uint64_t addr) {
    audit_entry_t e;
    fill_base(&e, AUDIT_MMIO_DIRECT, pid, AUDIT_SRC_NATIVE);
    char buf[40];
    ksnprintf(buf, sizeof(buf), "addr=0x%lx", (unsigned long)addr);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_cap_activate(int32_t caller_pid, uint32_t obj_idx,
                              int32_t result, const char *name, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_ACTIVATE, caller_pid, src);
    e.object_idx  = obj_idx;
    e.result_code = result;
    copy_detail(e.detail, name ? name : "");
    audit_enqueue(&e);
}

void audit_write_cap_deactivate(int32_t caller_pid, uint32_t obj_idx,
                                int32_t result, const char *name, uint8_t src) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CAP_DEACTIVATE, caller_pid, src);
    e.object_idx  = obj_idx;
    e.result_code = result;
    copy_detail(e.detail, name ? name : "");
    audit_enqueue(&e);
}

void audit_write_deprecated_syscall(int32_t caller_pid, const char *syscall_name) {
    audit_entry_t e;
    fill_base(&e, AUDIT_DEPRECATED_SYSCALL, caller_pid, AUDIT_SRC_SHIM);
    copy_detail(e.detail, syscall_name ? syscall_name : "unknown");
    audit_enqueue(&e);
}

// -------------- Phase 17 writers --------------

void audit_write_chan_send(int32_t caller_pid, uint32_t obj_idx,
                           int32_t result, const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CHAN_SEND, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = result;
    copy_detail(e.detail, detail ? detail : "chan_send");
    audit_enqueue(&e);
}

void audit_write_chan_recv(int32_t caller_pid, uint32_t obj_idx,
                           int32_t result, const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CHAN_RECV, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = result;
    copy_detail(e.detail, detail ? detail : "chan_recv");
    audit_enqueue(&e);
}

void audit_write_chan_type_mismatch(int32_t caller_pid, uint32_t obj_idx,
                                    uint64_t expected_hash, uint64_t got_hash) {
    audit_entry_t e;
    fill_base(&e, AUDIT_CHAN_TYPE_MISMATCH, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = CAP_V2_EPROTOTYPE;
    // Detail: "expected=HHHHHHHHHHHHHHHH got=HHHHHHHHHHHHHHHH"
    char buf[128];
    const char *hex = "0123456789abcdef";
    int p = 0;
    const char *lead = "expected=";
    while (lead[p]) { buf[p] = lead[p]; p++; }
    for (int i = 60; i >= 0; i -= 4) buf[p++] = hex[(expected_hash >> i) & 0xF];
    const char *mid = " got=";
    for (int i = 0; mid[i]; i++) buf[p++] = mid[i];
    for (int i = 60; i >= 0; i -= 4) buf[p++] = hex[(got_hash >> i) & 0xF];
    buf[p] = 0;
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_vmo_fault(int32_t caller_pid, uint32_t obj_idx,
                           uint64_t fault_va, int32_t result,
                           const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_VMO_FAULT, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = result;
    // Prepend the faulting VA in hex to the detail for triage.
    char buf[192];
    const char *hex = "0123456789abcdef";
    int p = 0;
    const char *lead = "va=0x";
    while (lead[p]) { buf[p] = lead[p]; p++; }
    for (int i = 60; i >= 0; i -= 4) buf[p++] = hex[(fault_va >> i) & 0xF];
    buf[p++] = ' ';
    const char *d = detail ? detail : "";
    while (*d && p < 190) buf[p++] = *d++;
    buf[p] = 0;
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

// -------------- Phase 18 writers --------------

void audit_write_stream_op_rejected(int32_t caller_pid, uint32_t obj_idx,
                                    uint16_t op, int32_t result,
                                    const char *detail) {
    audit_entry_t e;
    fill_base(&e, AUDIT_STREAM_OP_REJECTED, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = result;
    char buf[96];
    ksnprintf(buf, sizeof(buf), "op=%u %s",
              (unsigned)op, detail ? detail : "");
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_stream_destroy_canceled(int32_t caller_pid, uint32_t obj_idx,
                                         uint32_t ncancelled) {
    audit_entry_t e;
    fill_base(&e, AUDIT_STREAM_DESTROY_CANCELED, caller_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = 0;
    char buf[64];
    ksnprintf(buf, sizeof(buf), "cancelled=%u", (unsigned)ncancelled);
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

void audit_write_handle_transfer(int32_t sender_pid, int32_t receiver_pid,
                                 uint32_t obj_idx, uint8_t nhandles) {
    audit_entry_t e;
    fill_base(&e, AUDIT_HANDLE_TRANSFER, sender_pid, AUDIT_SRC_NATIVE);
    e.object_idx  = obj_idx;
    e.result_code = 0;
    char buf[96];
    const char *lead = "to=pid";
    int p = 0; while (lead[p]) { buf[p] = lead[p]; p++; }
    // Decimal receiver_pid, then " n=N".
    int rp = receiver_pid;
    char digits[12]; int n = 0;
    if (rp < 0) { buf[p++] = '-'; rp = -rp; }
    if (rp == 0) digits[n++] = '0';
    while (rp > 0) { digits[n++] = (char)('0' + (rp % 10)); rp /= 10; }
    while (n > 0) buf[p++] = digits[--n];
    const char *mid = " n=";
    for (int i = 0; mid[i]; i++) buf[p++] = mid[i];
    unsigned nh = nhandles;
    if (nh == 0) buf[p++] = '0';
    else { n = 0; while (nh > 0) { digits[n++] = (char)('0' + (nh % 10)); nh /= 10; }
           while (n > 0) buf[p++] = digits[--n]; }
    buf[p] = 0;
    copy_detail(e.detail, buf);
    audit_enqueue(&e);
}

// ---------------------------------------------------------------------------
// Query. Walks the in-memory ring from oldest still-resident entry to newest,
// filtering by time window and event_mask. Reads from kernel memory only —
// the caller (syscall dispatcher) is responsible for user-buffer copy.
// ---------------------------------------------------------------------------
int audit_query(uint64_t since_ns, uint64_t until_ns, uint32_t event_mask,
                audit_entry_t *out_buf, uint32_t max) {
    if (!out_buf || max == 0) return 0;
    audit_queue_t *q = &g_audit_state.queue;
    int written = 0;

    spinlock_acquire(&q->lock);
    uint64_t total = q->total_written;
    uint64_t start = (total > AUDIT_QUEUE_SIZE) ? (total - AUDIT_QUEUE_SIZE) : 0;
    for (uint64_t i = start; i < total && (uint32_t)written < max; i++) {
        audit_entry_t *e = &q->entries[i % AUDIT_QUEUE_SIZE];
        if (e->magic != AUDIT_ENTRY_MAGIC) continue;
        if (e->ns_timestamp < since_ns) continue;
        if (until_ns != 0 && e->ns_timestamp >= until_ns) continue;
        if (event_mask != 0) {
            if (e->event_type > 31) continue;
            if ((event_mask & (1u << e->event_type)) == 0) continue;
        }
        out_buf[written++] = *e;
    }
    spinlock_release(&q->lock);
    return written;
}
