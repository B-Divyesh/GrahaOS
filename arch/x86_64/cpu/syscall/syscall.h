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

// Phase 24 W19: snapshot syscalls. Spec originally proposed 1086-1089 but
// those slots were already taken (SPAWN_EX, DRV_REGISTER, DRV_IRQ_WAIT,
// MMIO_VMO_CREATE) — plan reconciled to 1093-1096.
#define SYS_SNAP_CREATE   1093  // RDI = uint32_t scope_flags (SNAP_SCOPE_*)
                                //   RSI = const char *name (≤31 chars + NUL,
                                //         user ptr; NULL permitted).
                                // Pledge: SYS_CONTROL + SYS_QUERY.
                                // Returns: cap_handle slot >= 0 on success,
                                //   negative -errno (-EINVAL, -EPERM, -ENOMEM,
                                //   -EBUSY, -ETIME).
#define SYS_SNAP_RESTORE  1094  // RDI = uint32_t handle.
                                // Pledge: SYS_CONTROL.
                                // Returns 0 on success, -EINVAL, -EPERM,
                                //   -ESTALE, -ETIME. (W14 partial: -ENOSYS
                                //   until W16 lands the restore body.)
#define SYS_SNAP_DELETE   1095  // RDI = uint32_t handle.
                                // Pledge: SYS_CONTROL.
                                // Returns 0 on success, -EINVAL, -EPERM,
                                //   -EBUSY (snapshot is RESTORING).
#define SYS_SNAP_LIST     1096  // RDI = snap_info_t *user_buf
                                //   RSI = size_t count.
                                // Pledge: SYS_QUERY.
                                // Returns count of snap_info_t records
                                //   written, or -EINVAL / -EPERM.

// Phase 24 sub-phase H.1: TSC frequency query.
#define SYS_TSC_HZ_QUERY  1097  // No args.  No pledge required.
                                // Returns g_tsc_hz (in Hz) or 0 if the
                                // TSC has not been calibrated yet.

// Phase 25 transactional speculation. Slots 1098-1100 (1090-1097 are
// reserved by Phase 22-24 syscalls). SYS_TXN_BEGIN allocates an implicit
// snapshot via snap_create_internal + pushes a transaction frame on the
// caller's task. chan_send while a txn is active is intercepted into the
// txn's per-txn buffer (Stage E lands the interception). SYS_TXN_COMMIT
// replays buffered external sends in original order then discards the
// snapshot; SYS_TXN_ABORT drops the buffer + restores the snapshot.
#define SYS_TXN_BEGIN     1098  // RDI = uint32_t flags (TXN_FLAG_*),
                                //   RSI = const char *name (≤31 + NUL).
                                // Pledge: SYS_CONTROL.
                                // Returns: cap_handle slot >= 0 on success,
                                //   negative -errno (-EINVAL, -EPERM,
                                //   -ENOMEM, -ENESTED, -EBUSY).
#define SYS_TXN_COMMIT    1099  // RDI = uint32_t handle.
                                // Pledge: SYS_CONTROL.
                                // Returns: 0 on full commit; -ETXNREPLAY
                                //   if replay stalled mid-sequence (caller
                                //   may retry or abort); -EINVAL/-EPERM/
                                //   -EBUSY otherwise.
#define SYS_TXN_ABORT     1100  // RDI = uint32_t handle.
                                // Pledge: SYS_CONTROL.
                                // Returns: 0 on successful abort; -EINVAL,
                                //   -EPERM, -ESTALE.

