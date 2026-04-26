// kernel/net/rawnet.h — Phase 22.
//
// Named-channel registry + per-connection bring-up. The entire Phase 22 kernel
// contribution (apart from the five legacy-syscall deletions) lives behind
// this header. It replaces the ~24 kLOC of Mongoose + net.c + klib + kmalloc
// with ~450 LOC that simply lets one userspace daemon PUBLISH a connection
// endpoint under a well-known ASCII name, and lets any other userspace process
// CONNECT to that name and receive a fresh bidirectional channel-pair routed
// to the publisher.
//
// Conceptually this is the "named service" primitive the Phase 22 spec
// (AW-22.1, AW-22.2) assumes but that did not exist in tree. Naming it
// `rawnet` preserves the spec's file name and nods at the fact that the
// primary client is the raw-Ethernet-frame bridge between `/bin/e1000d` and
// `/bin/netd`. The registry is deliberately generic — Phase 23 AHCI (+Phase
// 28 gsh) will reuse it unchanged.
//
// Design
// ------
// Each `rawnet_publish_entry_t` records (name, publisher_pid, payload
// type-hash, accept-channel pointer). When a connector calls
// `rawnet_connect(name)`, the registry:
//   1. allocates two regular channels (ring capacity 64 slots, blocking mode)
//      via `chan_create`, both typed with the publisher-declared payload
//      type-hash. `chan_create` inserts all four endpoint tokens into the
//      connector's handle table;
//   2. moves the pair `(read-end of channel-A, write-end of channel-B)` out
//      of the connector's table and into a kernel-staged channel message,
//      and writes that message on the accept channel — the publisher
//      receives it via a plain `chan_recv`;
//   3. returns the remaining pair `(write-end of A, read-end of B)` to the
//      connector.
// The semantics are thus: A is the `connector -> publisher` direction
// (the "request" channel); B is the `publisher -> connector` direction
// (the "response" channel). Both carry messages of the publisher-declared
// type. Neither side can starve the other — each channel has its own ring.
//
// The registry is flat: 64 slots protected by a single spinlock. Names are
// matched case-sensitively with `memcmp`. Duplicate publish by a different
// PID returns `CAP_V2_EEXIST`; republish by the same PID replaces the slot
// (supports daemon respawn).
//
// Peer-death cleanup
// ------------------
// `rawnet_on_peer_death(pid)` is called from `userdrv_on_owner_death` and
// from task destruction. It scans the table, clears any entry whose
// `publisher_pid == pid`, and decrements the refcount it took on the
// publisher's accept-channel. Any subscriber blocked in a subsequent
// `chan_recv` on a per-connection channel will observe `-EPIPE` via the
// existing `chan_endpoint_deactivate` path (the per-connection channels
// share the publisher's lifecycle through their read-end handle, which was
// transferred into the publisher's cap_handle table during `rawnet_connect`
// — when the publisher dies, the read end is destroyed, channel refcount
// drops, and waiters are woken).
//
// Reserved names & lifetimes
// --------------------------
// Phase 22 reserves:
//   `/sys/net/rawframe` — published once by `/bin/e1000d`; subscribed once
//      by `/bin/netd`. Message payload type = grahaos.net.frame.v1 (RX/TX
//      slot notifications + VMO handle transfer in the initial ANNOUNCE).
//   `/sys/net/service` — published once by `/bin/netd`; subscribed by
//      every user client (`ifconfig`, `ping`, `httptest`, `nettest`,
//      `dnstest`, `aitest`, `grahai`, migrated `gash` builtins). Message
//      payload type = grahaos.net.socket.v1.
// Name strings are limited to 64 ASCII bytes and must begin with '/'.
//
// Lock order
// ----------
// `g_rawnet_lock` → `channel_t.lock` (via the `chan_*` helpers). We never
// take `g_rawnet_lock` while holding a `channel_t.lock`.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cap/token.h"

// Phase 22 reserved names. Kept here so spec-conformance tests can check
// them directly instead of grepping for string literals.
#define RAWNET_NAME_RAWFRAME   "/sys/net/rawframe"
#define RAWNET_NAME_SERVICE    "/sys/net/service"

#define RAWNET_NAME_MAX        64u    // max bytes (excluding NUL)
#define RAWNET_SLOTS_MAX       64u    // max simultaneously-published names
#define RAWNET_CONN_CAPACITY   64u    // ring slots per per-connection channel

