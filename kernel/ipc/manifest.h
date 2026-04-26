// kernel/ipc/manifest.h — Phase 17.
//
// A runtime-populated registry of GCP manifest type names and their FNV-1a
// 64-bit hashes. Every channel is anchored to exactly one type_hash; sends
// whose header.type_hash does not match are rejected with -EPROTOTYPE. This
// file is the in-kernel authority for which type hashes are "known". Phase
// 18's scripts/gen_manifest.py will later emit /etc/gcp.json from the same
// table.
//
// Userspace computes the same hashes at program start via its own FNV-1a
// helper (user/syscalls.h exposes `gcp_type_hash(const char *)`); both sides
// agree because the algorithm is deterministic.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Manifest-type name strings. Canonical values — keep in sync with
// userspace's g_gcp_type_names[] if that ever gets mirrored.
#define MANIFEST_NAME_NOTIFY_V1      "grahaos.notify.v1"
#define MANIFEST_NAME_PIPE_BYTES_V1  "grahaos.pipe.bytes.v1"
#define MANIFEST_NAME_FAULT_V1       "grahaos.fault.v1"
#define MANIFEST_NAME_SHUTDOWN_V1    "grahaos.shutdown.v1"
#define MANIFEST_NAME_AUDIT_V1       "grahaos.audit.v1"
#define MANIFEST_NAME_TEST_V1        "grahaos.test.v1"
// Phase 18: stream completion notifications (channel payload type).
#define MANIFEST_NAME_IO_COMPLETION_V1 "grahaos.io.completion.v1"

// Phase 22: named channel registry + netd service + raw ethernet bridge.
#define MANIFEST_NAME_NET_ACCEPT_V1    "grahaos.net.accept.v1"   // publisher accept channel
#define MANIFEST_NAME_NET_FRAME_V1     "grahaos.net.frame.v1"    // per-conn rawframe data
#define MANIFEST_NAME_NET_SOCKET_V1    "grahaos.net.socket.v1"   // per-conn service data

// Phase 23: ahcid block-I/O service + diagnostic channel types.
#define MANIFEST_NAME_BLK_SERVICE_V1   "grahaos.blk.service.v1"  // ahcid per-client RPC
#define MANIFEST_NAME_BLK_LIST_V1      "grahaos.blk.list.v1"     // blkctl diagnostic

// Initialise the type-hash table. Called once from kmain after cap_object_init.
void manifest_init(void);

// Is this hash registered? Linear scan, ≤ 10 entries.
bool manifest_type_known(uint64_t type_hash);

// Fast accessor: hash for a well-known name (populated by manifest_init).
// Callable only after manifest_init. Returns 0 for unknown names (which is
// itself not a valid hash for any real string).
uint64_t manifest_hash_notify_v1(void);
uint64_t manifest_hash_pipe_bytes_v1(void);
uint64_t manifest_hash_fault_v1(void);
uint64_t manifest_hash_shutdown_v1(void);
uint64_t manifest_hash_audit_v1(void);
uint64_t manifest_hash_test_v1(void);
uint64_t manifest_hash_net_accept_v1(void);
uint64_t manifest_hash_net_frame_v1(void);
uint64_t manifest_hash_net_socket_v1(void);
uint64_t manifest_hash_blk_service_v1(void);
uint64_t manifest_hash_blk_list_v1(void);

// Lazy-compute helper reusing kernel/fs/simhash.c's fnv1a_hash64. Callable
// from any context.
uint64_t manifest_compute_hash(const char *type_name);
