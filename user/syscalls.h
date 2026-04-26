// user/syscalls.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../kernel/gcp.h" // For gcp_command_t

// Standard C library types
typedef long ssize_t;

// --- System Call Numbers ---
#define SYS_PUTC        1001
#define SYS_OPEN        1002
#define SYS_READ        1003
#define SYS_CLOSE       1004
#define SYS_GCP_EXECUTE 1005
#define SYS_GETC        1006
#define SYS_EXEC        1007
#define SYS_EXIT        1008
#define SYS_WAIT        1009
#define SYS_WRITE  1010
#define SYS_CREATE 1011
#define SYS_MKDIR  1012
#define SYS_STAT   1013
#define SYS_READDIR 1014
#define SYS_SYNC    1015
#define SYS_SPAWN   1017
#define SYS_KILL    1018
#define SYS_SIGNAL  1019
#define SYS_GETPID  1020
#define SYS_GET_SYSTEM_STATE 1021
#define SYS_CAP_ACTIVATE     1031
#define SYS_CAP_DEACTIVATE   1032
#define SYS_CAP_REGISTER     1033
#define SYS_CAP_UNREGISTER   1034
#define SYS_SET_AI_METADATA  1035
#define SYS_GET_AI_METADATA  1036
#define SYS_SEARCH_BY_TAG    1037
#define SYS_CAP_WATCH        1038
#define SYS_CAP_UNWATCH      1039
#define SYS_CAP_POLL         1040
// 1041..1045 retired in Phase 22 Stage F (-ENOSYS + AUDIT_DEPRECATED_SYSCALL).
// Replaced by libnet/libhttp over /sys/net/service.  IDs and wrappers below
// remain so pledgetest can still exercise NET_SERVER/NET_CLIENT pledge denial
// (the kernel checks pledge before returning -ENOSYS) and so that callers
// who didn't migrate yet get a loud audit entry instead of a link error.
#define SYS_NET_IFCONFIG     1041  // retired — see kernel/audit.c AUDIT_DEPRECATED_SYSCALL
#define SYS_NET_STATUS       1042  // retired
#define SYS_HTTP_GET         1043  // retired — use libhttp http_get
#define SYS_DNS_RESOLVE      1044  // retired — use libnet libnet_dns_resolve
#define SYS_HTTP_POST        1045  // retired — use libhttp http_post
#define SYS_PIPE             1046
#define SYS_DUP2             1047
#define SYS_DUP              1048
#define SYS_TRUNCATE         1049
#define SYS_COMPUTE_SIMHASH  1050
#define SYS_FIND_SIMILAR     1051
#define SYS_CLUSTER_LIST     1052
#define SYS_CLUSTER_MEMBERS  1053
#define SYS_KLOG_READ        1054
#define SYS_KLOG_WRITE       1055
#define SYS_DEBUG            1056
#define SYS_KHEAP_STATS      1057
#define SYS_CAP_DERIVE       1058
#define SYS_CAP_REVOKE_V2    1059
#define SYS_CAP_GRANT        1060
#define SYS_CAP_INSPECT      1061
// Phase 15b
#define SYS_PLEDGE           1062
#define SYS_AUDIT_QUERY      1063
// Phase 16: token-taking CAN activate/deactivate + name→token lookup.
#define SYS_CAN_ACTIVATE_T    1064
#define SYS_CAN_DEACTIVATE_T  1065
#define SYS_CAN_LOOKUP        1066
// Phase 17: Channels + VMOs.
#define SYS_CHAN_CREATE   1067
#define SYS_CHAN_SEND     1068
#define SYS_CHAN_RECV     1069
#define SYS_CHAN_POLL     1070
#define SYS_VMO_CREATE    1071
#define SYS_VMO_MAP       1072
#define SYS_VMO_UNMAP     1073
#define SYS_VMO_CLONE     1074

// Phase 18: Submission Streams.
#define SYS_STREAM_CREATE  1075
#define SYS_STREAM_SUBMIT  1076
#define SYS_STREAM_REAP    1077
#define SYS_STREAM_DESTROY 1078
// Phase 19: GrahaFS v2
#define SYS_FS_SNAPSHOT       1079
#define SYS_FS_LIST_VERSIONS  1080
#define SYS_FS_REVERT         1081
#define SYS_FS_GC_NOW         1082
#define SYS_FSYNC             1083

// Phase 20: resource limits (shifted from spec 1081-1082; Phase 19 owns those).
#define SYS_SETRLIMIT         1084
#define SYS_GETRLIMIT         1085
// Phase 20 U15: spawn-with-rlimit-overrides. Path + attrs*. attrs may be
// NULL (equivalent to SYS_SPAWN). When attrs.flags has SPAWN_ATTR_HAS_RLIMIT
// set, the caller MUST hold PLEDGE_SYS_CONTROL; the child's mem/cpu/io
// limits are overwritten before it runs.
#define SYS_SPAWN_EX          1086

// Phase 21: userspace driver framework + E1000 migration.
#define SYS_DRV_REGISTER      1087
#define SYS_DRV_IRQ_WAIT      1088
#define SYS_MMIO_VMO_CREATE   1089
#define SYS_DEBUG_INJECT_PCI  1090
// Phase 22: named channel registry. Replaces implicit-lookup assumptions in
// the spec with two explicit syscalls routed through kernel/net/rawnet.c.
#define SYS_CHAN_PUBLISH      1091
#define SYS_CHAN_CONNECT      1092

// SYS_MMIO_VMO_CREATE op codes (in RDI).
#define MMIO_VMO_OP_CREATE      0u
#define MMIO_VMO_OP_PHYS_QUERY  1u

// drv_caps_t — populated by sys_drv_register, returned via R10 ptr.
// Layout MUST match kernel/driver/userdrv.h struct drv_caps.
// Phase 21.1: added upstream_handle (daemon's READ end of the proxy→daemon
// control channel). Struct grew 56 → 64 bytes.
typedef struct {
    uint64_t mmio_handle;             // cap_token raw, CAP_KIND_MMIO_REGION
    uint64_t irq_channel_handle;      // cap_token raw, CAP_KIND_IRQ_CHANNEL
    uint64_t downstream_handle;       // cap_token raw, CAP_KIND_CHANNEL (daemon's WRITE end)
    uint64_t upstream_handle;         // Phase 21.1: cap_token raw, CAP_KIND_CHANNEL (daemon's READ end)
    uint64_t bar_phys;                // physical base of mapped BAR
    uint64_t bar_size;                // size of BAR in bytes
    uint32_t pci_addr;                // bus<<16 | dev<<8 | func
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq_vector;              // IDT vector assigned
    uint8_t  _pad[7];
} drv_caps_t;