// Per-entry state. One slot per published name.
typedef struct rawnet_publish_entry {
    char     name[RAWNET_NAME_MAX + 1];  // +1 for trailing NUL
    uint8_t  name_len;                   // 0 = slot free
    uint8_t  _pad[7];
    int32_t  publisher_pid;              // PID of the daemon owning this name
    uint32_t connection_seq;             // monotonic: incremented per connect
    uint64_t payload_type_hash;          // FNV-1a hash of the per-conn channel type
    struct channel *accept_channel;      // cached pointer to the publisher's accept channel
    uint32_t accept_chan_obj_idx;        // cap_object idx of the accept-write endpoint
} rawnet_publish_entry_t;

// Accept-notification payload. Sent by the kernel on the accept channel
// whenever a connect succeeds. The publisher recv's this message and
// reads out:
//   - header.handles[0]  = read end of channel A (client → server)
//   - header.handles[1]  = write end of channel B (server → client)
//   - inline_payload     = rawnet_accept_info_t (connector_pid, conn_id)
typedef struct rawnet_accept_info {
    int32_t  connector_pid;
    uint32_t connection_id;   // publisher_entry->connection_seq at bind time
    uint64_t _reserved;
} rawnet_accept_info_t;

_Static_assert(sizeof(rawnet_accept_info_t) == 16,
               "rawnet_accept_info_t must be 16 bytes");

// --- Lifecycle -----------------------------------------------------------

// Initialise the registry. Idempotent. Called once from kmain after
// userdrv_init.
void rawnet_init(void);

// --- Backend entry points for SYS_CHAN_PUBLISH / SYS_CHAN_CONNECT --------

// Publish a named service. `name` + `name_len` must describe a well-formed
// ASCII name (starts with '/', no NULs, length in [1, RAWNET_NAME_MAX]).
// `payload_type_hash` must already be registered via manifest_type_known.
// `accept_write_tok` must be a valid CHANNEL WRITE endpoint owned by
// `publisher_pid` whose channel was created with the same
// `payload_type_hash` is NOT required — the accept channel has a DIFFERENT
// type (grahaos.net.accept.v1); only the per-connection channels use the
// payload hash.
//
// Returns 0 on success, CAP_V2_EINVAL / CAP_V2_EPERM (name already published
// by a different pid) / CAP_V2_EPROTOTYPE (unknown payload hash) /
// CAP_V2_ENOMEM on failure.
//
// On success: bumps the accept channel's refcount so dead-publisher
// cleanup is deterministic.
int rawnet_publish(int32_t publisher_pid,
                   const char *name, uint32_t name_len,
                   uint64_t payload_type_hash,
                   cap_token_t accept_write_tok);

// Connect to a named service. `out_wr_req_tok` receives the client-side
// WRITE endpoint of channel A (client → server / request direction).
// `out_rd_resp_tok` receives the client-side READ endpoint of channel B
// (server → client / response direction).
//
// Returns 0 on success, CAP_V2_EBADF if no such name, CAP_V2_EINVAL for
// bad arguments, CAP_V2_ENOMEM on allocation failure, CAP_V2_EPIPE if
// the publisher has died before we could deliver the accept notification.
int rawnet_connect(int32_t connector_pid,
                   const char *name, uint32_t name_len,
                   cap_token_t *out_wr_req_tok,
                   cap_token_t *out_rd_resp_tok);

// --- Lifecycle glue from userdrv / sched / cap_object ---------------------

// Called when a task exits. Clears every registry entry owned by `pid`
// and drops the refcount on each such accept channel. Idempotent; cheap
// if `pid` owns no entries (O(RAWNET_SLOTS_MAX) linear scan).
void rawnet_on_peer_death(int32_t pid);

// Look up a name; returns a pointer into g_rawnet_entries[] on success
// (strictly for read-only audit / test introspection) or NULL on miss.
// Callers MUST hold the registry lock themselves — this is a low-level
// accessor, not a public API.
const rawnet_publish_entry_t *rawnet_lookup_locked(const char *name,
                                                   uint32_t name_len);

// --- Validation helpers (visible for tests) ------------------------------

// Returns true iff the name is syntactically valid (leading '/',
// 1..RAWNET_NAME_MAX bytes, no NULs or control chars).
bool rawnet_name_validate(const char *name, uint32_t name_len);
