// kernel/audit.h
//
// Phase 15b — Persistent audit log.
//
// Parallel to klog. Where klog is a lossy in-memory ring for live debugging,
// the audit log is a persistent, rotating, on-disk record of every sensitive
// operation: capability register/derive/revoke/grant/violation, pledge
// narrowing, process spawn/kill, critical file writes, network binds, AI
// invocations. Entries live in /var/audit/YYYY-MM-DD.log, one 256-byte record
// each, time-ordered. Phase 15b's /bin/auditq and SYS_AUDIT_QUERY expose the
// log to operators and AI agents.
//
// Write path: audit_write* enqueues a 256-byte record on an in-memory ring
// (64 KiB, 256 slots). A dedicated kernel flusher thread drains the ring,
// batches 32 entries per disk write, fsyncs after each batch, rotates the
// file at UTC-day boundaries. Producers block only when the ring is > 94%
// full (240/256 entries). Queue lock is held only for enqueue/dequeue — never
// across I/O.
//
// Read path: SYS_AUDIT_QUERY → audit_query → opens the appropriate day's
// file(s), seeks to the first record with ns_timestamp >= since_ns, streams
// matching records into the user buffer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync/spinlock.h"

// ---------------------------------------------------------------------------
// Magic numbers.
// ---------------------------------------------------------------------------
// Per-record magic: ASCII 'A','U','D','1' in little-endian — matches spec
// mnemonic "AUD17ENT" (schema version 1).
#define AUDIT_ENTRY_MAGIC  0x31445541u
// File header magic: fixed per spec.
#define AUDIT_FILE_MAGIC   0xA001D17Eu

#define AUDIT_SCHEMA_VERSION 1

// ---------------------------------------------------------------------------
// Event types. Order locked for external tool compatibility.
// ---------------------------------------------------------------------------
#define AUDIT_CAP_REGISTER        1
#define AUDIT_CAP_UNREGISTER      2
#define AUDIT_CAP_DERIVE          3
#define AUDIT_CAP_REVOKE          4
#define AUDIT_CAP_GRANT           5
#define AUDIT_CAP_VIOLATION       6
#define AUDIT_PLEDGE_NARROW       7
#define AUDIT_SPAWN               8
#define AUDIT_KILL                9
#define AUDIT_FS_WRITE_CRITICAL  10
#define AUDIT_MMIO_DIRECT        11
#define AUDIT_REBOOT             12
#define AUDIT_NET_BIND           13
#define AUDIT_AI_INVOKE          14
#define AUDIT_CAP_ACTIVATE       15
#define AUDIT_CAP_DEACTIVATE     16
#define AUDIT_DEPRECATED_SYSCALL 17
// Phase 17: channel + VMO events.
#define AUDIT_CHAN_SEND              18  // sensitive_only (rights/pledge fail)
#define AUDIT_CHAN_RECV              19  // sensitive_only
#define AUDIT_CHAN_TYPE_MISMATCH     20  // always logged
#define AUDIT_VMO_FAULT              21  // always logged
#define AUDIT_HANDLE_TRANSFER        22  // always logged
// Phase 18: stream events.
#define AUDIT_STREAM_OP_REJECTED     23  // SQE op not in manifest or pledge-denied
#define AUDIT_STREAM_DESTROY_CANCELED 24 // stream_destroy cancelled outstanding jobs
#define AUDIT_EVENT_MAX          24

// Source of the event.
#define AUDIT_SRC_NATIVE  0   // Native v2 API.
#define AUDIT_SRC_SHIM    1   // Phase 15a legacy shim path.