// drv_irq_msg_t — 16 bytes; produced by ISR, consumed by sys_drv_irq_wait.
// Layout MUST match kernel/driver/userdrv.h struct drv_irq_msg.
typedef struct {
    uint8_t  vector;
    uint8_t  _pad0[7];
    uint64_t timestamp_tsc;
} drv_irq_msg_t;

#define SPAWN_ATTR_HAS_RLIMIT_U  (1u << 0)

// Userspace mirror of the kernel's spawn_attrs_t subset — same layout as the
// kernel tail fields so the syscall can copy straight across. Fields not set
// by the caller MUST be zeroed (the kernel reads trailing fields conditionally
// on flag bits; stale pointer-like slots could be misinterpreted).
typedef struct {
    uint32_t flags;           // SPAWN_ATTR_HAS_RLIMIT_U, etc.
    uint32_t _pad;
    uint64_t rlimit_mem_pages;
    uint64_t rlimit_cpu_ns;
    uint64_t rlimit_io_bps;
} spawn_rlimits_t;

// Resource identifiers for SYS_SETRLIMIT / SYS_GETRLIMIT. Value = 0 means "no
// limit" for every resource.
#define RLIMIT_MEM            1   // pages of 4 KiB each
#define RLIMIT_CPU            2   // ns per 1-second epoch (max 1_000_000_000)
#define RLIMIT_IO             3   // bytes per second through stream submit

// Phase 17 constants (mirror kernel headers).
#define CHAN_MODE_BLOCKING     1u
#define CHAN_MODE_NONBLOCKING  2u
#define CHAN_ENDPOINT_READ     1u
#define CHAN_ENDPOINT_WRITE    2u
#define CHAN_MSG_INLINE_MAX    256u
#define CHAN_MSG_HANDLES_MAX   8u

#define PROT_READ   0x1u
#define PROT_WRITE  0x2u
#define PROT_EXEC   0x4u

#define VMO_ZEROED       0x1u
#define VMO_ONDEMAND     0x2u
#define VMO_PINNED       0x4u
#define VMO_CLONE_COW    0x10u
// Phase 22 Stage B: shared clone. Parent + child map the same physical
// pages with RW; no copy-on-write machinery. Used for DMA ring handoff
// between e1000d (producer) and netd (consumer).
#define VMO_CLONE_SHARED 0x20u

// Phase 17 userspace errno mirrors.
#define EPROTOTYPE_U  71
#define EFAULT_U       4
#define EAGAIN_U      11
#define ETIMEDOUT_U  110
#define EPIPE_U       32
#define EBADF_U        9
#define DEBUG_PERCPU_WRITE   44
#define DEBUG_PERCPU_READ    45
#define DEBUG_KMALLOC        46
#define DEBUG_KFREE          47
#define DEBUG_CAP_LOOKUP     48
// Phase 15b test-assist hooks.
#define DEBUG_READ_PLEDGE    50
// Phase 16 test-assist hooks (mirror of kernel/syscall.h).
#define DEBUG_PIC_READ_MASK    60
#define DEBUG_FB_READ_PIXEL    61
#define DEBUG_AHCI_PORT_CMD    62
#define DEBUG_E1000_READ_REG   63
#define DEBUG_KB_IS_ACTIVE     64
#define DEBUG_FB_IS_ACTIVE     65
#define DEBUG_E1000_IS_ACTIVE  66
#define DEBUG_AHCI_IS_ACTIVE   67
#define DEBUG_SET_WALL       51

// Phase 15a: Capability Objects v2 constants (mirror kernel/cap/token.h).
// Kinds
#define CAP_KIND_NONE    0
#define CAP_KIND_CAN     1
#define CAP_KIND_FILE    2
#define CAP_KIND_PROC    3
#define CAP_KIND_CHANNEL 4
#define CAP_KIND_VMO     5
// Phase 18-19
#define CAP_KIND_SNAPSHOT       6
#define CAP_KIND_WASM_INSTANCE  7
#define CAP_KIND_STREAM         8
#define CAP_KIND_FS_SNAPSHOT    9
// Phase 21
#define CAP_KIND_DRIVER_REGISTRAR  10
#define CAP_KIND_IRQ_CHANNEL       11
#define CAP_KIND_MMIO_REGION       12
// Rights
#define RIGHT_READ       0x0000000000000001ULL
#define RIGHT_WRITE      0x0000000000000002ULL
#define RIGHT_EXEC       0x0000000000000004ULL
#define RIGHT_DERIVE     0x0000000000000008ULL
#define RIGHT_REVOKE     0x0000000000000010ULL
#define RIGHT_INSPECT    0x0000000000000020ULL
#define RIGHT_ACTIVATE   0x0000000000000040ULL
#define RIGHT_DEACTIVATE 0x0000000000000080ULL
#define RIGHT_GRANT      0x0000000000000100ULL
#define RIGHTS_ALL       0xFFFFFFFFFFFFFFFFULL
// Flags
#define CAP_FLAG_PUBLIC           0x01
#define CAP_FLAG_EAGER_REVOKE     0x02
#define CAP_FLAG_SHIM_EPHEMERAL   0x04
#define CAP_FLAG_IMMORTAL         0x08
#define CAP_FLAG_INHERITABLE      0x10
#define CAP_FLAG_CASCADE_TRUNCATED 0x20
// Errors (-CAP_V2_*)
#define CAP_V2_OK         0
#define CAP_V2_EPERM     -1
#define CAP_V2_EREVOKED  -2
#define CAP_V2_ENOMEM    -3
#define CAP_V2_EFAULT    -4
#define CAP_V2_EINVAL    -5
#define CAP_V2_ENOSYS    -6
#define CAP_V2_EPLEDGE   -7   // Phase 15b: pledge class denied the call.
#define EPLEDGE           7   // Phase 15b userspace alias.
#define CAP_V2_EDEPRECATED -78 // Phase 16: legacy CAN syscall invoked.
#define EDEPRECATED      78   // Phase 16 userspace alias.
// Sentinels
#define PID_PUBLIC       0xFFFF
#define PID_NONE         (-1)

