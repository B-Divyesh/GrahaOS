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

// Phase 25 transactional speculation. Slots 1098-1100 (1090-1097 are
// reserved by Phase 22-24 syscalls). SYS_TXN_BEGIN implicitly creates a
// Phase-24 snapshot via snap_create_internal and pushes a transaction
// frame onto the caller's task; chan_send while a txn is active is
// intercepted (Stage E). SYS_TXN_COMMIT replays buffered external sends
// in original order then discards the snapshot; SYS_TXN_ABORT drops the
// buffer and restores the snapshot.
#define SYS_TXN_BEGIN         1098
#define SYS_TXN_COMMIT        1099
#define SYS_TXN_ABORT         1100

// Phase 27 syscalls (slots 1101-1112). Block A TUI Framework, Block B
// Graphics, Block C AI Primitives. See arch/x86_64/cpu/syscall/syscall.h
// for full per-slot documentation. Stage A2 wires SWITCH + ACK_RENDER;
// remaining slots stub to -ENOSYS until their owning stages.
#define SYS_CONSOLE_SWITCH          1101
#define SYS_CONSOLE_CREATE          1102
#define SYS_CONSOLE_ATTACH          1103
#define SYS_CONSOLE_INSPECT         1104
#define SYS_CONSOLE_OBSERVE         1105
#define SYS_CONSOLE_ACK_RENDER      1106
#define SYS_CONSOLE_SPRITE_REGISTER 1107
#define SYS_CONSOLE_GFX_ENABLE      1108
#define SYS_CONSOLE_GFX_DAMAGE      1109
#define SYS_AUDIT_SUBSCRIBE         1110
#define SYS_AUDIT_STREAM_READ       1111
#define SYS_MANIFEST_EXPORT         1112
// Pre-Phase-28 sweep C.1 (FU25.A.3 substrate): expose grahafs_pin_version.
#define SYS_GRAHAFS_PIN_VERSION     1113
#define SYS_TXN_PIN_PATH            1114
// Phase 29 Session C (FU28.B): SYS_SPAWN_ARGV — spawn child AND pass argv.
// RDI = path, RSI = char *const *argv, RDX = uint32_t argc (≤ 16).
#define SYS_SPAWN_ARGV              1115
// Phase 29 Session D — TUI primitives.
#define SYS_CONSOLE_READ_INPUT      1116
#define SYS_CONSOLE_GFX_MAP_FB      1117
#define SYS_CONSOLE_VSYNC_WAIT      1118

// Phase 29 Session E — sprite animation + cell-grid atomic TX.
#define SYS_CONSOLE_SPRITE_ANIMATE  1119
#define SYS_CONSOLE_BEGIN_TX        1120
#define SYS_CONSOLE_COMMIT_TX       1121
#define SYS_CONSOLE_ABORT_TX        1122

// Phase 24 W19: COW snapshot subsystem (slots reconciled to 1093-1096
// because spec's original 1086-1089 collide with SPAWN_EX..MMIO_VMO_CREATE).
#define SYS_SNAP_CREATE       1093
#define SYS_SNAP_RESTORE      1094
#define SYS_SNAP_DELETE       1095
#define SYS_SNAP_LIST         1096

// Phase 24 sub-phase H.1: TSC frequency query — substrate for replacing
// userspace spin_ms_approx() loops (currently calibrated for QEMU TCG;
// would collapse under KVM) with rdtsc-driven busy waits.  Returns the
// TSC tick rate measured at boot (g_tsc_hz, in Hz).  Returns 0 if the
// TSC has not been calibrated yet (very early boot only).
#define SYS_TSC_HZ_QUERY      1097

// SNAP_SCOPE_* must match kernel/snap/snapshot.h. Bitfield argument to
// SYS_SNAP_CREATE.flags.
#define SNAP_SCOPE_SELF             0x00000001u
#define SNAP_SCOPE_GLOBAL           0x00000002u
#define SNAP_SCOPE_FREEZE_ALL_CHANS 0x00000004u

#define SNAP_NAME_MAX_LEN_USER  31u  // Max chars (excl NUL) in name.

// SNAP_STATE_* must match kernel/snap/snapshot.h.
#define SNAP_STATE_ACTIVE     1u
#define SNAP_STATE_RESTORING  2u
#define SNAP_STATE_DELETED    3u

// snap_info_t — record returned by SYS_SNAP_LIST. Stable 88-byte ABI.
typedef struct snap_info_user {
    uint64_t id;                        //  0
    uint64_t created_utc_ns;            //  8
    int32_t  creator_pid;               // 16
    uint32_t scope_flags;               // 20
    uint32_t state;                     // 24
    uint32_t task_count;                // 28
    uint32_t vmo_count;                 // 32
    uint32_t chan_count;                // 36
    uint64_t pages_shared;              // 40
    uint64_t pages_diverged;            // 48
    char     name[32];                  // 56..87 (31 chars + NUL)
} snap_info_user_t;

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