// ---------------------------------------------------------------------------
// audit_entry_t: 256 bytes, fixed. Every field offset is _Static_assert'd to
// survive future layout drift.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;               //   0..3    AUDIT_ENTRY_MAGIC
    uint16_t schema_version;      //   4..5
    uint16_t event_type;          //   6..7
    uint64_t ns_timestamp;        //   8..15   Nanoseconds since boot.
    int64_t  wall_clock_seconds;  //  16..23   Unix epoch seconds at event time.
    int32_t  subject_pid;         //  24..27   -1 if kernel-originated.
    uint32_t object_idx;          //  28..31   0xFFFFFFFF if not object-scoped.
    int32_t  result_code;         //  32..35   0 on success; negative errno on failure.
    uint32_t _pad0;               //  36..39   Reserved; must be zero.
    uint64_t rights_required;     //  40..47   For CAP_VIOLATION.
    uint64_t rights_held;         //  48..55   For CAP_VIOLATION.
    uint16_t pledge_old;          //  56..57   For PLEDGE_NARROW.
    uint16_t pledge_new;          //  58..59
    uint8_t  audit_source;        //  60       AUDIT_SRC_*.
    uint8_t  reserved[3];         //  61..63
    char     detail[192];         //  64..255  Event-specific freeform text, NUL-terminated.
} audit_entry_t;

_Static_assert(sizeof(audit_entry_t) == 256, "audit_entry_t must be 256 bytes");
_Static_assert(offsetof(audit_entry_t, magic)              ==   0, "magic offset");
_Static_assert(offsetof(audit_entry_t, schema_version)     ==   4, "schema_version offset");
_Static_assert(offsetof(audit_entry_t, event_type)         ==   6, "event_type offset");
_Static_assert(offsetof(audit_entry_t, ns_timestamp)       ==   8, "ns_timestamp offset");
_Static_assert(offsetof(audit_entry_t, wall_clock_seconds) ==  16, "wall_clock_seconds offset");
_Static_assert(offsetof(audit_entry_t, subject_pid)        ==  24, "subject_pid offset");
_Static_assert(offsetof(audit_entry_t, object_idx)         ==  28, "object_idx offset");
_Static_assert(offsetof(audit_entry_t, result_code)        ==  32, "result_code offset");
_Static_assert(offsetof(audit_entry_t, rights_required)    ==  40, "rights_required offset");
_Static_assert(offsetof(audit_entry_t, rights_held)        ==  48, "rights_held offset");
_Static_assert(offsetof(audit_entry_t, pledge_old)         ==  56, "pledge_old offset");
_Static_assert(offsetof(audit_entry_t, pledge_new)         ==  58, "pledge_new offset");
_Static_assert(offsetof(audit_entry_t, audit_source)       ==  60, "audit_source offset");
_Static_assert(offsetof(audit_entry_t, detail)             ==  64, "detail offset");

// ---------------------------------------------------------------------------
// audit_queue_t: circular ring. Doubles as the in-memory history ring that
// SYS_AUDIT_QUERY walks — the flusher has a separate cursor so disk writes
// never shrink the queryable window.
//
// Out-of-spec: size bumped 256 → 1024 entries (256 KiB). Spec fixed 256
// entries (64 KiB) but the GrahaFS Phase 7b file-size ceiling is 48 KiB
// (~180 records per daily file). A larger in-memory ring means
// SYS_AUDIT_QUERY can serve recent history when the disk file is capped.
// ---------------------------------------------------------------------------
#define AUDIT_QUEUE_SIZE          1024u
#define AUDIT_FLUSH_BATCH         32u    // Max entries the flusher drains per iteration.

typedef struct {
    audit_entry_t entries[AUDIT_QUEUE_SIZE];
    // total_written: strictly monotonic insertion counter. Next insert slot =
    // total_written % AUDIT_QUEUE_SIZE. Never wraps (uint64_t takes ~585
    // years at 1 Gentry/s).
    uint64_t total_written;
    // disk_cursor: next entry the flusher will attempt to persist. Advances
    // on both successful disk write and on failure (drop). When
    // total_written - disk_cursor > AUDIT_QUEUE_SIZE, the flusher fell so
    // far behind that entries were overwritten before it got to them and
    // `overwritten` is bumped.
    uint64_t disk_cursor;
    uint64_t overwritten;  // Entries lost because flusher fell behind; separate from dropped.
    uint64_t dropped;      // Entries the flusher tried to write but couldn't (disk full, I/O error).
    spinlock_t lock;
} audit_queue_t;