// ---------------------------------------------------------------------------
// Phase 15b: pledge mask + audit entry constants (mirror kernel/cap/pledge.h
// and kernel/audit.h).
// ---------------------------------------------------------------------------

// Pledge class bit positions (0..11; bits 12..15 reserved).
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
// Phase 21
#define PLEDGE_CLASS_STORAGE_SERVER  12
#define PLEDGE_CLASS_INPUT_SERVER    13
#define PLEDGE_STORAGE_SERVER  (1u << PLEDGE_CLASS_STORAGE_SERVER)
#define PLEDGE_INPUT_SERVER    (1u << PLEDGE_CLASS_INPUT_SERVER)

#define PLEDGE_ALL  0x3FFFu
#define PLEDGE_NONE 0x0000u

// Audit event types. MUST stay in sync with kernel/audit.h — drift here causes
// auditq to misname events. Kept up to AUDIT_EVENT_MAX through Phase 20.
#define AUDIT_CAP_REGISTER            1
#define AUDIT_CAP_UNREGISTER          2
#define AUDIT_CAP_DERIVE              3
#define AUDIT_CAP_REVOKE              4
#define AUDIT_CAP_GRANT               5
#define AUDIT_CAP_VIOLATION           6
#define AUDIT_PLEDGE_NARROW           7
#define AUDIT_SPAWN                   8
#define AUDIT_KILL                    9
#define AUDIT_FS_WRITE_CRITICAL      10
#define AUDIT_MMIO_DIRECT            11
#define AUDIT_REBOOT                 12
#define AUDIT_NET_BIND               13
#define AUDIT_AI_INVOKE              14
#define AUDIT_CAP_ACTIVATE           15
#define AUDIT_CAP_DEACTIVATE         16
#define AUDIT_DEPRECATED_SYSCALL     17
#define AUDIT_CHAN_SEND              18
#define AUDIT_CHAN_RECV              19
#define AUDIT_CHAN_TYPE_MISMATCH     20
#define AUDIT_VMO_FAULT              21
#define AUDIT_HANDLE_TRANSFER        22
#define AUDIT_STREAM_OP_REJECTED     23
#define AUDIT_STREAM_DESTROY_CANCELED 24
#define AUDIT_FS_JOURNAL_REPLAY      25
#define AUDIT_FS_REVERT              26
#define AUDIT_FS_GC_NOW              27
#define AUDIT_FS_SNAPSHOT            28
// Phase 20: resource limits + scheduler telemetry.
#define AUDIT_RLIMIT_MEM             29
#define AUDIT_RLIMIT_CPU             30
#define AUDIT_RLIMIT_IO              31
#define AUDIT_SCHED_EPOCH            32
#define AUDIT_SCHED_STARVATION       33
#define AUDIT_SCHED_SPINLOCK_PANIC   34
// Phase 21: driver framework events.
#define AUDIT_DRV_REGISTERED         35
#define AUDIT_DRV_DIED               36
#define AUDIT_MMIO_DENIED            37
#define AUDIT_IRQ_DROPPED            38
#define AUDIT_EVENT_MAX              38

#define AUDIT_SRC_NATIVE  0
#define AUDIT_SRC_SHIM    1

// 256-byte user-visible mirror of kernel audit_entry_t. Field order MUST
// match kernel/audit.h exactly.
typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t event_type;
    uint64_t ns_timestamp;
    int64_t  wall_clock_seconds;
    int32_t  subject_pid;
    uint32_t object_idx;
    int32_t  result_code;
    uint32_t _pad0;
    uint64_t rights_required;
    uint64_t rights_held;
    uint16_t pledge_old;
    uint16_t pledge_new;
    uint8_t  audit_source;
    uint8_t  reserved[3];
    char     detail[192];
} audit_entry_u_t;

// Packed user-side cap_token_t (mirror kernel/cap/token.h layout).
typedef uint64_t cap_token_raw_t;

// Phase 16: a struct wrapper for ergonomic pass-by-value through the syscall
// wrappers below. The field layout mirrors kernel/cap/token.h: 32-bit gen,
// 24-bit idx, 8-bit flags, packed into a single uint64_t.
typedef union {
    cap_token_raw_t raw;
    struct {
        uint8_t  flags;   // low 8 bits
        uint32_t idx : 24;
        uint32_t gen;
    } __attribute__((packed)) fields;
} cap_token_u_t;

// 64-byte inspect-result struct (mirror kernel layout).
typedef struct {
    uint16_t  kind;
    uint8_t   flags;
    uint8_t   reserved1;
    uint32_t  generation;
    uint64_t  rights_bitmap;
    int32_t   audience_pids[8];
    cap_token_raw_t parent_token;
    int32_t   owner_pid;
    uint8_t   reserved2[4];
} cap_inspect_result_u_t;

// Phase 13: klog level/subsys constants (mirror of kernel/log.h).
// Keep these in sync with the kernel-side definitions.
#define KLOG_TRACE 0
#define KLOG_DEBUG 1
#define KLOG_INFO  2
#define KLOG_WARN  3
#define KLOG_ERROR 4
#define KLOG_FATAL 5

// SYS_DEBUG sub-operations (build-gated, test-only).
#define DEBUG_PANIC      1
#define DEBUG_KERNEL_PF  2

// Directory entry structure for user space
typedef struct {
    uint32_t type;
    char name[28];
} user_dirent_t;

// --- Syscall Wrappers (Inline Assembly) ---

static inline void syscall_putc(char c) {
    asm volatile("syscall" : : "a"(SYS_PUTC), "D"(c) : "rcx", "r11", "memory");
}

static inline int syscall_open(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_OPEN), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline ssize_t syscall_read(int fd, void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READ), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_close(int fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CLOSE), "D"(fd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_gcp_execute(gcp_command_t *cmd) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GCP_EXECUTE), "D"(cmd) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline char syscall_getc(void) {
    long ret;
    // This is a blocking call; the kernel won't return until a key is pressed.
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GETC) : "rcx", "r11", "memory");
    return (char)ret;
}

