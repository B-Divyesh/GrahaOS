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

// Phase 19: GrahaFS v2 — versioned segments + full data journaling.
// Spec listed 1077-1080; shifted +2 because Phase 18 consumed 1077-1078 for
// SYS_STREAM_REAP / SYS_STREAM_DESTROY. Out-of-spec addition #1.
#define SYS_FS_SNAPSHOT       1079  // RDI = const char *label (user ptr, may be NULL)
                                    //   RSI = label_len (0..64)
                                    // Returns: cap_token_raw_t for new
                                    //   CAP_KIND_FS_SNAPSHOT, or -errno.
#define SYS_FS_LIST_VERSIONS  1080  // RDI = uint32_t inode_num
                                    //   RSI = fs_version_info_t *out (user ptr)
                                    //   RDX = uint32_t max_count (1..64)
                                    // Returns count written (0..max_count), or -errno.
#define SYS_FS_REVERT         1081  // RDI = uint32_t inode_num
                                    //   RSI = uint64_t target_version_id
                                    // Returns new version_id (uint64_t cast), or -errno.
#define SYS_FS_GC_NOW         1082  // No args.
                                    // Returns count of versions pruned, or -errno.
#define SYS_FSYNC             1083  // RDI = int fd
                                    // Force-commits any pending journal txn for
                                    // the underlying node and issues a disk
                                    // flush. Returns 0 or -errno.

// Phase 20: resource limits.
// Spec listed 1081-1082; shifted +3 because Phase 19 consumed 1081-1083
// for SYS_FS_REVERT / SYS_FS_GC_NOW / SYS_FSYNC. Out-of-spec addition #1.
#define SYS_SETRLIMIT         1084  // RDI = uint32_t pid (0 = self)
                                    //   RSI = uint32_t resource (RLIMIT_MEM/CPU/IO)
                                    //   RDX = uint64_t value
                                    // Requires PLEDGE_SYS_CONTROL even for self
                                    // (limits are not self-manageable). Returns
                                    // 0 on success, -ESRCH / -EINVAL / -EPLEDGE.
#define SYS_GETRLIMIT         1085  // RDI = uint32_t pid (0 = self)
                                    //   RSI = uint32_t resource
                                    //   RDX = uint64_t *value_out (user ptr)
                                    // Requires PLEDGE_SYS_QUERY (default-granted).
                                    // Returns 0 on success, writes limit to
                                    // *value_out; -ESRCH / -EINVAL / -EFAULT.
#define SYS_SPAWN_EX          1086  // RDI = const char *path
                                    //   RSI = const spawn_rlimits_t *attrs (user ptr, may be NULL)
                                    // Default inheritance (attrs==NULL): same as SYS_SPAWN.
                                    // With SPAWN_ATTR_HAS_RLIMIT set in attrs.flags, caller
                                    // MUST hold PLEDGE_SYS_CONTROL. Returns child pid or
                                    // -EAGAIN / -EPLEDGE / -EFAULT.

// Phase 21: userspace driver framework + E1000 migration.
// Spec listed 1083-1085; shifted +4 because Phase 19/20 consumed 1083-1086.
// Out-of-spec addition #1 (same drift pattern as prior phases).
#define SYS_DRV_REGISTER      1087  // RDI = uint16_t vendor_id
                                    //   RSI = uint16_t device_id
                                    //   RDX = uint8_t  device_class (PCI class)
                                    //   R10 = struct drv_caps *out_caps (user ptr)
                                    // Pledge: SYS_CONTROL + class-specific
                                    // (NET_SERVER for NICs, STORAGE_SERVER for AHCI).
                                    // Cap: CAP_KIND_DRIVER_REGISTRAR token in caller's
                                    // handle table (slot 0 by convention, granted by init).
                                    // Returns 0, fills out_caps; -ENODEV/-EBUSY/-EPLEDGE/
                                    // -EFAULT/-ENOMEM.