// ---------------------------------------------------------------------------
// audit_state_t: singleton. One per kernel.
// ---------------------------------------------------------------------------
#define AUDIT_FILE_HEADER_SIZE 32u

typedef struct {
    audit_queue_t queue;          // In-memory pending ring.
    int32_t       flusher_task_id;  // PID of the audit flusher kernel thread, -1 until started.
    int32_t       current_fd;     // Currently unused: Phase 15b uses a vfs_node_t pointer directly; reserved for future reuse.
    void         *current_file;   // Opaque vfs_node_t * for the current day's log (set by attach_fs).
    int64_t       current_day_seconds;  // day_seconds of the currently-open file; rotation when now/86400 > this.
    uint64_t      current_file_size;    // Byte offset into the current file; next append happens here.
    uint32_t      current_entry_count;  // Matches the in-file header's entry_count after flush.
    uint64_t      next_seq;       // Strictly monotonic seq; prepended as "seq=<N>" to detail.
    bool          fs_attached;    // True if audit_attach_fs succeeded; false = klog-only fallback.
} audit_state_t;

extern audit_state_t g_audit_state;

// ---------------------------------------------------------------------------
// File format. On-disk: 32-byte header + N * 256-byte audit_entry_t records.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t file_magic;        // 0..3     AUDIT_FILE_MAGIC
    uint16_t schema_version;    // 4..5
    uint16_t _pad0;             // 6..7
    int64_t  day_seconds;       // 8..15    Unix-seconds timestamp of the day this file represents.
    uint32_t entry_count;       // 16..19   Updated after each flushed batch.
    uint8_t  reserved[12];      // 20..31
} audit_file_header_t;

_Static_assert(sizeof(audit_file_header_t) == AUDIT_FILE_HEADER_SIZE,
               "audit_file_header_t must be 32 bytes");

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Prepare the in-memory audit subsystem: zero the queue, init the lock,
// mark fs_attached=false. Called from main.c between cap_object_init() and
// the first cap_register(). Safe to call with interrupts disabled.
void audit_init(void);

// Open /var/audit/YYYY-MM-DD.log (creating the file and /var/audit/ if
// missing), write the file header, drain any pending entries that were
// queued before FS was ready. Called from main.c after grahafs_mount. On
// failure (disk full, corruption, /var missing), logs a klog ERROR and leaves
// audit in klog-only fallback mode (fs_attached = false).
void audit_attach_fs(void);

// Kernel-thread entry for the audit flusher. Loops forever:
//   1. Sleep ~10 ms.
//   2. Take queue lock, snapshot up to AUDIT_FLUSH_BATCH entries, release.
//   3. Rotate file if day advanced.
//   4. Append the batch via grahafs_write, update header, release.
//   5. On I/O error: retry once; on second failure, increment dropped.
// Never returns. Called via sched_create_task(audit_flusher_task_entry) after
// sched_init in main.c.
void audit_flusher_task_entry(void);

// ---------------------------------------------------------------------------
// Convenience writers. All of these fan out to an internal enqueue; callers
// never touch audit_entry_t directly. Safe to call from any context
// (IRQ-safe — the queue lock disables preemption while held).
// ---------------------------------------------------------------------------
void audit_write_cap_register(uint32_t obj_idx, const char *name, uint8_t src);
void audit_write_cap_unregister(uint32_t obj_idx, const char *name, uint8_t src);
void audit_write_cap_derive(int32_t caller_pid, uint32_t parent_idx,
                            uint32_t new_idx, uint64_t rights, uint8_t src);
void audit_write_cap_revoke(int32_t caller_pid, uint32_t obj_idx,
                            int32_t result, uint8_t src);
void audit_write_cap_grant(int32_t caller_pid, int32_t target_pid,
                           uint32_t obj_idx, uint8_t src);

// CAP_VIOLATION: a protected operation was refused. rights_required/held
// describe what was needed vs actually held; detail adds human-readable
// context. src=NATIVE for v2 path violations, SHIM for legacy path.
void audit_write_cap_violation(int32_t caller_pid, uint32_t obj_idx,
                               int32_t result,
                               uint64_t rights_required,
                               uint64_t rights_held,
                               const char *detail, uint8_t src);