// =============================================================================
// Phase 27 syscalls — TUI Framework + Graphics + AI Primitives (slots 1101-1112).
//
// Block A (TUI Framework): 1101 SWITCH, 1102 CREATE, 1103 ATTACH, 1104 INSPECT,
//                          1105 OBSERVE, 1106 ACK_RENDER.
// Block B (Graphics):     1107 SPRITE_REGISTER, 1108 GFX_ENABLE, 1109 GFX_DAMAGE.
// Block C (AI Primitives):1110 AUDIT_SUBSCRIBE, (1111 reserved), 1112 MANIFEST_EXPORT.
//
// Original Phase 27 spec proposed 1093/1094 — those collide with Phase 24
// SYS_SNAP_CREATE/RESTORE. Reassigned 1101-1112 per phase-27-mega.yml.
// =============================================================================
#define SYS_CONSOLE_SWITCH         1101  // RDI = uint32_t console_id (0..NUM_CONSOLES-1)
                                         // Pledge: IPC_SEND. Cap: CAP_KIND_SYSTEM RIGHT_INVOKE.
                                         // Returns: 0 on success, -EINVAL/-EPERM.
#define SYS_CONSOLE_CREATE         1102  // RDI = uint32_t width_cells (0=default)
                                         // RSI = uint32_t height_cells (0=default)
                                         // RDX = console_info_t *out (user ptr)
                                         // Pledge: SYS_CONTROL. Cap: CAP_KIND_SYSTEM.
                                         // Returns: cap_handle ≥ 0, or -EINVAL/-EPERM/-ENOMEM/-E2BIG.
#define SYS_CONSOLE_ATTACH         1103  // RDI = uint32_t console_id
                                         // RSI = uint64_t cap_token (CAP_KIND_CONSOLE w/ RIGHT_ATTACH)
                                         // Pledge: IPC_RECV.
                                         // Returns: 0 on success, -EBUSY/-EPERM/-EINVAL.
#define SYS_CONSOLE_INSPECT        1104  // RDI = uint32_t console_id
                                         // RSI = void *out_buf (user ptr to W*H*16 bytes)
                                         // RDX = size_t buflen
                                         // RCX(R10) = uint64_t cap_token (CAP_KIND_CONSOLE w/ RIGHT_INSPECT)
                                         // Pledge: SYS_QUERY.
                                         // Returns: bytes copied, or -EFAULT/-EPERM/-EINVAL.
#define SYS_CONSOLE_OBSERVE        1105  // RDI = uint32_t console_id
                                         // RSI = uint64_t cap_token (CAP_KIND_CONSOLE w/ RIGHT_OBSERVE)
                                         // RDX = uint64_t *out_chan_token (user ptr)
                                         // Pledge: IPC_RECV.
                                         // Returns: 0 on success, -EPERM/-EINVAL/-ENOMEM.
#define SYS_CONSOLE_ACK_RENDER     1106  // RDI = uint32_t console_id
                                         // RSI = uint64_t rendered_seq
                                         // Pledge: IPC_SEND. Cap: CAP_KIND_SYSTEM RIGHT_INVOKE.
                                         // Returns: 0 on success, -EINVAL/-EPERM. Sets fbd_alive=true on
                                         // first call. Used by fbd userspace daemon.
#define SYS_CONSOLE_SPRITE_REGISTER 1107 // RDI = uint64_t cap_token (CAP_KIND_CONSOLE w/ RIGHT_WRITE)
                                         // RSI = uint32_t sprite_id (0..NUM_SPRITES-1)
                                         // RDX = const uint8_t *bitmap16 (user ptr; 16 bytes)
                                         // Pledge: SYS_CONTROL.
                                         // Block B (Stage B1) — A2 stub returns -ENOSYS.
#define SYS_CONSOLE_GFX_ENABLE     1108  // RDI = uint64_t cap_token, RSI = w_px, RDX = h_px,
                                         // R10 = uint64_t *out_vmo_handle. Block B (Stage B1) — A2 stub.
#define SYS_CONSOLE_GFX_DAMAGE     1109  // RDI = uint64_t cap_token, RSI = damage_rect_t *.
                                         // Block B (Stage B1) — A2 stub.
#define SYS_AUDIT_SUBSCRIBE        1110  // RDI = uint64_t filter_mask, returns slot id (>=0) or -EAGAIN
#define SYS_AUDIT_STREAM_READ      1111  // RDI = int slot, RSI = audit_entry_t *buf, RDX = uint32_t max
                                         // RDX = uint64_t *out_chan_token
                                         // Pledge: SYS_QUERY, IPC_RECV.
                                         // Block C (Stage C1) — A2 stub returns -ENOSYS.