#define SYS_DRV_IRQ_WAIT      1088  // RDI = uint32_t irq_channel_handle (cap_token raw)
                                    //   RSI = struct drv_irq_msg *out_msgs (user ptr)
                                    //   RDX = uint32_t max_msgs (1..64; capped at 64)
                                    //   R10 = uint32_t timeout_ms (0=poll, UINT32_MAX=block)
                                    // Pledge: SYS_CONTROL.
                                    // Cap: CAP_KIND_IRQ_CHANNEL.
                                    // Returns N copied (0..max_msgs), 0 on timeout, or
                                    // -EBADF/-EFAULT/-ESHUTDOWN/-EPLEDGE.
#define SYS_MMIO_VMO_CREATE   1089  // RDI = uint64_t op (0=CREATE, 1=PHYS_QUERY)
                                    // op=CREATE: RSI=phys_addr, RDX=size, R10=flags
                                    //   Returns vmo cap_token raw or -EACCES/-EINVAL/
                                    //   -ENOMEM/-EPLEDGE.
                                    // op=PHYS_QUERY: RSI=vmo_handle, RDX=page_index,
                                    //   R10=uint64_t *phys_out
                                    //   Returns 0 (writes phys_out), or -EBADF/-EINVAL.
                                    // Pledge: SYS_CONTROL. Cap: CAP_KIND_DRIVER_REGISTRAR.
#define SYS_DEBUG_INJECT_PCI  1090  // RDI = const struct pci_inject *desc (user ptr)
                                    // Debug-only: gated by WITH_DEBUG_SYSCALL build flag
                                    // AND cmdline test_mode=1. Adds a fake PCI entry to
                                    // g_pci_table for the test harness's deterministic
                                    // EBUSY/ENODEV/EACCES tests. Returns the new entry
                                    // index (0..g_pci_table_count-1), or -EPERM if not
                                    // built/enabled, or -ENOSPC if table full.

// Phase 22: named channel registry (backed by kernel/net/rawnet.c). Enables
// `chan_connect("/sys/net/service")`-style service lookup that the spec
// (AW-22.1 / AW-22.2) assumes but that Phase 17 did not provide. The five
// Mongoose-era syscalls (1041-1045) are retired in Phase 22's Stage F and
// return -ENOSYS + AUDIT_DEPRECATED_SYSCALL thereafter. Net delta: -3 syscalls.
#define SYS_CHAN_PUBLISH      1091  // RDI = const char *name (user ptr)
                                    //   RSI = uint32_t name_len
                                    //   RDX = uint64_t payload_type_hash (FNV-1a)
                                    //   R10 = cap_token_raw_t accept_channel_write_end
                                    // Pledge: IPC_SEND | IPC_RECV. No cap
                                    // requirement beyond endpoint ownership.
                                    // Returns 0 on success, -EPERM (name already
                                    //   owned by different pid), -EINVAL (bad
                                    //   name or unknown type_hash), -ENOMEM
                                    //   (registry full), -EPROTOTYPE (hash not
                                    //   in the manifest).
#define SYS_CHAN_CONNECT      1092  // RDI = const char *name (user ptr)
                                    //   RSI = uint32_t name_len
                                    //   RDX = cap_token_u_t *out_wr_req   (user ptr)
                                    //   R10 = cap_token_u_t *out_rd_resp  (user ptr)
                                    // Pledge: IPC_SEND | IPC_RECV.
                                    // On success, a fresh request channel (client
                                    //   writes, publisher reads) and response
                                    //   channel (publisher writes, client reads)
                                    //   are allocated; the client-side tokens
                                    //   are written to out_wr_req / out_rd_resp;
                                    //   the server-side tokens are delivered to
                                    //   the publisher via the accept channel it
                                    //   registered at publish time.
                                    // Returns 0, -EBADF (no such name),
                                    //   -EINVAL (bad name), -ENOMEM, -EFAULT
                                    //   (bad user pointer), -EPIPE (publisher
                                    //   has died and cleanup has not yet run).

// Resource identifiers for SYS_SETRLIMIT / SYS_GETRLIMIT.
#define RLIMIT_MEM            1     // pages (4 KiB each); 0 = unlimited
#define RLIMIT_CPU            2     // ns per 1-second epoch (max 1_000_000_000); 0 = unlimited
#define RLIMIT_IO             3     // bytes per second through stream submit; 0 = unlimited

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