#define SPAWN_ATTR_HAS_RLIMIT_U   (1u << 0)
// Phase 29 Session C (FU25.H): when set, attrs.handles_to_inherit[0..n-1]
// (slot indices in the caller's handle table) are cap_handle_insert'd into
// the child's handle table AFTER the spawn succeeds (in addition to the
// CAP_FLAG_INHERITABLE walk that runs unconditionally).  Pre-validated;
// any unresolvable slot fails the syscall with -EINVAL BEFORE the spawn.
#define SPAWN_ATTR_HAS_HANDLES_U  (1u << 1)

// Userspace mirror of the kernel's spawn_attrs_t subset — same layout as the
// kernel tail fields so the syscall can copy straight across. Fields not set
// by the caller MUST be zeroed (the kernel reads trailing fields conditionally
// on flag bits; stale pointer-like slots could be misinterpreted).
typedef struct {
    uint32_t flags;           // SPAWN_ATTR_HAS_RLIMIT_U / SPAWN_ATTR_HAS_HANDLES_U
    uint32_t _pad;
    uint64_t rlimit_mem_pages;
    uint64_t rlimit_cpu_ns;
    uint64_t rlimit_io_bps;
    // Phase 29 Session C (FU25.H): handles_to_inherit holds cap_token_raw_t
    // values (returned from SYS_VMO_CREATE, SYS_CHAN_CREATE, etc.); the
    // kernel resolves each token + cap_handle_insert into the child's table.
    // n_handles_to_inherit bounds the walk.  Only honored when
    // SPAWN_ATTR_HAS_HANDLES_U is set in flags.
    uint64_t handles_to_inherit[16];
    uint32_t n_handles_to_inherit;
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
// FU26.C: kernel-side ksnprintf reachable from user/tests/vsnprintftest.c.
#define DEBUG_VSNPRINTF        68
#define DEBUG_INJECT_SCANCODE  69
#define DEBUG_CONSOLE_GET_SELECTED 70
#define DEBUG_CONSOLE_WRITE_CELL   71
#define DEBUG_CONSOLE_SYNTHETIC_RENDER 72
#define DEBUG_CONSOLE_GFX_FILL     73
#define DEBUG_AUDIT_EMIT_PLAN      74
// FU27.X.cap_recursive_inheritance: test-only helpers used by
// user/tests/cap_recursive_inheritance.c.  CAP_CREATE_WITH_FLAGS allocates
// a CAP_KIND_PROC cap_object with caller-specified flags and audience=
// [caller_pid], then inserts into caller's cap_handle_table; returns the
// packed cap_token_raw_t (0 on failure).  CAP_CHECK_INHERITED_AUDIENCE
// walks caller's handle table; returns 0 if any cap with
// CAP_FLAG_RECURSIVE_INHERIT set has caller_pid in its audience set, 1
// otherwise.  Both gated on PLEDGE_CLASS_SYS_CONTROL like other DEBUG ops.
#define DEBUG_CAP_CREATE_WITH_FLAGS         75
#define DEBUG_CAP_CHECK_INHERITED_AUDIENCE  76
// Phase 28 Session G.1 fault injection.
#define DEBUG_INJECT_PMM_FAIL_NTH           80
#define DEBUG_INJECT_KMALLOC_FAIL_NTH       81
#define DEBUG_INJECT_CHAN_SEND_FAIL_RATE    82
#define DEBUG_INJECT_SPINLOCK_TIMEOUT_RATE  83
#define DEBUG_INJECT_RESET_ALL              84
// Phase 29 Session C (FU25.H): caller's handle table count.
#define DEBUG_HANDLE_COUNT                  85
// Phase 29 Session D — TUI test substrate.
#define DEBUG_FB_OWNER_SET                  86
#define DEBUG_DIRTY_RECT_GET_COUNTS         87
#define DEBUG_DIRTY_RECT_RESET              88
#define DEBUG_CONSOLE_READ_CELL             89
// Phase 29 Session E — animation + mouse + cell-grid TX test substrate.
#define DEBUG_ANIM_TICK                     90
#define DEBUG_ANIM_GET_FRAME                91
#define DEBUG_ANIM_GET_STATE                92
#define DEBUG_INJECT_MOUSE                  93
#define DEBUG_FB_READ_PIXEL_AT              94
#define DEBUG_MOUSE_CURSOR_VISIBLE          95
#define DEBUG_FB_READ_PIXEL    61
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
// FU27.X.cap_recursive_inheritance: when paired with CAP_FLAG_INHERITABLE,
// the child's inherited copy gets the child's pid appended to the audience
// set so the child can re-derive (re-inherit) to its own grandchildren.
#define CAP_FLAG_RECURSIVE_INHERIT 0x40
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
// Phase 26 mirrors of kernel/audit.h codes 45-49.
#define AUDIT_WASM_CAP_DENIED        45
#define AUDIT_WASM_TRAP              46
#define AUDIT_WASM_CRASHED           47
#define AUDIT_WASM_OUT_OF_FUEL       48
#define AUDIT_PLEDGE_NARROW_EXEC     49
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
// Phase 25: transaction lifecycle events.
#define AUDIT_TXN_BEGIN              41
#define AUDIT_TXN_COMMIT             42
#define AUDIT_TXN_ABORT              43
#define AUDIT_TXN_PARTIAL_EXTERNAL   44
#define AUDIT_EVENT_MAX              49  // Phase 26: AUDIT_PLEDGE_NARROW_EXEC + WASM_*

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

// Phase 29 Session C (FU28.B): spawn AND pass argv.  argv MUST be an array
// of at least argc valid `char *` pointers; each pointee MUST be a
// NUL-terminated string (≤ 255 chars).  Kernel copies all strings into
// scratch buffers, spawns the child, then seeds argc in RDI and an argv
// pointer in RSI on the child's stack.  Total bytes across all argv
// strings must be ≤ 3072 bytes.
// Returns child pid (>0) or negative -errno (-EFAULT/-EINVAL/-E2BIG/
// -EPLEDGE/-ENOMEM).
static inline int syscall_spawn_argv(const char *path, int argc,
                                     char *const argv[]) {
    long ret;
    register long r10 asm("r10") = 0;  /* unused */
    (void)r10;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_SPAWN_ARGV), "D"(path), "S"(argv), "d"((long)argc)
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

// FU26.C: SYS_DEBUG wrapper for DEBUG_VSNPRINTF.
//   fmt:  user format string (NUL-terminated, max 255 bytes).
//   a1, a2: caller-defined uint64_t args, passed through to ksnprintf.
//   out:  user-side 256-byte buffer to receive the formatted output.
// Returns the number of bytes the formatted output would consume
// (vsnprintf convention) or -EFAULT on copy failure.
static inline long syscall_debug_vsnprintf(const char *fmt,
                                           uint64_t a1, uint64_t a2,
                                           char *out) {
    long ret;
    register uint64_t r10 asm("r10") = a2;
    register uint64_t r8  asm("r8")  = (uint64_t)(uintptr_t)out;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_VSNPRINTF),
          "S"((uint64_t)(uintptr_t)fmt),
          "d"(a1),
          "r"(r10),
          "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Block A (Stage A3): inject a PS/2 scancode into the keyboard ISR
// handler. Used by user/tests/keyboard_alt.c to verify Alt+N detection
// without poking real hardware. Returns 0.
static inline long syscall_debug_inject_scancode(uint8_t scancode) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_INJECT_SCANCODE),
          "S"((uint64_t)scancode)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Block A (Stage A3): query selected console (0..NUM_CONSOLES-1).