static inline int syscall_exec(const char *pathname) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_EXEC), "D"(pathname) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_exit(int status) {
    asm volatile("syscall" : : "a"(SYS_EXIT), "D"(status) : "rcx", "r11", "memory");
    // Should never return
    while(1);
}

// NEW: wait() - Wait for any child process to exit
// Returns: child PID on success, -1 on error
// If status is non-NULL, the exit status of the child is stored there
static inline int syscall_wait(int *status) {
    long ret;
    
    // Keep retrying if we get the special retry value
    do {
        asm volatile("syscall" : "=a"(ret) : "a"(SYS_WAIT), "D"(status) : "rcx", "r11", "memory");
        
        // If we got -99, it means we were blocked and need to retry
        if (ret == -99) {
            // The kernel has woken us up, retry the syscall
            continue;
        }
        
        break;
    } while (1);
    
    return (int)ret;
}

// Convenience wrapper that ignores exit status
static inline int wait(void) {
    return syscall_wait(NULL);
}

static inline ssize_t syscall_write(int fd, const void *buf, size_t count) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_WRITE), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return (ssize_t)ret;
}

static inline int syscall_create(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_CREATE), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_mkdir(const char *pathname, uint32_t mode) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_MKDIR), "D"(pathname), "S"(mode) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_readdir(const char *pathname, uint32_t index, user_dirent_t *dirent) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_READDIR), "D"(pathname), "S"(index), "d"(dirent) : "rcx", "r11", "memory");
    return (int)ret;
}

static inline void syscall_sync(void) {
    asm volatile("syscall" : : "a"(SYS_SYNC) : "rcx", "r11", "memory");
}

// Phase 21 — userspace driver framework wrappers.
// drv_register: claim a PCI device. Pledge: SYS_CONTROL + class-specific.
static inline long syscall_drv_register(uint16_t vendor_id, uint16_t device_id,
                                        uint8_t device_class,
                                        drv_caps_t *out_caps) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)(uintptr_t)out_caps;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DRV_REGISTER),
          "D"((uint64_t)vendor_id),
          "S"((uint64_t)device_id),
          "d"((uint64_t)device_class),
          "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

// drv_irq_wait: block (or poll if timeout_ms==0) for IRQ messages on a
// CAP_KIND_IRQ_CHANNEL handle. Returns N copied (0..max), 0 on timeout,
// negative on error.
static inline long syscall_drv_irq_wait(uint64_t irq_handle,
                                        drv_irq_msg_t *out_msgs,
                                        uint32_t max_msgs,
                                        uint32_t timeout_ms) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)timeout_ms;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DRV_IRQ_WAIT),
          "D"(irq_handle),
          "S"((uint64_t)(uintptr_t)out_msgs),
          "d"((uint64_t)max_msgs),
          "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

// mmio_vmo_create: SYS_MMIO_VMO_CREATE op=CREATE. Returns cap_token raw or
// negative errno. flags currently unused (CACHE_DISABLE forced).
static inline long syscall_mmio_vmo_create(uint64_t phys, uint64_t size,
                                           uint32_t flags) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)flags;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_MMIO_VMO_CREATE),
          "D"((uint64_t)MMIO_VMO_OP_CREATE),
          "S"(phys),
          "d"(size),
          "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

// vmo_phys_query: SYS_MMIO_VMO_CREATE op=PHYS_QUERY. Writes the physical
// address backing page `page_idx` of vmo handle `vmo` to *phys_out.
static inline long syscall_vmo_phys(uint64_t vmo_handle, uint32_t page_idx,
                                    uint64_t *phys_out) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)(uintptr_t)phys_out;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_MMIO_VMO_CREATE),
          "D"((uint64_t)MMIO_VMO_OP_PHYS_QUERY),
          "S"(vmo_handle),
          "d"((uint64_t)page_idx),
          "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 7d: Spawn a new process (modern replacement for fork+exec)
static inline int syscall_spawn(const char *path) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_SPAWN), "D"(path) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 20 U15: spawn with optional rlimit overrides. attrs may be NULL
// (equivalent to syscall_spawn). If attrs.flags has SPAWN_ATTR_HAS_RLIMIT_U
// the caller must hold PLEDGE_SYS_CONTROL or the call returns -EPLEDGE.
static inline int syscall_spawn_ex(const char *path, const spawn_rlimits_t *attrs) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_SPAWN_EX), "D"(path), "S"(attrs)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 7d: Send a signal to a process
static inline int syscall_kill(int pid, int signal) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_KILL), "D"(pid), "S"(signal) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 7d: Get current process ID
static inline int syscall_getpid(void) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(SYS_GETPID) : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8a: Get system state snapshot
// category: STATE_CAT_* from state.h
// buf: user buffer to fill, or NULL to query required size
// buf_size: size of user buffer
// Returns: bytes written on success, required size if buf is NULL, negative on error
static inline long syscall_get_system_state(uint32_t category, void *buf, size_t buf_size) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_GET_SYSTEM_STATE), "D"(category), "S"(buf), "d"(buf_size)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 8b: Capability Activation Network
static inline int syscall_cap_activate(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_ACTIVATE), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

static inline int syscall_cap_deactivate(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_DEACTIVATE), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8b-ii: Register a user-owned capability
// type: CAP_APPLICATION(3), CAP_FEATURE(4), or CAP_COMPOSITE(5)
// dep_names: array of dependency name strings, dep_count: number of deps
// Returns: cap_id (>=0) on success, negative error on failure
static inline int syscall_cap_register(const char *name, uint32_t type,
                                       const char **dep_names, int dep_count) {
    long ret;
    register long r10 asm("r10") = (long)dep_count;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_REGISTER), "D"(name), "S"(type), "d"(dep_names), "r"(r10)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8b-ii: Unregister a user-owned capability
