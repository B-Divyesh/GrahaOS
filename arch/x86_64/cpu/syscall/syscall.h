// arch/x86_64/cpu/syscall/syscall.h
#pragma once
#include <stdint.h>
#include "../interrupts.h"
#include "../smp.h"
// Define system call numbers
#define SYS_TEST 0
#define SYS_PUTC 1001

// Filesystem Syscalls
#define SYS_OPEN  1002
#define SYS_READ  1003
#define SYS_CLOSE 1004

// GCP Syscall for Phase 6b
#define SYS_GCP_EXECUTE 1005

// NEW Syscalls for Phase 6c
#define SYS_GETC 1006
#define SYS_EXEC 1007

#define SYS_EXIT 1008

#define SYS_WAIT 1009
#define SYS_WRITE  1010
#define SYS_CREATE 1011
#define SYS_MKDIR  1012
#define SYS_STAT   1013
#define SYS_READDIR 1014
#define SYS_SYNC 1015
#define SYS_BRK  1016

// Phase 7d: Modern Process Management
#define SYS_SPAWN  1017
#define SYS_KILL   1018
#define SYS_SIGNAL 1019
#define SYS_GETPID 1020

// Phase 8a: System State Introspection
#define SYS_GET_SYSTEM_STATE 1021

// Phase 8b: Capability Activation Network
#define SYS_CAP_ACTIVATE   1031
#define SYS_CAP_DEACTIVATE 1032
#define SYS_CAP_REGISTER   1033
#define SYS_CAP_UNREGISTER 1034

// Phase 8c: AI Metadata in GrahaFS
#define SYS_SET_AI_METADATA 1035
#define SYS_GET_AI_METADATA 1036
#define SYS_SEARCH_BY_TAG   1037

// Phase 8d: CAN Event Propagation
#define SYS_CAP_WATCH   1038
#define SYS_CAP_UNWATCH 1039
#define SYS_CAP_POLL    1040

// Phase 9a: Network
#define SYS_NET_IFCONFIG 1041

// Phase 9b: Network Status
#define SYS_NET_STATUS 1042

// Phase 9c: DNS + HTTP Client
#define SYS_HTTP_GET    1043
#define SYS_DNS_RESOLVE 1044

// Phase 9e: HTTP POST (for AI API calls)
#define SYS_HTTP_POST   1045

// Phase 10b: Pipes and FD duplication
#define SYS_PIPE      1046
#define SYS_DUP2      1047
#define SYS_DUP       1048
#define SYS_TRUNCATE  1049

// Phase 11a: SimHash feature extraction
#define SYS_COMPUTE_SIMHASH 1050
#define SYS_FIND_SIMILAR    1051

// Phase 11b: Sequential Leader Clustering
#define SYS_CLUSTER_LIST    1052
#define SYS_CLUSTER_MEMBERS 1053

// Phase 13: Structured logging
#define SYS_KLOG_READ       1054
#define SYS_KLOG_WRITE      1055

// Phase 13: controlled-panic / kernel-page-fault trigger for the
// harness's gate tests. Build-gated — the dispatcher only compiles
// the handler when WITH_DEBUG_SYSCALL is defined. In release builds
// the syscall number is reserved but returns -1.
#define SYS_DEBUG           1056

// Phase 14: allocator statistics for /bin/memstat and tests.
#define SYS_KHEAP_STATS     1057

// Phase 15a: Capability Objects v2. Spec listed 1057-1060; we shift up by
// one since Phase 14 consumed 1057. Every new syscall takes a cap_token_t
// (packed 64-bit {gen:32 | idx:24 | flags:8}) and returns -EREVOKED /
// -EPERM / -ENOMEM / -EFAULT on failure.
#define SYS_CAP_DERIVE      1058
#define SYS_CAP_REVOKE_V2   1059
#define SYS_CAP_GRANT       1060
#define SYS_CAP_INSPECT     1061

// Phase 15b: pledge + audit. Spec listed 1061-1062; we shift up by one
// since Phase 15a consumed 1061 (SYS_CAP_INSPECT). Per-task 16-bit
// pledge mask + persistent on-disk audit log.
#define SYS_PLEDGE          1062
#define SYS_AUDIT_QUERY     1063

// Phase 16: token-taking CAN activate/deactivate + name→token lookup. Spec
// listed 1063-1064; shifted by one since Phase 15b consumed 1063. LOOKUP is
// out-of-spec (see plan §Out-of-Spec Additions #2) — needed so /bin/can-ctl
// can resolve a cap name to a cap_token_t before calling ACTIVATE_T.
#define SYS_CAN_ACTIVATE_T   1064  // RDI = cap_token_t.raw; returns 0 or -err.
#define SYS_CAN_DEACTIVATE_T 1065  // RDI = cap_token_t.raw; returns count or -err.
#define SYS_CAN_LOOKUP       1066  // RDI = const char *name (user ptr)
                                   // RSI = size_t name_len
                                   // Returns cap_token_t.raw on success, 0 on not-found.

