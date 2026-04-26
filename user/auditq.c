// user/auditq.c — Phase 15b: /bin/auditq audit-log reader.
//
// Reads the in-memory audit ring via SYS_AUDIT_QUERY and prints entries
// in a human-readable columnar format. For Phase 15b, argv isn't plumbed
// through the spawn path — Phase 16's channel-based spawn will add it.
// Until then /bin/auditq always dumps up to AUDITQ_BATCH recent entries.
//
// Output format (one entry per line):
//   [ns=<n>] <EVENT_NAME> pid=<p> obj=0x<hex> rc=<code> [src=SHIM]
//           [old=0x<hex> new=0x<hex>] [req=0x<hex> held=0x<hex>]
//           [detail="<text>"]

#include <stdint.h>
#include "syscalls.h"
#include "../libc/include/stdio.h"
#include "../libc/include/string.h"
#include "../libc/include/stdlib.h"

#define AUDITQ_BATCH 64

static const char *event_name(unsigned ev) {
    switch (ev) {
    case AUDIT_CAP_REGISTER:       return "CAP_REGISTER";
    case AUDIT_CAP_UNREGISTER:     return "CAP_UNREGISTER";
    case AUDIT_CAP_DERIVE:         return "CAP_DERIVE";
    case AUDIT_CAP_REVOKE:         return "CAP_REVOKE";
    case AUDIT_CAP_GRANT:          return "CAP_GRANT";
    case AUDIT_CAP_VIOLATION:      return "CAP_VIOLATION";
    case AUDIT_PLEDGE_NARROW:      return "PLEDGE_NARROW";
    case AUDIT_SPAWN:              return "SPAWN";
    case AUDIT_KILL:               return "KILL";
    case AUDIT_FS_WRITE_CRITICAL:  return "FS_WRITE_CRITICAL";
    case AUDIT_MMIO_DIRECT:        return "MMIO_DIRECT";
    case AUDIT_REBOOT:             return "REBOOT";
    case AUDIT_NET_BIND:           return "NET_BIND";
    case AUDIT_AI_INVOKE:          return "AI_INVOKE";
    case AUDIT_CAP_ACTIVATE:           return "CAP_ACTIVATE";
    case AUDIT_CAP_DEACTIVATE:         return "CAP_DEACTIVATE";
    case AUDIT_DEPRECATED_SYSCALL:     return "DEPRECATED_SYSCALL";
    case AUDIT_CHAN_SEND:              return "CHAN_SEND";
    case AUDIT_CHAN_RECV:              return "CHAN_RECV";
    case AUDIT_CHAN_TYPE_MISMATCH:     return "CHAN_TYPE_MISMATCH";
    case AUDIT_VMO_FAULT:              return "VMO_FAULT";
    case AUDIT_HANDLE_TRANSFER:        return "HANDLE_TRANSFER";
    case AUDIT_STREAM_OP_REJECTED:     return "STREAM_OP_REJECTED";
    case AUDIT_STREAM_DESTROY_CANCELED: return "STREAM_DESTROY_CANCELED";
    case AUDIT_FS_JOURNAL_REPLAY:      return "FS_JOURNAL_REPLAY";
    case AUDIT_FS_REVERT:              return "FS_REVERT";
    case AUDIT_FS_GC_NOW:              return "FS_GC_NOW";
    case AUDIT_FS_SNAPSHOT:            return "FS_SNAPSHOT";
    case AUDIT_RLIMIT_MEM:             return "RLIMIT_MEM";
    case AUDIT_RLIMIT_CPU:             return "RLIMIT_CPU";
    case AUDIT_RLIMIT_IO:              return "RLIMIT_IO";
    case AUDIT_SCHED_EPOCH:            return "SCHED_EPOCH";
    case AUDIT_SCHED_STARVATION:       return "SCHED_STARVATION";
    case AUDIT_SCHED_SPINLOCK_PANIC:   return "SCHED_SPINLOCK_PANIC";
    case AUDIT_DRV_REGISTERED:         return "DRV_REGISTERED";
    case AUDIT_DRV_DIED:               return "DRV_DIED";
    case AUDIT_MMIO_DENIED:            return "MMIO_DENIED";
    case AUDIT_IRQ_DROPPED:            return "IRQ_DROPPED";
    default:                        return "UNKNOWN";
    }
}

static void print_entry(const audit_entry_u_t *e) {
    printf("[ns=%lu] %-18s pid=%-4d obj=0x%08x rc=%-4d",
           (unsigned long)e->ns_timestamp,
           event_name(e->event_type),
           (int)e->subject_pid,
           (unsigned)e->object_idx,
           (int)e->result_code);
    if (e->audit_source == AUDIT_SRC_SHIM) printf(" src=SHIM");
    if (e->event_type == AUDIT_PLEDGE_NARROW) {
        printf(" old=0x%x new=0x%x",
               (unsigned)e->pledge_old, (unsigned)e->pledge_new);
    }
    if (e->event_type == AUDIT_CAP_VIOLATION &&
        (e->rights_required | e->rights_held)) {
        printf(" req=0x%lx held=0x%lx",
               (unsigned long)e->rights_required,
               (unsigned long)e->rights_held);
    }
    if (e->detail[0]) printf(" detail=\"%.160s\"", e->detail);
    printf("\n");
}

// Static batch buffer so auditq doesn't depend on malloc for its one big
// allocation. 16 KiB.
static audit_entry_u_t g_batch[AUDITQ_BATCH];

void _start(void) {
    long got = syscall_audit_query(/*since_ns*/ 0, /*until_ns*/ 0,
                                   /*event_mask*/ 0,
                                   g_batch, AUDITQ_BATCH);
    if (got < 0) {
        printf("auditq: SYS_AUDIT_QUERY failed rc=%ld\n", got);
        exit(1);
    }
    if (got == 0) {
        printf("auditq: no entries\n");
        exit(0);
    }

    for (long i = 0; i < got; i++) {
        print_entry(&g_batch[i]);
    }

    printf("\n-- %ld entries --\n", got);
    exit(0);
}