// Returns: 0 on success, negative error on failure
static inline int syscall_cap_unregister(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_UNREGISTER), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8c: AI Metadata operations
// Set AI metadata on a file by path
static inline int syscall_set_ai_metadata(const char *path, const void *meta) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_SET_AI_METADATA), "D"(path), "S"(meta)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Get AI metadata from a file by path
static inline int syscall_get_ai_metadata(const char *path, void *meta) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_GET_AI_METADATA), "D"(path), "S"(meta)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Search files by tag substring
// Returns: number of matches found, negative on error
static inline int syscall_search_by_tag(const char *tag, void *results, int max) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_SEARCH_BY_TAG), "D"(tag), "S"(results), "d"(max)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Watch a capability for state change events
static inline int syscall_cap_watch(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_WATCH), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Stop watching a capability
static inline int syscall_cap_unwatch(const char *name) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_UNWATCH), "D"(name)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 8d: Poll for CAN events (blocking with -99 retry)
// events: buffer for state_cap_event_t array
// max_events: max number of events to dequeue
// Returns: number of events dequeued, or -99 (retry/block)
static inline int syscall_cap_poll(void *events, int max_events) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_CAP_POLL), "D"(events), "S"(max_events)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 8d: Non-blocking poll for CAN events (returns immediately)
// Returns: number of events dequeued, 0 if none pending
static inline int syscall_cap_poll_nonblock(void *events, int max_events) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CAP_POLL), "D"(events), "S"(max_events)
        : "rcx", "r11", "memory");
    // Don't retry on -99, just return it (caller handles)
    return (int)ret;
}

// Phase 9a: Get network interface info
// buf: at least 7 bytes (6 MAC + 1 link_up)
// Returns: 0 on success, -1 bad pointer, -2 no NIC
static inline int syscall_net_ifconfig(void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_NET_IFCONFIG), "D"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9b: Get network stack status
// buf: pointer to net_status_t (21 bytes)
// Returns: 0 on success, -1 bad pointer
static inline int syscall_net_status(void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_NET_STATUS), "D"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9c: HTTP GET (blocking, returns body length or negative error)
// url: HTTP URL string (e.g. "http://10.0.2.15/")
// response_buf: buffer for response body
// max_len: maximum bytes to store in response_buf
// Returns: body length (>=0) on success, negative error on failure
static inline int syscall_http_get(const char *url, char *response_buf, int max_len) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_HTTP_GET), "D"(url), "S"(response_buf), "d"(max_len)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 10b: Create a pipe
// fds[0] = read end, fds[1] = write end
// Returns: 0 on success, -1 on failure
static inline int syscall_pipe(int fds[2]) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_PIPE), "D"(fds)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10b: Duplicate file descriptor to specific slot
// Returns: new_fd on success, -1 on failure
static inline int syscall_dup2(int old_fd, int new_fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DUP2), "D"(old_fd), "S"(new_fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10b: Duplicate file descriptor to lowest available slot
// Returns: new fd number on success, -1 on failure
static inline int syscall_dup(int old_fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DUP), "D"(old_fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 10c: Truncate file to 0 bytes
// Returns: 0 on success, -1 on failure
static inline int syscall_truncate(int fd) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_TRUNCATE), "D"(fd)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 11a: Compute SimHash for a file
// Returns 64-bit SimHash on success, 0 on failure
static inline uint64_t syscall_compute_simhash(const char *path) {
    uint64_t ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_COMPUTE_SIMHASH), "D"(path)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 11a: Find files similar to path by SimHash Hamming distance
// threshold: max Hamming distance (0 = use default 10)
// results: pointer to grahafs_search_results_t
// Returns: number of matches, negative on error
static inline int syscall_find_similar(const char *path, int threshold, void *results) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_FIND_SIMILAR), "D"(path), "S"(threshold), "d"(results)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 11b: Get list of all clusters
// buf: pointer to cluster_list_t
// Returns: number of clusters, negative on error
static inline int syscall_cluster_list(void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CLUSTER_LIST), "D"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 11b: Get members of a specific cluster
// cluster_id: 1-based cluster ID
// buf: pointer to cluster_members_t
// Returns: member count on success, negative on error
static inline int syscall_cluster_members(uint32_t cluster_id, void *buf) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_CLUSTER_MEMBERS), "D"(cluster_id), "S"(buf)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 13: klog write. Emit one entry into the kernel log ring.
// level must be < KLOG_FATAL (user cannot emit FATAL).
// subsys must be >= 10 (kernel subsystem ids are reserved).
// msg may be any bytes; the kernel bounds them to 223 bytes.
// Returns 0 on success, -1 on validation failure or copy error.
static inline int syscall_klog_write(uint8_t level, uint8_t subsys,
                                     const char *msg, uint32_t msg_len) {
    long ret;
    register long r10 asm("r10") = (long)msg_len;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_KLOG_WRITE), "D"((long)level), "S"((long)subsys),
          "d"(msg), "r"(r10)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 13: klog read. Copy the most recent `tail_count` entries (0 =
// all currently held) matching `level_mask` into `buf`. `level_mask`
// is a bitmap (bit N = include level N); 0 means "all levels".
// Returns the number of klog_entry_t records written, or negative
// on error. Each record is 256 bytes; buf_cap should be a multiple.
static inline int syscall_klog_read(uint8_t level_mask, uint32_t tail_count,
                                    void *buf, uint32_t buf_cap) {
    long ret;
    register long r10 asm("r10") = (long)buf_cap;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_KLOG_READ), "D"((long)level_mask), "S"((long)tail_count),
          "d"(buf), "r"(r10)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 13: controlled-panic trigger for gate tests. Build-gated;
// returns -1 on release builds. op=DEBUG_PANIC takes a reason string
// in RSI. op=DEBUG_KERNEL_PF takes no args.
static inline int syscall_debug(int op, const char *arg) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DEBUG), "D"((long)op), "S"(arg)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 14: generic SYS_DEBUG wrapper taking 3 scalar args (used for
// percpu/kmalloc/kfree test hooks).
static inline long syscall_debug3(int op, long a1, long a2) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_DEBUG), "D"((long)op), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 14: kheap_stats_entry_t — must mirror kernel/mm/kheap.h exactly.
typedef struct kheap_stats_entry_u {
    char     name[32];
    uint32_t object_size;
    uint32_t _pad0;
    uint64_t in_use;
    uint64_t free;
    uint32_t pages;
    uint32_t _pad1;
    uint32_t subsys_counters[16];
} kheap_stats_entry_u_t;

// SYS_KHEAP_STATS: fill buffer with up to `max` entries. Returns the
// count written, or -1 on error.
static inline int syscall_kheap_stats(kheap_stats_entry_u_t *buf, uint32_t max) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
        : "a"(SYS_KHEAP_STATS), "D"(buf), "S"((long)max)
        : "rcx", "r11", "memory");
    return (int)ret;
}