// Phase 17: Channels + VMOs. Syscall numbers shifted +2 from spec (1065-1072
// → 1067-1074) because Phase 15b took 1063 and Phase 16 took 1064-1066.
#define SYS_CHAN_CREATE   1067  // RDI=type_hash, RSI=mode, RDX=capacity
                                // RCX (user's 4th arg) holds a pointer to
                                // cap_token_t for the WRITE end; the READ end
                                // is returned in RAX.
#define SYS_CHAN_SEND     1068  // RDI=wr_handle, RSI=user msg ptr, RDX=timeout_ns
#define SYS_CHAN_RECV     1069  // RDI=rd_handle, RSI=user msg ptr, RDX=timeout_ns
#define SYS_CHAN_POLL     1070  // RDI=array ptr, RSI=n, RDX=timeout_ns
#define SYS_VMO_CREATE    1071  // RDI=size_bytes, RSI=flags
#define SYS_VMO_MAP       1072  // RDI=vmo_handle, RSI=addr_hint, RDX=offset,
                                //   R10=len, R8=prot
#define SYS_VMO_UNMAP     1073  // RDI=vaddr, RSI=len
#define SYS_VMO_CLONE     1074  // RDI=src_handle, RSI=flags

// Phase 18: Submission Streams. Syscall numbers shifted +2 from spec
// (1073-1076 -> 1075-1078) because Phase 17 consumed 1073-1074 for
// SYS_VMO_UNMAP / SYS_VMO_CLONE.
#define SYS_STREAM_CREATE  1075  // RDI=type_hash, RSI=sq_entries,
                                 //   RDX=cq_entries, R10=stream_handles_t *out,
                                 //   R8=notify_wr_handle_raw (0 = no notify).
#define SYS_STREAM_SUBMIT  1076  // RDI=stream_handle_raw, RSI=n_to_submit
#define SYS_STREAM_REAP    1077  // RDI=stream_handle_raw, RSI=min_complete,
                                 //   RDX=timeout_ns
#define SYS_STREAM_DESTROY 1078  // RDI=stream_handle_raw

// SYS_DEBUG sub-operations passed in RDI.
#define DEBUG_PANIC         1   // RSI = const char *reason
#define DEBUG_KERNEL_PF     2   // no args; writes to unmapped kernel addr
// Phase 14 test-assist hooks (build-gated with WITH_DEBUG_SYSCALL):
#define DEBUG_PERCPU_WRITE  44  // RSI = uint64_t val → writes per_cpu(test_slot)
#define DEBUG_PERCPU_READ   45  // no args; returns per_cpu(test_slot)
#define DEBUG_KMALLOC       46  // RSI = size, RDX = subsys; returns kernel ptr as u64
#define DEBUG_KFREE         47  // RSI = kernel ptr (u64 from DEBUG_KMALLOC)
// Phase 15a test-assist hook (build-gated with WITH_DEBUG_SYSCALL):
#define DEBUG_CAP_LOOKUP    48  // RSI = const char *name; returns packed cap_token_t
                                // for the named CAN cap's paired cap_object_t.
                                // Returns 0 on not-found.
// Phase 15b test-assist hooks:
#define DEBUG_READ_PLEDGE   50  // no args; returns current->pledge_mask.raw
                                // as a uint16_t (cast to uint64_t).
#define DEBUG_SET_WALL      51  // RSI = int64_t new_boot_wall_seconds.
                                // Overwrites g_boot_wall_seconds for log-rotation
                                // tests; returns the old value.
// Phase 16 test-assist hooks (build-gated with WITH_DEBUG_SYSCALL):
#define DEBUG_PIC_READ_MASK   60  // RSI = uint8_t line; returns 1 if masked, 0 if not.
#define DEBUG_FB_READ_PIXEL   61  // RSI = uint32_t x, RDX = uint32_t y; returns raw u32.
#define DEBUG_AHCI_PORT_CMD   62  // RSI = int port_num; returns port->cmd.
#define DEBUG_E1000_READ_REG  63  // RSI = uint32_t offset; returns register value.
#define DEBUG_KB_IS_ACTIVE    64  // no args; returns 1/0.
#define DEBUG_FB_IS_ACTIVE    65  // no args; returns 1/0.
#define DEBUG_E1000_IS_ACTIVE 66  // no args; returns 1/0.
#define DEBUG_AHCI_IS_ACTIVE  67  // no args; returns 1/0.

void syscall_init(void);
void syscall_dispatcher(struct syscall_frame *frame);