// 1111 reserved for SYS_AUDIT_UNSUBSCRIBE or SYS_AUDIT_FILTER_UPDATE (FU27.X).
#define SYS_MANIFEST_EXPORT        1112  // RDI = char *user_buf, RSI = size_t buflen,
                                         // RDX = uint32_t *out_generation.
                                         // Pledge: SYS_QUERY.
                                         // Block C (Stage C2) — A2 stub returns -ENOSYS.

// Pre-Phase-28 sweep C.1 (FU25.A.3 substrate) — SYS_GRAHAFS_PIN_VERSION
// exposes kernel/fs/grahafs_v2.c::grahafs_pin_version to userspace. This
// is the substrate piece for transactional FS-revert: gash's
// `txn { echo > X } abort` semantics need the per-FD pin to retain a
// pre-write version across close. Pledge: FS_WRITE (the pin holds a
// version alive, equivalent to write-side bookkeeping).
#define SYS_GRAHAFS_PIN_VERSION    1113  // RDI = uint32_t inode_num, RSI = uint64_t version_id
                                         // returns 0 on success, negative -CAP_V2_* on failure.

// Pre-Phase-28 sweep B.3 (FU25.A.3 wiring) — SYS_TXN_PIN_PATH wraps a
// path resolution + version_chain_head_id lookup + snap_add_fs_pin call
// into one syscall, so gash's cmd_echo / cmd_touch can request a pin
// without inode/version awareness. No-op (returns 0) when the caller
// has no active txn — gash calls it unconditionally and gets the
// "txn-aware" semantic only when running inside a txn body. Pledge:
// FS_WRITE (it modifies how subsequent FS writes are revertable).
#define SYS_TXN_PIN_PATH           1114  // RDI = const char *path
                                         // returns 0 on success / no-active-txn,
                                         // negative on resolution / pin failure.

// Phase 29 Session C (FU28.B): SYS_SPAWN_ARGV — spawn a child AND pass
// argv. The existing SYS_SPAWN (1017) and SYS_SPAWN_EX (1086) do not
// propagate argv, so the child's _start sees argc=0 (or, for legacy
// `_start(void)` binaries, simply ignores RDI/RSI). gsh's `wasm run
// <file>` and similar tooling need argv to reach the child.
//
// ABI (RDI=path, RSI=argv, RDX=argc):
//   RDI = const char *path             (user ptr, NUL-terminated, max 255 B)
//   RSI = char *const *argv            (user ptr to array of char* ptrs;
//                                        no NUL sentinel required — kernel
//                                        treats argc as authoritative)
//   RDX = uint32_t argc                (bounded to SYS_SPAWN_ARGV_MAX = 16;
//                                        argc == 0 is legal and equivalent
//                                        to SYS_SPAWN)
//
// Each argv[i] is copied into a kernel-side scratch buffer (max
// SYS_SPAWN_ARGV_STRBYTES = 4096 bytes total). The kernel then maps the
// child's top user-stack page via HHDM, writes a packed layout
// (argv strings + array of char* pointers in user-virtual coords),
// and seeds the child's regs.rdi = argc, regs.rsi = argv_user_ptr,
// regs.rsp = user_stack_top - 8 (preserves the System V alignment
// invariant SYS_SPAWN already relies on).
//
// Pledge: SPAWN (same as SYS_SPAWN).
// Returns: child pid (>0) on success, or -EFAULT / -EINVAL / -E2BIG /
//          -ENOMEM / -EPLEDGE.
#define SYS_SPAWN_ARGV             1115  // RDI = path, RSI = argv, RDX = argc

