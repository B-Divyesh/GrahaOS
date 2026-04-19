// kernel/ipc/channel.h — Phase 17.
//
// Bidirectional typed message channel. One channel_t backs TWO CAP_KIND_CHANNEL
// cap_objects — a read endpoint and a write endpoint. Handle-passing moves
// one endpoint's cap_token to another process (via SYS_CHAN_SEND or
// attrs.handles_to_inherit on SYS_SPAWN); once both endpoints are closed the
// channel_t is freed.
//
// Lock order: cap_handle_table.lock → channel_t.lock → pmm/kheap.
//
// Non-goals in Phase 17: multi-reader fan-out (channels are strictly
// point-to-point), per-message priority, out-of-band messages.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../sync/spinlock.h"
#include "../cap/token.h"       // cap_token_t

struct task_struct;
struct cap_object;

// --- Mode + direction ----------------------------------------------------
#define CHAN_MODE_BLOCKING     1u
#define CHAN_MODE_NONBLOCKING  2u

#define CHAN_ENDPOINT_READ     1u
#define CHAN_ENDPOINT_WRITE    2u

#define CHAN_MSG_INLINE_MAX    256u
#define CHAN_MSG_HANDLES_MAX   8u
#define CHAN_CAPACITY_MAX      4096u
#define CHANNEL_MAGIC          0xCAFEC4A1u

// --- Message header ------------------------------------------------------
// 32 bytes. Emitted by sender, validated by kernel, echoed to receiver.
typedef struct chan_msg_header {
    uint64_t seq;            //  0..7   Per-channel monotonic at send
    uint64_t type_hash;      //  8..15  Sender-provided; must match channel
    uint32_t sender_pid;     // 16..19  Filled by kernel
    uint16_t inline_len;     // 20..21  Bytes valid in inline_payload (0..256)
    uint8_t  nhandles;       // 22      Handles in the slot (0..8)
    uint8_t  flags;          // 23      MSG_FLAG_*
    uint64_t timestamp_tsc;  // 24..31  rdtsc snapshot at send
} chan_msg_header_t;

_Static_assert(sizeof(chan_msg_header_t) == 32, "chan_msg_header_t must be 32 bytes");

// --- Full in-kernel message slot ----------------------------------------
// 320 bytes: 32 header + 256 payload + 32 handle array. Handles are stored
// as cap_object_t indices (uint32_t); the sender's table entry is removed
// at send time, and the receiver re-inserts into its own table at recv.
typedef struct channel_msg {
    chan_msg_header_t header;
    uint8_t           inline_payload[CHAN_MSG_INLINE_MAX];
    uint32_t          in_flight_idx[CHAN_MSG_HANDLES_MAX];
} channel_msg_t;

_Static_assert(sizeof(channel_msg_t) == 320, "channel_msg_t must be 320 bytes");

// --- Userspace-visible message ------------------------------------------
// 32 header + 256 inline + 8 * 8 handles = 352 bytes. Userspace sees full
// cap_token_t's (object_idx | generation | flags). See user/syscalls.h for
// the matching declaration.
typedef struct chan_msg_user {
    chan_msg_header_t header;
    uint8_t           inline_payload[CHAN_MSG_INLINE_MAX];
    cap_token_t       handles[CHAN_MSG_HANDLES_MAX];
} chan_msg_user_t;

_Static_assert(sizeof(chan_msg_user_t) == 352, "chan_msg_user_t must be 352 bytes");

// --- Channel struct ------------------------------------------------------
typedef struct channel {
    uint32_t magic;                  //  0..3    CHANNEL_MAGIC
    uint32_t mode;                   //  4..7    CHAN_MODE_*
    uint64_t id;                     //  8..15   Monotonic global id
    uint64_t type_hash;              // 16..23   Manifest-type anchor
    uint32_t capacity;               // 24..27   Ring slots (1..4096)
    uint32_t head;                   // 28..31   Reader index
    uint32_t tail;                   // 32..35   Writer index
    uint32_t msgcount;               // 36..39   Live messages in ring
    uint32_t refcount;               // 40..43   # outstanding endpoint caps
    uint32_t read_cap_idx;           // 44..47   cap_object_idx for read end
    uint32_t write_cap_idx;          // 48..51   cap_object_idx for write end
    uint32_t _pad0;                  // 52..55
    struct task_struct *read_waiters;   // 56..63   list of tasks blocked on recv
    struct task_struct *write_waiters;  // 64..71   list of tasks blocked on send
    channel_msg_t   *ring;           // 72..79   kheap array of capacity slots
    uint64_t seq_next;               // 80..87   Monotonic send seq (audit)
    uint64_t total_sends;            // 88..95   telemetry
    uint64_t total_recvs;            // 96..103  telemetry
    uint64_t rejected_messages;      // 104..111 telemetry
    int32_t  owner_pid;              // 112..115 creator pid
    uint32_t _pad1;                  // 116..119
    spinlock_t lock;                 // 120..167 48-byte spinlock_t
} channel_t;