void audit_write_pledge_narrow(int32_t pid, uint16_t old_mask, uint16_t new_mask);
void audit_write_pledge_violation(int32_t pid, uint16_t old_mask,
                                  uint16_t new_mask, const char *detail);

void audit_write_spawn(int32_t parent_pid, int32_t child_pid, const char *path);
void audit_write_kill(int32_t sender_pid, int32_t target_pid, int32_t signal);
void audit_write_fs_write_critical(int32_t pid, const char *path);
void audit_write_net_bind(int32_t pid, const char *detail);
void audit_write_ai_invoke(int32_t pid, const char *detail);
void audit_write_reboot(int32_t pid);
void audit_write_mmio_direct(int32_t pid, uint64_t addr);

// Phase 16 writers. CAP_ACTIVATE / CAP_DEACTIVATE fire for every successful
// state flip; cascades produce one entry per cap touched. object_idx is the
// cap_object_idx paired with the CAN entry; detail carries the cap name.
// DEPRECATED_SYSCALL fires at most once per (pid, syscall_num) pair — see
// kernel/cap/deprecated.h for the suppression logic.
void audit_write_cap_activate(int32_t caller_pid, uint32_t obj_idx,
                              int32_t result, const char *name, uint8_t src);
void audit_write_cap_deactivate(int32_t caller_pid, uint32_t obj_idx,
                                int32_t result, const char *name, uint8_t src);
void audit_write_deprecated_syscall(int32_t caller_pid, const char *syscall_name);

// Phase 17 writers.
// CHAN_SEND / CHAN_RECV fire only on permission or pledge denial (the happy
// path is too hot to audit). object_idx is the CAP_KIND_CHANNEL endpoint.
void audit_write_chan_send(int32_t caller_pid, uint32_t obj_idx,
                           int32_t result, const char *detail);
void audit_write_chan_recv(int32_t caller_pid, uint32_t obj_idx,
                           int32_t result, const char *detail);
// TYPE_MISMATCH fires on every rejected send. Carries expected vs actual hash.
void audit_write_chan_type_mismatch(int32_t caller_pid, uint32_t obj_idx,
                                    uint64_t expected_hash, uint64_t got_hash);
// VMO_FAULT fires when vmo_cow_fault cannot satisfy a page (e.g., -ENOMEM)
// or when a rights-denied page fault occurs on a VMO-backed mapping.
void audit_write_vmo_fault(int32_t caller_pid, uint32_t obj_idx,
                           uint64_t fault_va, int32_t result,
                           const char *detail);
// HANDLE_TRANSFER fires on every successful chan_send that carried handles.
// nhandles = number moved; detail describes the first handle's kind.
void audit_write_handle_transfer(int32_t sender_pid, int32_t receiver_pid,
                                 uint32_t obj_idx, uint8_t nhandles);

// Phase 18 writers.
// STREAM_OP_REJECTED fires when submit_batch rejects an SQE for unknown op
// or pledge denial. obj_idx is the stream cap_object index; op is the raw
// opcode the SQE carried; detail carries the op name (if known) and reason.
void audit_write_stream_op_rejected(int32_t caller_pid, uint32_t obj_idx,
                                    uint16_t op, int32_t result,
                                    const char *detail);
// STREAM_DESTROY_CANCELED fires when stream_destroy cancels outstanding jobs.
// ncancelled reports how many jobs received a CQE with CQE_FLAG_CANCELED.
void audit_write_stream_destroy_canceled(int32_t caller_pid, uint32_t obj_idx,
                                         uint32_t ncancelled);

// ---------------------------------------------------------------------------
// Query path. Backs SYS_AUDIT_QUERY.
// ---------------------------------------------------------------------------
// Streams matching entries into out_buf (kernel memory; syscall dispatcher
// copies to user after validation). Returns number of entries written, or
// a negative CAP_V2_* errno on failure. event_mask = 0 means all events.
int audit_query(uint64_t since_ns, uint64_t until_ns, uint32_t event_mask,
                audit_entry_t *out_buf, uint32_t max);