// Phase 29 Session D — TUI primitives (slots 1116-1118).  SYS_CONSOLE_ATTACH
// (1103) is also wired for real in this session (was -ENOSYS).
//
// SYS_CONSOLE_READ_INPUT — drain console input chan non-blocking.
//   RDI = uint32_t console_id
//   RSI = input_event_t *out (user ptr)
//   RDX = uint32_t max_events
// Returns the count copied; if more events are still queued, the high bit
// (bit 62, 0x4000_0000_0000_0000) is set so the value stays positive.
// Pledge: IPC_RECV.  Errors: -EINVAL / -EFAULT.
#define SYS_CONSOLE_READ_INPUT     1116
// SYS_CONSOLE_GFX_MAP_FB — mint a VMO_MMIO handle backed by the hardware
// framebuffer's physical address range.
//   RDI = uint64_t *out_handle_raw (user ptr)
//   RSI = fb_dims_t *out_dims      (user ptr, 24 B)
// Returns 0 on success.  First caller wins exclusive write; subsequent
// callers get -EPERM unless they're the recorded owner.
// Pledge: SYS_CONTROL.  Errors: -EFAULT / -EPERM / -ENOMEM.
#define SYS_CONSOLE_GFX_MAP_FB     1117
// SYS_CONSOLE_VSYNC_WAIT — block until the next 60 Hz tick or max_wait_ns.
//   RDI = uint64_t max_wait_ns (0 = wait indefinitely)
// Returns 0 on tick, -ETIME on timeout.  Pledge: IPC_RECV.
#define SYS_CONSOLE_VSYNC_WAIT     1118

// Phase 29 Session E — sprite animation + cell-grid atomic TX (slots
// 1119-1122).
//
// SYS_CONSOLE_SPRITE_ANIMATE — register N keyframes for a sprite + total
// duration; the kernel's 60Hz timer steps frame.  Auto-commits at end.
//   RDI = uint32_t console_id
//   RSI = uint32_t sprite_id  (target sprite slot in the console's registry)
//   RDX = const tui_cell_t *keyframes (user ptr to array of n_frames cells)
//   R10 = uint16_t n_frames   (1..64)
//   R8  = uint32_t duration_ms_per_frame (1..60000)
// Returns animation slot (>= 0) on success, or -EINVAL / -ENOMEM / -EPERM.
// Pledge: SYS_CONTROL.
#define SYS_CONSOLE_SPRITE_ANIMATE 1119

// SYS_CONSOLE_BEGIN_TX — snapshot the console's cell-VMO via COW; future
// userspace writes to the mapped cell-VMO go to the shadow page.  Atomic
// flip on commit.
//   RDI = uint32_t console_id
// Returns tx_handle (>=1) on success, or -EBUSY (a TX already active on
// this console for this caller) / -ENOMEM / -EINVAL / -EPERM.
// Pledge: SYS_CONTROL.
#define SYS_CONSOLE_BEGIN_TX       1120

// SYS_CONSOLE_COMMIT_TX — atomic pointer swap, replace live cell-VMO with
// the shadow.  Old live is freed.  TX handle is consumed.
//   RDI = uint32_t tx_handle
// Returns 0 on success, -EINVAL on bad handle.  Pledge: SYS_CONTROL.
#define SYS_CONSOLE_COMMIT_TX      1121

// SYS_CONSOLE_ABORT_TX — drop the shadow + restore userspace mapping to
// live cell-VMO.  Emits AUDIT_TUI_TX_ABORT (code 59).
//   RDI = uint32_t tx_handle
// Returns 0 on success, -EINVAL on bad handle.  Pledge: SYS_CONTROL.
#define SYS_CONSOLE_ABORT_TX       1122