// Phase 9c: DNS resolve (blocking, returns 0 or negative error)
// hostname: hostname to resolve (e.g. "dns.google")
// ip_buf: buffer for 4-byte IPv4 address result
// Returns: 0 on success, negative error on failure
static inline int syscall_dns_resolve(const char *hostname, uint8_t *ip_buf) {
    long ret;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_DNS_RESOLVE), "D"(hostname), "S"(ip_buf)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// Phase 9e: HTTP POST (blocking, returns body length or negative error)
// url: full URL (http:// or https://)
// body: POST body data
// body_len: length of POST body
// response_buf: buffer for response body
// max_len: maximum bytes to store in response_buf
// Returns: body length (>=0) on success, negative error on failure
static inline int syscall_http_post(const char *url, const char *body, int body_len,
                                     char *response_buf, int max_len) {
    long ret;
    register const char *r10 asm("r10") = response_buf;
    register long r8 asm("r8") = (long)max_len;
    do {
        asm volatile("syscall" : "=a"(ret)
            : "a"(SYS_HTTP_POST), "D"(url), "S"(body), "d"((long)body_len),
              "r"(r10), "r"(r8)
            : "rcx", "r11", "memory");
        if (ret == -99) continue;
        break;
    } while (1);
    return (int)ret;
}

// ------------------------------------------------------------------------
// Phase 15a: Capability Objects v2 wrappers.
// ------------------------------------------------------------------------

// SYS_CAP_DERIVE(parent_tok, rights_subset, audience_subset, flags_subset)
// Returns the new token's raw u64 on success, or a negative CAP_V2_* on
// failure (test with `(long)ret < 0`).
static inline cap_token_raw_t syscall_cap_derive(cap_token_raw_t parent_tok,
                                                 uint64_t rights_subset,
                                                 const int32_t *audience_subset,
                                                 uint8_t flags_subset) {
    long ret;
    register long r10 asm("r10") = (long)flags_subset;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAP_DERIVE), "D"(parent_tok), "S"(rights_subset),
          "d"(audience_subset), "r"(r10)
        : "rcx", "r11", "memory");
    return (cap_token_raw_t)ret;
}

// SYS_CAP_REVOKE_V2(target_tok) -> count invalidated or -CAP_V2_*.
static inline long syscall_cap_revoke_v2(cap_token_raw_t tok) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAP_REVOKE_V2), "D"(tok)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_CAP_GRANT(tok, target_pid) -> slot idx or -CAP_V2_*.
static inline long syscall_cap_grant(cap_token_raw_t tok, int32_t target_pid) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAP_GRANT), "D"(tok), "S"((long)target_pid)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_CAP_INSPECT(tok, out) -> 0 on success or -CAP_V2_*.
static inline long syscall_cap_inspect(cap_token_raw_t tok, cap_inspect_result_u_t *out) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAP_INSPECT), "D"(tok), "S"(out)
        : "rcx", "r11", "memory");
    return ret;
}

// Helpers to inspect token fields from user code.
static inline uint32_t cap_token_u_gen(cap_token_raw_t t) { return (uint32_t)(t >> 32); }
static inline uint32_t cap_token_u_idx(cap_token_raw_t t) { return (uint32_t)((t >> 8) & 0xFFFFFFULL); }
static inline uint8_t  cap_token_u_flags(cap_token_raw_t t) { return (uint8_t)(t & 0xFFu); }

// Phase 15b: SYS_PLEDGE(new_mask) -> 0 on success, -CAP_V2_EPERM for widen,
// -CAP_V2_EINVAL for reserved bits or mask==0.
static inline long syscall_pledge(uint16_t new_mask) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_PLEDGE), "D"((uint64_t)new_mask)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 15b: SYS_AUDIT_QUERY(since_ns, until_ns, event_mask, buf, max) ->
// number of entries written, or negative -CAP_V2_*.
static inline long syscall_audit_query(uint64_t since_ns, uint64_t until_ns,
                                       uint32_t event_mask,
                                       audit_entry_u_t *buf, uint32_t max) {
    register uint64_t r10 asm("r10") = (uint64_t)buf;
    register uint64_t r8  asm("r8")  = (uint64_t)max;
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_AUDIT_QUERY), "D"(since_ns), "S"(until_ns),
          "d"((uint64_t)event_mask), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

// -------------------------------------------------------------------------
// Phase 16 — token-taking CAN control + name→token lookup.
//   SYS_CAN_ACTIVATE_T(tok)        -> 0 on success, negative on error.
//   SYS_CAN_DEACTIVATE_T(tok)      -> cascade count on success, negative on error.
//   SYS_CAN_LOOKUP(name, len)      -> cap_token_u_t.raw, or 0 if not found.
// -------------------------------------------------------------------------
static inline long syscall_can_activate_t(cap_token_u_t tok) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAN_ACTIVATE_T), "D"(tok.raw)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_can_deactivate_t(cap_token_u_t tok) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CAN_DEACTIVATE_T), "D"(tok.raw)
        : "rcx", "r11", "memory");
    return ret;
}

static inline cap_token_u_t syscall_can_lookup(const char *name, unsigned long name_len) {
    cap_token_u_t out;
    asm volatile("syscall"
        : "=a"(out.raw)
        : "a"(SYS_CAN_LOOKUP), "D"(name), "S"(name_len)
        : "rcx", "r11", "memory");
    return out;
}

// -------------------------------------------------------------------------
// Phase 17 — Channels + VMOs.
// -------------------------------------------------------------------------

// User-visible message header (32 bytes, matches kernel layout).
typedef struct __attribute__((packed)) chan_msg_header_u {
    uint64_t seq;
    uint64_t type_hash;
    uint32_t sender_pid;
    uint16_t inline_len;
    uint8_t  nhandles;
    uint8_t  flags;
    uint64_t timestamp_tsc;
} chan_msg_header_u_t;

// Userspace-visible message body: 32 + 256 + 8*8 = 352 bytes.
typedef struct chan_msg_user {
    chan_msg_header_u_t header;
    uint8_t             inline_payload[CHAN_MSG_INLINE_MAX];
    cap_token_raw_t     handles[CHAN_MSG_HANDLES_MAX];
} chan_msg_user_t;