static inline uint32_t syscall_debug_console_get_selected(void) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_CONSOLE_GET_SELECTED)
        : "rcx", "r11", "memory");
    return (uint32_t)ret;
}

// Phase 27 Block A (Stage A4): test-only direct cell-VMO write. Bypasses
// SYS_CONSOLE_ATTACH (which is -ENOSYS until Stage C2 cap inheritance).
// Pack: codepoint:32 | fg:8 | bg:8 | attrs:16.
static inline long syscall_debug_console_write_cell(uint32_t console_id,
                                                    uint32_t row, uint32_t col,
                                                    uint32_t codepoint,
                                                    uint8_t fg, uint8_t bg,
                                                    uint16_t attrs) {
    uint64_t packed = (uint64_t)codepoint |
                      ((uint64_t)fg << 32) |
                      ((uint64_t)bg << 40) |
                      ((uint64_t)attrs << 48);
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)col;
    register uint64_t r8  asm("r8")  = packed;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_CONSOLE_WRITE_CELL),
          "S"((uint64_t)console_id),
          "d"((uint64_t)row),
          "r"(r10),
          "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Block A (Stage A4): trigger kernel-side synthetic composite of
// the named console's cell-VMO into the framebuffer. Used by fbd_render.tap.
static inline long syscall_debug_console_synthetic_render(uint32_t console_id) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_CONSOLE_SYNTHETIC_RENDER),
          "S"((uint64_t)console_id)
        : "rcx", "r11", "memory");
    return ret;
}

// Read raw 32-bit pixel value at (x,y) from the hardware framebuffer.
// Returns the ARGB word as written by framebuffer_force_draw_cell or zero
// if (x,y) is out of bounds. Used by fbd_render.tap to verify rendered
// pixels match expected glyph.
static inline uint32_t syscall_debug_fb_read_pixel(uint32_t x, uint32_t y) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG),
          "D"((uint64_t)DEBUG_FB_READ_PIXEL),
          "S"((uint64_t)x),
          "d"((uint64_t)y)
        : "rcx", "r11", "memory");
    return (uint32_t)ret;
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