// --- Endpoint payload ----------------------------------------------------
// Stored at cap_object.kind_data via heap allocation. One per endpoint cap.
typedef struct chan_endpoint {
    channel_t *channel;  // backing channel (held under channel refcount)
    uint8_t    direction;  // CHAN_ENDPOINT_READ or _WRITE
    uint8_t    _pad[7];
} chan_endpoint_t;

_Static_assert(sizeof(chan_endpoint_t) == 16, "chan_endpoint_t must be 16 bytes");

// --- Lifecycle -----------------------------------------------------------
void channel_subsystem_init(void);

// Create a channel. Allocates channel_t, ring, and two cap_objects (one
// read endpoint, one write endpoint) both owned by caller_pid with
// audience = [caller_pid]. Writes the resulting read token to *rd_tok and
// write token to *wr_tok. Returns 0 on success, CAP_V2_* error otherwise.
int chan_create(uint64_t type_hash, uint32_t mode, uint32_t capacity,
                int32_t caller_pid,
                cap_token_t *rd_tok_out, cap_token_t *wr_tok_out);

// Kernel-internal: create a bare channel_t without cap_object wrapping.
// Used by the pipe shim which operates below the capability layer.
// Returns channel_t* on success, NULL on failure.
channel_t *chan_create_kernel(uint64_t type_hash, uint32_t mode,
                              uint32_t capacity);

// Kernel-internal: tear down a bare channel_t from chan_create_kernel.
// Drops both endpoint refcount slots and frees storage.
void chan_destroy_kernel(channel_t *c);

// Called from cap_object_destroy() when the last handle to an endpoint
// cap_object closes. Drops channel refcount and frees the channel at zero.
void chan_endpoint_deactivate(struct cap_object *obj);

// --- Send / recv ---------------------------------------------------------
// msg_kern is already-marshalled kernel memory (produced by msg_copyin). The
// function validates type_hash, copies the slot into the ring, transfers
// handles into the slot, wakes a reader. Blocks on full ring per mode.
// Returns 0 on success or negative CAP_V2_* on failure.
int chan_send(channel_t *c, struct task_struct *sender,
              channel_msg_t *msg_kern, uint64_t timeout_ns);

// Pops the head slot into msg_kern. Transfers handles out of the slot into
// the receiver's handle table. Wakes a writer if any were waiting for space.
// Returns # inline bytes on success; negative CAP_V2_* on failure.
int chan_recv(channel_t *c, struct task_struct *receiver,
              channel_msg_t *msg_kern, uint64_t timeout_ns);

// Non-blocking readability/writability probe. Returns bitmask:
//  bit 0 = readable (has messages), bit 1 = writable (has space),
//  bit 2 = closed-peer (EPIPE on further I/O).
uint32_t chan_poll_probe(channel_t *c);

// --- Handle-transfer helpers (kernel-internal) ---------------------------
// Extract a channel_t* from a cap_token_t if the handle resolves to a
// CHANNEL endpoint with the given direction and required_rights. Sets
// *out_channel and *out_obj_idx on success. Returns CAP_V2_* error on fail.
int chan_resolve_endpoint(int32_t caller_pid, cap_token_t tok, uint8_t dir,
                          uint64_t required_rights,
                          channel_t **out_channel, uint32_t *out_obj_idx);

// --- Marshaling + handle transfer ---------------------------------------
// chan_marshal_send: copies a chan_msg_user_t from user space into a freshly
// staged channel_msg_t suitable for ring insertion. Handles are atomically
// transferred OUT of the sender's handle table: on success, sender no longer
// owns them; on failure, sender's table is untouched (all-or-nothing).
// Returns 0 on success, negative CAP_V2_* on failure.
int chan_marshal_send(struct task_struct *sender,
                      const chan_msg_user_t *user_msg,
                      channel_msg_t *staged);

// chan_marshal_recv: copy a ring slot into a userspace-shaped chan_msg_user_t,
// inserting each in-flight cap_object_idx into the receiver's handle table
// and writing the resulting cap_tokens into user_msg->handles[]. On any
// insert failure, already-inserted handles are removed again before returning.
int chan_marshal_recv(struct task_struct *receiver,
                      const channel_msg_t *slot,
                      chan_msg_user_t *user_msg);