typedef struct chan_poll_entry {
    cap_token_raw_t handle;
    uint32_t        wanted;
    uint32_t        revents;
} chan_poll_entry_t;

// SYS_CHAN_CREATE: rdi=type_hash, rsi=wr_out_ptr, rdx=mode|(capacity<<32).
// Returns read_handle.raw on success; writes write_handle to *wr_out.
static inline long syscall_chan_create(uint64_t type_hash, uint32_t mode,
                                       uint32_t capacity, cap_token_u_t *wr_out) {
    long ret;
    uint64_t packed = (uint64_t)mode | ((uint64_t)capacity << 32);
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_CREATE), "D"(type_hash), "S"((uint64_t)wr_out), "d"(packed)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_chan_send(cap_token_u_t wr_handle,
                                     const chan_msg_user_t *msg,
                                     uint64_t timeout_ns) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_SEND), "D"(wr_handle.raw), "S"(msg), "d"(timeout_ns)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_chan_recv(cap_token_u_t rd_handle,
                                     chan_msg_user_t *msg,
                                     uint64_t timeout_ns) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_RECV), "D"(rd_handle.raw), "S"(msg), "d"(timeout_ns)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_chan_poll(chan_poll_entry_t *polls, uint32_t npolls,
                                     uint64_t timeout_ns) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_POLL), "D"(polls), "S"((uint64_t)npolls), "d"(timeout_ns)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 22: named channel registry wrappers. Publish registers the caller as
// the owner of `name`; connect allocates a fresh bidirectional channel pair
// (request direction + response direction) and delivers the server-side
// endpoints to the publisher via its registered accept channel.
//
// Note: R10 is not reliably set from inline asm under our syscall ABI (same
// caveat as SYS_VMO_MAP above); we stage it explicitly.
static inline long syscall_chan_publish(const char *name, uint32_t name_len,
                                        uint64_t payload_type_hash,
                                        cap_token_u_t accept_write_end) {
    long ret;
    asm volatile(
        "movq %5, %%r10\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_PUBLISH), "D"(name), "S"((uint64_t)name_len),
          "d"(payload_type_hash), "r"(accept_write_end.raw)
        : "rcx", "r10", "r11", "memory");
    return ret;
}

static inline long syscall_chan_connect(const char *name, uint32_t name_len,
                                        cap_token_u_t *out_wr_req,
                                        cap_token_u_t *out_rd_resp) {
    long ret;
    asm volatile(
        "movq %5, %%r10\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(SYS_CHAN_CONNECT), "D"(name), "S"((uint64_t)name_len),
          "d"(out_wr_req), "r"(out_rd_resp)
        : "rcx", "r10", "r11", "memory");
    return ret;
}

static inline long syscall_vmo_create(uint64_t size_bytes, uint32_t flags) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_CREATE), "D"(size_bytes), "S"((uint64_t)flags)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_vmo_map(cap_token_u_t vmo_handle, uint64_t addr_hint,
                                   uint64_t offset, uint64_t len, uint32_t prot) {
    long ret;
    asm volatile(
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_MAP), "D"(vmo_handle.raw), "S"(addr_hint), "d"(offset),
          "r"(len), "r"((uint64_t)prot)
        : "rcx", "r8", "r10", "r11", "memory");
    return ret;
}

static inline long syscall_vmo_unmap(uint64_t vaddr, uint64_t len) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_UNMAP), "D"(vaddr), "S"(len)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_vmo_clone(cap_token_u_t src_handle, uint32_t flags) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_CLONE), "D"(src_handle.raw), "S"((uint64_t)flags)
        : "rcx", "r11", "memory");
    return ret;
}

// Userspace FNV-1a 64-bit hash. Matches kernel/fs/simhash.c:fnv1a_hash64.
// Compute type_hash for a manifest string at program start:
//   uint64_t h = gcp_type_hash("grahaos.notify.v1");
static inline uint64_t gcp_type_hash(const char *name) {
    uint64_t h = 0xCBF29CE484222325ULL;
    const uint8_t *p = (const uint8_t *)name;
    while (p && *p) {
        h ^= (uint64_t)(*p);
        h *= 0x00000100000001B3ULL;
        p++;
    }
    return h;
}

// =========================================================================
// Phase 18: Submission Streams (Async I/O).
// =========================================================================
//
// Opcode / type-hash / flag constants come from
// user/include/gcp_ops_generated.h which scripts/gen_manifest.py emits from
// /etc/gcp.json. Include that file from any program using streams.

// Userspace SQE (submission queue entry). ABI-identical to kernel sqe_t.
typedef struct __attribute__((packed)) sqe_u {
    uint16_t op;
    uint16_t flags;
    uint32_t fd_or_handle;
    uint64_t offset;
    uint64_t len;
    uint32_t dest_vmo_handle;
    uint32_t _pad0;
    uint64_t dest_vmo_offset;
    uint64_t cookie;
    uint8_t  reserved[16];
} sqe_u_t;

_Static_assert(sizeof(sqe_u_t) == 64, "userspace sqe_u_t must be 64 bytes");

// Userspace CQE (completion queue entry). ABI-identical to kernel cqe_t.
typedef struct __attribute__((packed)) cqe_u {
    uint64_t cookie;
    int64_t  result;
    uint32_t flags;
    uint8_t  reserved[12];
} cqe_u_t;

_Static_assert(sizeof(cqe_u_t) == 32, "userspace cqe_u_t must be 32 bytes");

// Out-struct filled by SYS_STREAM_CREATE.
typedef struct __attribute__((packed)) stream_handles_u {
    uint64_t stream_handle_raw;    // cap_token_raw_t for CAP_KIND_STREAM
    uint64_t sq_vmo_handle_raw;    // cap_token_raw_t for SQ CAP_KIND_VMO
    uint64_t cq_vmo_handle_raw;    // cap_token_raw_t for CQ CAP_KIND_VMO
    uint64_t reserved;
} stream_handles_u_t;

_Static_assert(sizeof(stream_handles_u_t) == 32,
               "stream_handles_u_t must be 32 bytes");

// Ring metadata offsets. Head at byte 0, tail at byte 128, entries start
// at page 1 (byte 4096).
#define STREAM_RING_HEAD_OFFSET  0
#define STREAM_RING_TAIL_OFFSET  128
#define STREAM_RING_ENTRIES_OFFSET  4096