// Phase 26 Stage D — PLEDGE_FLAG_NARROW_EXEC. Userspace mirror of
// kernel/cap/pledge.h. wasmd uses this to spawn its worker process with
// the narrowed pledge bundle + delegated capability handles already
// installed atomically (no observable wide-pledged window).
#define PLEDGE_FLAG_NARROW_EXEC_U  0x80000000u
#define WASM_PLEDGE_NARROW_DELEGATIONS_MAX_U  16
#define WASM_PLEDGE_NARROW_PATH_MAX_U         64

typedef struct wasm_pledge_narrow_args_u {
    uint16_t new_pledges;
    uint16_t reserved16;
    uint32_t flags;
    char     entry_path[WASM_PLEDGE_NARROW_PATH_MAX_U];
    uint64_t cap_delegation_list[WASM_PLEDGE_NARROW_DELEGATIONS_MAX_U];
    uint8_t  ndelegations;
    uint8_t  reserved8[7];
    // Pre-Phase-28 sweep B.1: kernel populates child_slots_out[i] with
    // the child-side handle-table slot index for cap_delegation_list[i].
    // Parent reads this after a successful syscall_pledge_narrow_exec
    // call. Used by wasmd to build a boot manifest message the worker
    // then receives via its inherited channel handle (so worker knows
    // which slot its module-VMO + response-channel are in).
    uint32_t child_slots_out[WASM_PLEDGE_NARROW_DELEGATIONS_MAX_U];
} wasm_pledge_narrow_args_u_t;

// SYS_PLEDGE | PLEDGE_FLAG_NARROW_EXEC: returns child PID on success,
// negative -CAP_V2_* on failure.
static inline long syscall_pledge_narrow_exec(const wasm_pledge_narrow_args_u_t *args) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_PLEDGE),
          "D"((uint64_t)PLEDGE_FLAG_NARROW_EXEC_U),
          "S"((uint64_t)(uintptr_t)args)
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

// Phase 25 transactional speculation. SYS_TXN_BEGIN allocates an implicit
// snapshot via snap_create_internal + pushes a transaction frame on the
// caller's task. Subsequent chan_send-while-active intercepts go into
// the txn's buffer; SYS_TXN_COMMIT replays in original order, SYS_TXN_ABORT
// drops them.
//
// Flag bits (see kernel/txn/transaction.h for full definition):
//   TXN_FLAG_SELF_SCOPE   = 0x1  (default)
//   TXN_FLAG_GLOBAL_SCOPE = 0x2  (requires CAP_KIND_SYSTEM — Phase 26+)
//   TXN_FLAG_BUFFER_2MB   = 0x10
//   TXN_FLAG_BUFFER_4MB   = 0x20 (default)
//   TXN_FLAG_BUFFER_8MB   = 0x40
#define TXN_FLAG_SELF_SCOPE   0x00000001u
#define TXN_FLAG_GLOBAL_SCOPE 0x00000002u
#define TXN_FLAG_BUFFER_2MB   0x00000010u
#define TXN_FLAG_BUFFER_4MB   0x00000020u
#define TXN_FLAG_BUFFER_8MB   0x00000040u

static inline long syscall_txn_begin(uint32_t flags, const char *name) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_TXN_BEGIN), "D"((uint64_t)flags), "S"(name)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_txn_commit(uint32_t handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_TXN_COMMIT), "D"((uint64_t)handle)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_txn_abort(uint32_t handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_TXN_ABORT), "D"((uint64_t)handle)
                 : "rcx", "r11", "memory");
    return ret;
}

// =====================================================================
// Phase 27 syscall wrappers (slots 1101-1112).
// Stage A2 wires SWITCH + ACK_RENDER; rest are stubs returning -ENOSYS.
// =====================================================================
static inline long syscall_console_switch(uint32_t console_id) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_SWITCH), "D"((uint64_t)console_id)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_ack_render(uint32_t console_id, uint64_t rendered_seq) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_ACK_RENDER), "D"((uint64_t)console_id), "S"(rendered_seq)
                 : "rcx", "r11", "memory");
    return ret;
}

// Stage A4 implementations — currently -ENOSYS.
static inline long syscall_console_create(uint32_t width_cells, uint32_t height_cells, void *out_info) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_CREATE), "D"((uint64_t)width_cells),
                   "S"((uint64_t)height_cells), "d"(out_info)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_attach(uint32_t console_id, uint64_t cap_token_raw) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_ATTACH), "D"((uint64_t)console_id), "S"(cap_token_raw)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_inspect(uint32_t console_id, void *out_buf, uint64_t buflen, uint64_t cap_token_raw) {
    long ret;
    register uint64_t r10 asm("r10") = cap_token_raw;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_INSPECT), "D"((uint64_t)console_id),
                   "S"(out_buf), "d"(buflen), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_observe(uint32_t console_id, uint64_t cap_token_raw, uint64_t *out_chan_token) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_OBSERVE), "D"((uint64_t)console_id),
                   "S"(cap_token_raw), "d"(out_chan_token)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Block B (Stage B1) — sprite + RGBA overlay wrappers. ABI:
//   SPRITE_REGISTER: RDI=console_id, RSI=sprite_id, RDX=*bitmap16 → 0/-EINVAL
//   GFX_ENABLE:      RDI=console_id, RSI=w_px, RDX=h_px → cap_idx (>0) / negative
//   GFX_DAMAGE:      RDI=console_id, RSI=x, RDX=y, R10=w, R8=h → 0/-EINVAL
static inline long syscall_console_sprite_register(uint32_t console_id,
                                                   uint32_t sprite_id,
                                                   const void *bitmap16) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_SPRITE_REGISTER),
                   "D"((uint64_t)console_id),
                   "S"((uint64_t)sprite_id),
                   "d"(bitmap16)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_gfx_enable(uint32_t console_id,
                                              uint32_t w_px, uint32_t h_px) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_GFX_ENABLE),
                   "D"((uint64_t)console_id),
                   "S"((uint64_t)w_px),
                   "d"((uint64_t)h_px)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_console_gfx_damage(uint32_t console_id,
                                              uint32_t x, uint32_t y,
                                              uint32_t w, uint32_t h) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)w;
    register uint64_t r8  asm("r8")  = (uint64_t)h;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_GFX_DAMAGE),
                   "D"((uint64_t)console_id),
                   "S"((uint64_t)x),
                   "d"((uint64_t)y),
                   "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return ret;
}

// DEBUG-only fill of overlay rect with ARGB color (Stage B1 test substrate).
// Production path is to syscall_vmo_map the cap returned by gfx_enable and
// write directly; this helper exists to keep console_gfx.tap from needing
// the full vmo_map plumbing.
static inline long syscall_debug_console_gfx_fill(uint32_t console_id,
                                                  uint32_t x, uint32_t y,
                                                  uint32_t w, uint32_t h,
                                                  uint32_t color) {
    uint64_t packed = ((uint64_t)x & 0xFFFFu) |
                      (((uint64_t)y & 0xFFFFu) << 16) |
                      (((uint64_t)w & 0xFFFFu) << 32) |
                      (((uint64_t)h & 0xFFFFu) << 48);
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)color;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_CONSOLE_GFX_FILL),
                   "S"((uint64_t)console_id),
                   "d"(packed),
                   "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

// =====================================================================
// Phase 29 Session D — TUI primitives.
// =====================================================================

// 20-byte input event ABI (mirror of kernel/console/console.h input_event_t).
typedef struct __attribute__((packed)) input_event_u {
    uint8_t  kind;            // 0=key, 1=mouse_btn, 2=mouse_motion
    uint8_t  action;          // for key: 0=press 1=release
    uint16_t key;             // scancode
    int16_t  x_or_dx;
    int16_t  y_or_dy;
    uint16_t modifiers;
    uint8_t  _pad[2];
    uint64_t timestamp_tsc;
} input_event_u_t;
_Static_assert(sizeof(input_event_u_t) == 20, "input_event_u_t must be 20 bytes");

// 24-byte framebuffer dimensions returned by SYS_CONSOLE_GFX_MAP_FB.
typedef struct __attribute__((packed)) fb_dims_u {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t pitch_bytes;
    uint32_t bpp;
    uint64_t size_bytes;
} fb_dims_u_t;
_Static_assert(sizeof(fb_dims_u_t) == 24, "fb_dims_u_t must be 24 bytes");

// Bit 62 in SYS_CONSOLE_READ_INPUT return value: more events queued.
#define CONSOLE_INPUT_MORE_FLAG (1ULL << 62)

// SYS_CONSOLE_READ_INPUT (1116) — drain console's input chan.
static inline long syscall_console_read_input(uint32_t console_id,
                                               input_event_u_t *out,
                                               uint32_t max_events) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_READ_INPUT),
                   "D"((uint64_t)console_id),
                   "S"((uint64_t)out),
                   "d"((uint64_t)max_events)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_GFX_MAP_FB (1117) — mint FB MMIO handle.
static inline long syscall_console_gfx_map_fb(uint64_t *out_handle_raw,
                                               fb_dims_u_t *out_dims) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_GFX_MAP_FB),
                   "D"((uint64_t)out_handle_raw),
                   "S"((uint64_t)out_dims)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_VSYNC_WAIT (1118) — block until next 60Hz tick or timeout.
