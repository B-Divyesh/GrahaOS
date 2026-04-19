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

// Initialise the type-hash table. Called once from kmain after cap_object_init.
void manifest_init(void);

// Is this hash registered? Linear scan, ≤ 6 entries.
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

// Lazy-compute helper reusing kernel/fs/simhash.c's fnv1a_hash64. Callable
// from any context.
uint64_t manifest_compute_hash(const char *type_name);