// Phase 29 Session I (FU24.E): set CPU affinity for a task.
//   RDI = int32_t pid       (target task; -1 / 0 → self)
//   RSI = uint32_t mask     (lowest set bit = pinned CPU; 0xFFFFFFFFu = unpin)
// Returns 0 on success, -EINVAL on bad args, -EPERM if target_pid != self and
// caller lacks SYS_CONTROL.  Pledge: SYS_CONTROL unless target_pid == self.
#define SYS_SET_CPU_AFFINITY       1123

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
// FU26.C (Phase 26 closeout): kernel ksnprintf reachable from user/tests.
//   RSI = const char *fmt (user, NUL-term, max 255)
//   RDX = uint64_t arg1
//   R10 = uint64_t arg2
//   R8  = char *out (user, must be 256 bytes)
// Returns vsnprintf-convention byte count or -EFAULT on copy failure.
#define DEBUG_VSNPRINTF       68
// Phase 27 Block A (Stage A3): inject a single PS/2 scancode into the
// keyboard ISR path so user/tests/keyboard_alt.c can exercise Alt+N detection
// without poking real hardware. Returns 0; no error path.
#define DEBUG_INJECT_SCANCODE 69  // RSI = uint8_t scancode
// Phase 27 Block A (Stage A3): query console_table.selected so user-tests
// can verify Alt+N detection actually switched the displayed console.
#define DEBUG_CONSOLE_GET_SELECTED 70  // no args; returns selected console ID
// Phase 27 Block A (Stage A4): test-only direct cell-VMO write so the
// fbd_render.tap gate test can populate a console without owning it
// (SYS_CONSOLE_ATTACH is -ENOSYS until Stage C2 lands cap inheritance).
//   RSI = uint32_t console_id
//   RDX = uint32_t row
//   R10 = uint32_t col
//   R8  = uint64_t packed (codepoint:32 | fg:8 | bg:8 | attrs:16)
// Returns 0 / -1.
#define DEBUG_CONSOLE_WRITE_CELL   71
// Phase 27 Block A (Stage A4): trigger a kernel-side composite of the named
// console's cell-VMO into the hardware framebuffer. Bypasses g_fbd_alive
// (force_draw_cell). Used by fbd_render.tap to validate the cell→pixel path
// without spawning fbd in autorun=ktest.
//   RSI = uint32_t console_id
// Returns 0 / -1.
#define DEBUG_CONSOLE_SYNTHETIC_RENDER 72
// Phase 27 Block B (Stage B1): test-only fill of a rectangle in a console's
// gfx_overlay buffer with a known ARGB color. Used by console_gfx.tap so the
// test doesn't need the full vmo_map plumbing to write overlay pixels.
//   RSI = uint32_t console_id
//   RDX = uint64_t (packed: x:16 | y:16 | w:16 | h:16)
//   R10 = uint32_t color (ARGB)
// Returns 0 / -1.
#define DEBUG_CONSOLE_GFX_FILL 73
// Phase 27 Block C (Stage C1): test-only emission of a synthetic audit event
// (PLAN_BEGIN / PLAN_STEP / PLAN_COMMIT / PLAN_ABORT) so audit_stream + audit_plan_codes
// gates can verify the broadcast path without wiring grahai end-to-end.
//   RSI = uint16_t event_type
//   RDX = uint64_t plan_id
// Returns 0.
#define DEBUG_AUDIT_EMIT_PLAN 74
// FU27.X.cap_recursive_inheritance: TEST-ONLY helpers paired with the
// user-side wrappers in user/syscalls.h. CAP_CREATE_WITH_FLAGS allocates
// a cap_object with caller-specified flags + audience=[caller_pid] +
// RIGHTS_ALL, inserts into caller's cap_handle_table, returns packed
// cap_token (0 on failure).
//   RSI = uint8_t flags  Returns: cap_token_raw_t (0 on failure)
// CAP_CHECK_INHERITED_AUDIENCE walks caller's handle table looking for a
// cap with CAP_FLAG_RECURSIVE_INHERIT set; returns 0 if found AND
// caller_pid is in its audience set, 1 otherwise. Used by the child half
// of the cap_recursive_inheritance gate test.
#define DEBUG_CAP_CREATE_WITH_FLAGS         75
#define DEBUG_CAP_CHECK_INHERITED_AUDIENCE  76
// Phase 28 Session G.1 fault injection subops.  RSI carries the new
// counter value; RAX returns 0 (or the prior hits count for RESET_ALL).
//   PMM_FAIL_NTH (80)     : RSI = int64_t  countdown (0 = disabled)
//   KMALLOC_FAIL_NTH (81) : RSI = int64_t  countdown (0 = disabled)
//   CHAN_SEND_FAIL_RATE (82)    : RSI = uint32_t (0 = disabled, >0 = sample on)
//   SPINLOCK_TIMEOUT_RATE (83)  : RSI = uint32_t (0 = disabled, >0 = sample on)
//   RESET_ALL (84)        : zero all counters; RAX = prior spinlock_injection_hits
#define DEBUG_INJECT_PMM_FAIL_NTH           80
#define DEBUG_INJECT_KMALLOC_FAIL_NTH       81
#define DEBUG_INJECT_CHAN_SEND_FAIL_RATE    82
#define DEBUG_INJECT_SPINLOCK_TIMEOUT_RATE  83
#define DEBUG_INJECT_RESET_ALL              84
// Phase 29 Session C (FU25.H): expose current task's handle table count
// so spawn_handles_inherit.tap can verify that handles_to_inherit added
// the expected number of entries to the child's table.  No args; returns
// caller's cap_handles.count (uint32 cast to uint64).
#define DEBUG_HANDLE_COUNT                  85
// Phase 29 Session D — TUI test substrate.
//   FB_OWNER_SET (86):  RSI = int32_t pid (or -1 to clear).  Test-only
//     override of the SYS_CONSOLE_GFX_MAP_FB exclusive-owner pid so
//     fb_mmio_map.tap can simulate the fbd ownership handshake without
//     spawning fbd.  Returns the prior owner pid.
//   DIRTY_RECT_GET_COUNTS (87):  RSI = uint64_t *out (user ptr to two
//     uint64_t: [partial, full]).  Reads the global dirty-rect render
//     counters atomic.  Returns 0.
//   DIRTY_RECT_RESET (88):  no args.  Zero both counters.  Returns 0.
//   CONSOLE_READ_CELL (89):  RSI = uint32_t console_id, RDX = uint32_t row,
//     R10 = uint32_t col.  Returns the cell's codepoint OR -1 on error.
//     Test-only readback for console_attach_map.tap.
#define DEBUG_FB_OWNER_SET                  86
#define DEBUG_DIRTY_RECT_GET_COUNTS         87
#define DEBUG_DIRTY_RECT_RESET              88
#define DEBUG_CONSOLE_READ_CELL             89
// Phase 29 Session E — TUI animation + mouse + cell-grid TX test substrate.
//   ANIM_TICK (90):  RSI = uint32_t console_id.  Force-ticks the animation
//     scheduler for that console once (advances any active animation by 1
//     frame, ignoring real-time delta).  Returns 0.
//   ANIM_GET_FRAME (91): RSI = uint32_t console_id, RDX = uint32_t anim_slot.
//     Returns cur_frame OR -1 if slot not active.
//   ANIM_GET_STATE (92): RSI = uint32_t console_id, RDX = uint32_t anim_slot.
//     Returns animation state (0=idle/1=running/2=committed/3=aborted), or -1.
//   INJECT_MOUSE (93): RSI = uint64_t packed{kind:8|action:8|dx:16|dy:16|btn:8|_:8},
//     synthesises a mouse event onto the selected console's input ring.
//     Returns 0.
//   FB_READ_PIXEL_AT (94): RSI = uint64_t packed{x:32|y:32}.  Returns raw
//     framebuffer pixel; used by font_full_sweep.tap.
//   MOUSE_CURSOR_VISIBLE (95): RSI = uint32_t console_id.  Returns 1 if
//     mouse cursor sprite is currently visible on that console, 0 else.
#define DEBUG_ANIM_TICK                     90
#define DEBUG_ANIM_GET_FRAME                91
#define DEBUG_ANIM_GET_STATE                92
#define DEBUG_INJECT_MOUSE                  93
#define DEBUG_FB_READ_PIXEL_AT              94
#define DEBUG_MOUSE_CURSOR_VISIBLE          95

void syscall_init(void);
void syscall_dispatcher(struct syscall_frame *frame);