static inline long syscall_console_vsync_wait(uint64_t max_wait_ns) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_VSYNC_WAIT), "D"(max_wait_ns)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_ATTACH (1103) — wired in Session D.  RDI = console_id,
// RSI = cap_token_raw (or 0 for substrate "trusted" mode), RDX/R10 =
// uint64_t *out_cell_token / *out_input_token.
static inline long syscall_console_attach_full(uint32_t console_id,
                                                uint64_t cap_token_raw,
                                                uint64_t *out_cell_token,
                                                uint64_t *out_input_token) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)out_input_token;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_ATTACH),
                   "D"((uint64_t)console_id),
                   "S"(cap_token_raw),
                   "d"((uint64_t)out_cell_token),
                   "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

// =====================================================================
// Phase 29 Session E — sprite animation, cell-grid atomic TX, mouse.
// =====================================================================

// SYS_CONSOLE_SPRITE_ANIMATE (1119).
//   RDI = console_id, RSI = sprite_id,
//   RDX = const tui_cell_t *keyframes,
//   R10 = n_frames, R8 = duration_ms_per_frame.
static inline long syscall_console_sprite_animate(uint32_t console_id,
                                                  uint32_t sprite_id,
                                                  const void *keyframes,
                                                  uint16_t n_frames,
                                                  uint32_t duration_ms) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)n_frames;
    register uint64_t r8  asm("r8")  = (uint64_t)duration_ms;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_SPRITE_ANIMATE),
                   "D"((uint64_t)console_id),
                   "S"((uint64_t)sprite_id),
                   "d"((uint64_t)keyframes),
                   "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_BEGIN_TX (1120).
static inline long syscall_console_begin_tx(uint32_t console_id) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_BEGIN_TX), "D"((uint64_t)console_id)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_COMMIT_TX (1121).
static inline long syscall_console_commit_tx(uint32_t tx_handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_COMMIT_TX), "D"((uint64_t)tx_handle)
                 : "rcx", "r11", "memory");
    return ret;
}

// SYS_CONSOLE_ABORT_TX (1122).
static inline long syscall_console_abort_tx(uint32_t tx_handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_CONSOLE_ABORT_TX), "D"((uint64_t)tx_handle)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only Session E DEBUG wrappers.
static inline long syscall_debug_anim_tick(uint32_t console_id) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_ANIM_TICK),
                   "S"((uint64_t)console_id)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_debug_anim_get_frame(uint32_t console_id,
                                                uint32_t slot) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_ANIM_GET_FRAME),
                   "S"((uint64_t)console_id),
                   "d"((uint64_t)slot)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_debug_anim_get_state(uint32_t console_id,
                                                uint32_t slot) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_ANIM_GET_STATE),
                   "S"((uint64_t)console_id),
                   "d"((uint64_t)slot)
                 : "rcx", "r11", "memory");
    return ret;
}

// Inject a synthetic mouse event.  Packed:
//   bits 0..7   kind  (1=button, 2=motion)
//   bits 8..15  action(0=press, 1=release)
//   bits 16..31 dx (int16)
//   bits 32..47 dy (int16)
//   bits 48..55 button (0=left,1=right,2=middle)
static inline long syscall_debug_inject_mouse(uint8_t kind, uint8_t action,
                                              int16_t dx, int16_t dy,
                                              uint8_t button) {
    uint64_t packed =
        ((uint64_t)kind        & 0xFFu) |
        (((uint64_t)action     & 0xFFu) << 8) |
        (((uint64_t)(uint16_t)dx & 0xFFFFu) << 16) |
        (((uint64_t)(uint16_t)dy & 0xFFFFu) << 32) |
        (((uint64_t)button     & 0xFFu) << 48);
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_MOUSE),
                   "S"(packed)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_debug_mouse_cursor_visible(uint32_t console_id) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_MOUSE_CURSOR_VISIBLE),
                   "S"((uint64_t)console_id)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_debug_fb_read_pixel_at(uint32_t x, uint32_t y) {
    uint64_t packed = ((uint64_t)x & 0xFFFFFFFFu) |
                      (((uint64_t)y & 0xFFFFFFFFu) << 32);
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_FB_READ_PIXEL_AT),
                   "S"(packed)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only: override SYS_CONSOLE_GFX_MAP_FB's exclusive owner.
static inline long syscall_debug_fb_owner_set(int32_t pid) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_FB_OWNER_SET),
                   "S"((uint64_t)(int64_t)pid)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only: read dirty-rect render counters.
static inline long syscall_debug_dirty_rect_get_counts(uint64_t out[2]) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_DIRTY_RECT_GET_COUNTS),
                   "S"((uint64_t)out)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only: reset dirty-rect counters.
static inline long syscall_debug_dirty_rect_reset(void) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_DIRTY_RECT_RESET)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only: read a cell's codepoint from the cell VMO.
static inline long syscall_debug_console_read_cell(uint32_t console_id,
                                                    uint32_t row,
                                                    uint32_t col) {
    long ret;
    register uint64_t r10 asm("r10") = (uint64_t)col;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_CONSOLE_READ_CELL),
                   "S"((uint64_t)console_id),
                   "d"((uint64_t)row),
                   "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Block C (Stage C1) — audit log streaming. Returns subscriber