// Ring geometry limits (mirror kernel).
#define STREAM_MIN_ENTRIES        16u
#define STREAM_MAX_SQ_ENTRIES     4096u
#define STREAM_MAX_CQ_ENTRIES     8192u

// SYS_STREAM_CREATE:
//   rdi = type_hash
//   rsi = stream_handles_u_t *out
//   rdx = sq_entries | (cq_entries << 32)
//   r8  = notify_wr_handle_raw (0 = no notify)
// Returns 0 on success; writes handles to *out. Negative errno on failure.
static inline long syscall_stream_create(uint64_t type_hash,
                                         uint32_t sq_entries,
                                         uint32_t cq_entries,
                                         stream_handles_u_t *out,
                                         uint64_t notify_wr_handle_raw) {
    long ret;
    uint64_t packed = (uint64_t)sq_entries | ((uint64_t)cq_entries << 32);
    asm volatile(
        "movq %5, %%r8\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(SYS_STREAM_CREATE), "D"(type_hash), "S"((uint64_t)out),
          "d"(packed), "r"(notify_wr_handle_raw)
        : "rcx", "r8", "r11", "memory");
    return ret;
}

// SYS_STREAM_SUBMIT: returns number of SQEs processed (both accepted and
// rejected-with-immediate-CQE), negative errno on failure.
static inline long syscall_stream_submit(uint64_t stream_handle_raw,
                                         uint32_t n_to_submit) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_STREAM_SUBMIT), "D"(stream_handle_raw),
          "S"((uint64_t)n_to_submit)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_STREAM_REAP: block until min_complete CQEs ready or timeout_ns. On
// return, number of CQEs currently ready (>= min_complete unless TIMEDOUT).
static inline long syscall_stream_reap(uint64_t stream_handle_raw,
                                       uint32_t min_complete,
                                       uint64_t timeout_ns) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_STREAM_REAP), "D"(stream_handle_raw),
          "S"((uint64_t)min_complete), "d"(timeout_ns)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_stream_destroy(uint64_t stream_handle_raw) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_STREAM_DESTROY), "D"(stream_handle_raw)
        : "rcx", "r11", "memory");
    return ret;
}

// ------------------------------------------------------------------------
// Phase 19: GrahaFS v2 — version info struct + 4 syscall wrappers + fsync.
// fs_version_info_u_t mirrors kernel/fs/vfs.h:fs_version_info_t layout
// exactly. A _Static_assert on sizeof guards against drift.
// ------------------------------------------------------------------------
typedef struct {
    uint64_t version_id;       //   0..7
    uint64_t timestamp_ns;     //   8..15
    uint64_t size;             //  16..23
    uint64_t simhash;          //  24..31
    uint32_t cluster_id;       //  32..35
    uint32_t segment_id;       //  36..39
    uint64_t parent_version;   //  40..47
    uint64_t prev_version;     //  48..55
    uint8_t  reserved[8];      //  56..63
} fs_version_info_u_t;

_Static_assert(sizeof(fs_version_info_u_t) == 64,
               "fs_version_info_u_t must be 64 bytes (ABI match with kernel)");

// SYS_FS_SNAPSHOT: captures version-chain heads of every live inode and
// pins their segments. Returns a cap_token_raw_t of CAP_KIND_FS_SNAPSHOT,
// or a negative errno. label may be NULL (in which case label_len must be 0).
static inline long syscall_fs_snapshot(const char *label, uint32_t label_len) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_FS_SNAPSHOT), "D"((unsigned long)(uintptr_t)label),
          "S"((unsigned long)label_len)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_FS_LIST_VERSIONS: fills out[0..max_count) with newest-first version
// records for `inode_num`. Returns entries written, or -errno.
static inline long syscall_fs_list_versions(uint32_t inode_num,
                                            fs_version_info_u_t *out,
                                            uint32_t max_count) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_FS_LIST_VERSIONS), "D"((unsigned long)inode_num),
          "S"((unsigned long)(uintptr_t)out), "d"((unsigned long)max_count)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_FS_REVERT: non-destructive revert — allocates a new version_record that
// references the target's data segments. Returns the new version_id
// (positive uint64_t cast to long), or -errno.
static inline long syscall_fs_revert(uint32_t inode_num,
                                     uint64_t target_version_id) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_FS_REVERT), "D"((unsigned long)inode_num),
          "S"(target_version_id)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_FS_GC_NOW: synchronous garbage collection pass. Returns pruned count or -errno.
static inline long syscall_fs_gc_now(void) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_FS_GC_NOW)
        : "rcx", "r11", "memory");
    return ret;
}

// SYS_FSYNC: force-durably commits any in-flight journal txn for the file
// backing `fd`. Returns 0 or -errno.
static inline long syscall_fsync(int fd) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_FSYNC), "D"((unsigned long)fd)
        : "rcx", "r11", "memory");
    return ret;
}

// Minimal brk wrapper (syscall 1016) — passes NULL to query current brk,
// or a new address to grow/shrink. Returns the resulting brk or -1.
#ifndef SYS_BRK
#define SYS_BRK 1016
#endif
static inline long syscall_brk(void *addr) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_BRK), "D"((unsigned long)addr)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 20: SYS_SETRLIMIT — set a resource limit on a task. pid == 0 means
// "self". Requires PLEDGE_SYS_CONTROL on caller regardless of whether the
// value raises or lowers the existing limit (self-lowering is not allowed).
// Returns 0 on success, or -ESRCH / -EINVAL / -EPLEDGE.
static inline long syscall_setrlimit(unsigned int pid,
                                     unsigned int resource,
                                     unsigned long value) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_SETRLIMIT), "D"((unsigned long)pid),
          "S"((unsigned long)resource), "d"(value)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 20: SYS_GETRLIMIT — read a resource limit. pid == 0 means "self".
// Writes the current limit to *value_out on success. Requires PLEDGE_SYS_QUERY
// (default-granted). Returns 0 on success, or -ESRCH / -EINVAL / -EFAULT.
static inline long syscall_getrlimit(unsigned int pid,
                                     unsigned int resource,
                                     unsigned long *value_out) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_GETRLIMIT), "D"((unsigned long)pid),
          "S"((unsigned long)resource), "d"(value_out)
        : "rcx", "r11", "memory");
    return ret;
}