// slot id (0..15) on success or -EAGAIN if no free slot. filter_mask is a
// 64-bit bitmap of audit event types (bit N = (1ULL << N)); ~0 receives
// every event.
static inline long syscall_audit_subscribe(uint64_t filter_mask) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_AUDIT_SUBSCRIBE), "D"(filter_mask)
                 : "rcx", "r11", "memory");
    return ret;
}

// Test-only: emit a synthetic PLAN_* audit event. Used by audit_stream.tap
// + audit_plan_codes.tap so they can verify subscriber broadcast without
// wiring grahai end-to-end.
static inline long syscall_debug_audit_emit_plan(uint16_t event_type,
                                                 uint64_t plan_id) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_AUDIT_EMIT_PLAN),
                   "S"((uint64_t)event_type),
                   "d"(plan_id)
                 : "rcx", "r11", "memory");
    return ret;
}

// AUDIT_PLAN_* event codes — mirror kernel/audit.h.
#define U_AUDIT_PLAN_BEGIN          50
#define U_AUDIT_PLAN_STEP           51
#define U_AUDIT_PLAN_COMMIT         52
#define U_AUDIT_PLAN_ABORT          53
#define U_AUDIT_RLIMIT_SYSCALL_RATE 54

// FU27.X.cap_recursive_inheritance: test-only helper. Creates a fresh
// CAP_KIND_PROC cap_object with caller-specified flags + audience=
// [caller_pid] + RIGHTS_ALL, inserts into caller's cap_handle_table.
// Returns the packed cap_token_raw_t (0 on failure). Used by
// user/tests/cap_recursive_inheritance.tap.
static inline cap_token_raw_t syscall_debug_cap_create_with_flags(uint8_t flags) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_CAP_CREATE_WITH_FLAGS),
                   "S"((uint64_t)flags)
                 : "rcx", "r11", "memory");
    return (cap_token_raw_t)ret;
}

// FU27.X.cap_recursive_inheritance: test-only helper. Walks caller's
// cap_handle_table for any cap with CAP_FLAG_RECURSIVE_INHERIT set and
// checks whether caller's pid is in the audience set. Returns 0 on
// found-and-verified, 1 on not-found-or-pid-missing. Used by the child
// half of the cap_recursive_inheritance gate test.
static inline long syscall_debug_cap_check_inherited_audience(void) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_CAP_CHECK_INHERITED_AUDIENCE)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 28 Session G.1 — fault injection control plane.  Each call
// installs a new value for one of four kernel-side counters; the
// counter then drives synthetic failures from the soak harness and
// the inject_*.tap gate tests.  Spinlock injection is observe-only —
// the kernel never alters control flow on a sample, only increments
// g_debug_spinlock_injection_hits, which RESET_ALL returns.
static inline long syscall_debug_inject_pmm_fail_nth(int64_t n) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_PMM_FAIL_NTH),
                   "S"((uint64_t)n)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall_debug_inject_kmalloc_fail_nth(int64_t n) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_KMALLOC_FAIL_NTH),
                   "S"((uint64_t)n)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall_debug_inject_chan_send_fail_rate(uint32_t r) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_CHAN_SEND_FAIL_RATE),
                   "S"((uint64_t)r)
                 : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall_debug_inject_spinlock_timeout_rate(uint32_t r) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_SPINLOCK_TIMEOUT_RATE),
                   "S"((uint64_t)r)
                 : "rcx", "r11", "memory");
    return ret;
}
// Returns the prior g_debug_spinlock_injection_hits value so callers can
// confirm the sample-gated hook actually fired.
static inline long syscall_debug_inject_reset_all(void) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_DEBUG),
                   "D"((uint64_t)DEBUG_INJECT_RESET_ALL)
                 : "rcx", "r11", "memory");
    return ret;
}

// Drain up to `max` audit_entry_t records from subscriber `slot` into
// `buf`. Returns count copied (0 if empty, max 64), or -EINVAL.
static inline long syscall_audit_stream_read(int slot, void *buf, uint32_t max) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_AUDIT_STREAM_READ),
                   "D"((long)slot),
                   "S"(buf),
                   "d"((uint64_t)max)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 27 Stage C2 — manifest blob export. Returns bytes copied (>0) or
// -EINVAL. out_generation receives the FNV-1a 64-bit hash of the source
// gcp.json so AI agents can detect surface drift between kernel builds.
static inline long syscall_manifest_export(void *user_buf, uint64_t buflen,
                                           uint64_t *out_generation) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_MANIFEST_EXPORT), "D"(user_buf),
                   "S"(buflen), "d"(out_generation)
                 : "rcx", "r11", "memory");
    return ret;
}

// Pre-Phase-28 sweep C.1 (FU25.A.3 substrate): pin a grahafs version so it
// stays alive across close. Used by transactional FS-revert: if a file is
// modified inside a `txn { }` body, the pre-write version must be pinned
// so abort can revert. Returns 0 on success; negative on failure (e.g.
// version not found, or inode doesn't exist).
static inline long syscall_grahafs_pin_version(uint32_t inode_num,
                                               uint64_t version_id) {
    long ret;
    register uint64_t r_inode asm("rdi") = (uint64_t)inode_num;
    register uint64_t r_ver   asm("rsi") = version_id;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_GRAHAFS_PIN_VERSION), "r"(r_inode), "r"(r_ver)
                 : "rcx", "r11", "memory");
    return ret;
}

/* Pre-Phase-28 sweep B.3 (FU25.A.3) — path-aware, txn-aware "pin if
 * active txn, else no-op". Resolves path to inode + current version
 * chain head id, then captures a pin under the caller's active_txn
 * backing snapshot via snap_add_fs_pin. On txn_abort the kernel
 * walks fs_pins[] and calls grahafs_revert_to_version on each, so a
 * pre-write pin restores the file to its pre-modification state.
 * Gash's cmd_echo / cmd_touch call this unconditionally; the syscall
 * silently no-ops when no active txn is present, when the file
 * doesn't exist, or when the file is on the v1 (non-versioned) FS. */
static inline long syscall_txn_pin_path(const char *path) {
    long ret;
    register const char *r_path asm("rdi") = path;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_TXN_PIN_PATH), "r"(r_path)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 24 W19 (partial): snapshot lifecycle wrappers. capture machinery
// (W14.1-W14.7) lands later; for W19-partial these return -ENOSYS for
// snap_restore and a valid handle for snap_create / 0 for snap_delete /
// record count for snap_list against an empty-payload snapshot body.
static inline long syscall_snap_create(uint32_t scope_flags, const char *name) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_SNAP_CREATE), "D"((uint64_t)scope_flags), "S"(name)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_snap_restore(uint32_t handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_SNAP_RESTORE), "D"((uint64_t)handle)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_snap_delete(uint32_t handle) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_SNAP_DELETE), "D"((uint64_t)handle)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall_snap_list(snap_info_user_t *user_buf, uint64_t count) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_SNAP_LIST), "D"(user_buf), "S"(count)
                 : "rcx", "r11", "memory");
    return ret;
}

// Phase 24 sub-phase H.1: returns the calibrated TSC tick rate in Hz.
// Userspace busy-wait loops can use this with rdtsc to synthesise a
// frequency-independent delay (correct under both QEMU TCG and KVM).
// Returns 0 if the TSC has not been calibrated (very early boot only).
static inline uint64_t syscall_tsc_hz_query(void) {
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(SYS_TSC_HZ_QUERY)
                 : "rcx", "r11", "memory");
    return (uint64_t)ret;
}

// rdtsc + spin_us helpers — define here so any test that includes
// syscalls.h gets them. The first call queries g_tsc_hz once; subsequent
// calls reuse the cached value. Falls back to a calibration-based loop
// if the syscall returns 0 (extremely early boot).
static inline uint64_t spin_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Cached TSC frequency. 0 = uninitialised; on first spin_us call we
// query the kernel and cache. Static-inline keeps this out of the user
// program's binary unless something actually calls spin_us.
static inline uint64_t spin_tsc_hz(void) {
    static uint64_t cached = 0;
    if (cached == 0) {
        cached = syscall_tsc_hz_query();
        if (cached == 0) cached = 3000000000ULL;  // 3 GHz fallback
    }
    return cached;
}

// Spin for approximately `us` microseconds using rdtsc. Frequency-
// independent: works on TCG (slow rdtsc) and KVM (real rdtsc) alike.
static inline void spin_us(uint64_t us) {
    uint64_t hz = spin_tsc_hz();
    uint64_t target_ticks = (us * hz) / 1000000ULL;
    uint64_t start = spin_rdtsc();
    while (spin_rdtsc() - start < target_ticks) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static inline void spin_ms(uint64_t ms) {
    spin_us(ms * 1000ULL);
}

// Compatibility helper for tests that previously implemented their own
// `spin_ms_approx(N)` as a TCG-calibrated busy-loop (`for (i = 0; i <
// N*100000; i++) {}`). The bare loop was actually waiting ~400 µs per
// "ms unit" in TCG, NOT 1 ms.  Under KVM the cycles execute too fast,
// so the test poll budgets collapse and races surface.  This TSC-
// calibrated equivalent preserves the same observable wait-per-N
// behaviour (~ms*400 µs) under both TCG and KVM, so existing test
// poll budgets remain valid post sub-phase H.
//
// Use spin_ms(N) when you want a real-millisecond wait; use
// spin_ms_approx(N) only when porting code that calibrated against
// the old busy-loop timing.
static inline void spin_ms_approx(uint64_t ms) {
    spin_us(ms * 400ULL);
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