// kernel/cap/token.h
// Phase 15a: Capability Objects v2 — token layout and helpers.
//
// A cap_token_t is the opaque 64-bit handle userspace holds. It packs:
//
//     bits [63..32] = generation (32 bits)
//     bits [31..8]  = index into g_cap_object_ptrs (24 bits, max 16 777 216)
//     bits [7..0]   = flags (CAP_FLAG_* bitmap; subset of the underlying
//                     cap_object_t.flags that this holder claims)
//
// The token is never heap-allocated — it is passed by value in registers.
//
// Resolution (`cap_token_resolve`) is the hot path for every cap-taking
// syscall. It fetches g_cap_object_ptrs[idx], verifies the object exists,
// is not deleted, matches the token's generation, the calling pid is in
// its audience (or CAP_FLAG_PUBLIC bypasses that check), and that the
// required rights are a subset of the object's rights_bitmap.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// ------------------------------------------------------------------------
// Packed 64-bit token — wrapped in a struct so the C type is distinct from
// a bare u64 and accidental arithmetic is caught by the compiler.
// ------------------------------------------------------------------------
typedef struct cap_token {
    uint64_t raw;
} cap_token_t;

// ------------------------------------------------------------------------
// Kind values (cap_object_t.kind; NOT stored in token).
// ------------------------------------------------------------------------
#define CAP_KIND_NONE           0
#define CAP_KIND_CAN            1   // Phase 8b / 15a CAN entry
#define CAP_KIND_FILE           2   // Phase 10 fd, Phase 17 properly
#define CAP_KIND_PROC           3
#define CAP_KIND_CHANNEL        4   // Phase 17
#define CAP_KIND_VMO            5   // Phase 17
#define CAP_KIND_SNAPSHOT       6   // Phase 24
#define CAP_KIND_WASM_INSTANCE  7   // Phase 26
#define CAP_KIND_STREAM         8   // Phase 18
#define CAP_KIND_FS_SNAPSHOT    9   // Phase 19 (FS-level version-chain pin)
// Phase 21: userspace driver framework. REGISTRAR is held by daemons authorised
// to claim PCI devices via sys_drv_register; init grants derived tokens to its
// declared driver children. IRQ_CHANNEL and MMIO_REGION are returned by a
// successful registration and resolved by sys_drv_irq_wait / sys_mmio_vmo_create.
#define CAP_KIND_DRIVER_REGISTRAR  10
#define CAP_KIND_IRQ_CHANNEL       11
#define CAP_KIND_MMIO_REGION       12

// ------------------------------------------------------------------------
// Rights bitmap (cap_object_t.rights_bitmap — 64 bits).
// ------------------------------------------------------------------------
#define RIGHT_READ        0x0000000000000001ULL
#define RIGHT_WRITE       0x0000000000000002ULL
#define RIGHT_EXEC        0x0000000000000004ULL
#define RIGHT_DERIVE      0x0000000000000008ULL
#define RIGHT_REVOKE      0x0000000000000010ULL
#define RIGHT_INSPECT     0x0000000000000020ULL
#define RIGHT_ACTIVATE    0x0000000000000040ULL  // CAN-only
#define RIGHT_DEACTIVATE  0x0000000000000080ULL  // CAN-only
#define RIGHT_GRANT       0x0000000000000100ULL
// Phase 17: channel-endpoint rights.
#define RIGHT_SEND        0x0000000000000200ULL  // CHANNEL write-end
#define RIGHT_RECV        0x0000000000000400ULL  // CHANNEL read-end
// 0x0000000000000800ULL .. reserved for higher kind-specific rights.
#define RIGHTS_ALL        0xFFFFFFFFFFFFFFFFULL

// ------------------------------------------------------------------------
// Flags (cap_object_t.flags, 8 bits; token flags are a subset of these).
// ------------------------------------------------------------------------
#define CAP_FLAG_PUBLIC             0x01  // bypass audience check
#define CAP_FLAG_EAGER_REVOKE       0x02  // revoke cascades to children
#define CAP_FLAG_SHIM_EPHEMERAL     0x04  // Phase 15a shim-synthesized token
#define CAP_FLAG_IMMORTAL           0x08  // revoke returns -EPERM
#define CAP_FLAG_INHERITABLE        0x10  // copy to child handle table on spawn
#define CAP_FLAG_CASCADE_TRUNCATED  0x20  // revoke overflowed work queue

// ------------------------------------------------------------------------
// Well-known pid sentinels.
// ------------------------------------------------------------------------
#define PID_PUBLIC  (0xFFFF)    // Audience-set sentinel implying PUBLIC.
#define PID_KERNEL  (-1)        // owner_pid for kernel-created objects.
#define PID_NONE    (-1)        // audience_set empty-slot marker.

// ------------------------------------------------------------------------
// Error codes. Negative returns from cap_* helpers; the syscall dispatcher
// propagates them unchanged to userspace (consistent with -errno convention).
// ------------------------------------------------------------------------
#define CAP_V2_OK        0
#define CAP_V2_EPERM    -1
#define CAP_V2_EREVOKED -2
#define CAP_V2_ENOMEM   -3
#define CAP_V2_EFAULT   -4
#define CAP_V2_EINVAL   -5
#define CAP_V2_ENOSYS   -6
// Phase 15b: pledge class denial. The caller's syscall was rejected because
// the needed pledge bit had already been dropped.
#define CAP_V2_EPLEDGE  -7
// Phase 16: legacy CAN syscall invoked. The string-named SYS_CAP_ACTIVATE and
// siblings (1031-1040) now return this; callers must migrate to the
// token-taking SYS_CAN_ACTIVATE_T / SYS_CAN_DEACTIVATE_T via SYS_CAN_LOOKUP.
#define CAP_V2_EDEPRECATED -78

// Phase 17: channel + VMO errors.
#define CAP_V2_EBADF       -9   // Bad handle (stale or wrong kind).
#define CAP_V2_EAGAIN      -11  // Would block (non-blocking ring full/empty).
#define CAP_V2_EPIPE       -32  // Peer endpoints all closed.
#define CAP_V2_EPROTOTYPE  -71  // Channel manifest-type mismatch on send.
#define CAP_V2_ETIMEDOUT  -110  // Blocking op exceeded timeout_ns.

// Phase 18: stream (async I/O) errors.
#define CAP_V2_ECANCELED  -125  // Outstanding stream job cancelled by destroy.

// Phase 19: GrahaFS v2 errors.
#define CAP_V2_EBADFS     -126  // Superblock magic/CRC bad; or unknown FS version.
#define CAP_V2_EROFS      -127  // Write attempted on read-only mount (v1 compat).

// Null token: idx==0 is reserved slot, always fails resolve.
#define CAP_TOKEN_NULL ((cap_token_t){.raw = 0})

// ------------------------------------------------------------------------
// Pack/unpack helpers (inline).
// ------------------------------------------------------------------------
static inline cap_token_t cap_token_pack(uint32_t gen, uint32_t idx,
                                         uint8_t flags) {
    cap_token_t t;
    t.raw = ((uint64_t)gen << 32)
          | (((uint64_t)idx & 0xFFFFFFULL) << 8)
          | (uint64_t)flags;
    return t;
}

static inline uint32_t cap_token_gen(cap_token_t t) {
    return (uint32_t)(t.raw >> 32);
}

static inline uint32_t cap_token_idx(cap_token_t t) {
    return (uint32_t)((t.raw >> 8) & 0xFFFFFFULL);
}

static inline uint8_t cap_token_flags(cap_token_t t) {
    return (uint8_t)(t.raw & 0xFFu);
}

static inline bool cap_token_is_null(cap_token_t t) {
    return cap_token_idx(t) == 0;
}

// ------------------------------------------------------------------------
// Slow-path helpers. Declared here; defined in token.c and object.c.
// ------------------------------------------------------------------------
// Forward-declare so callers don't have to include object.h.
struct cap_object;

// Validate that `pid` is a member of obj's audience (or PUBLIC).
bool cap_token_validate_audience(const struct cap_object *obj, int32_t pid);

// Pretty-print "tok={gen=N,idx=M,flags=0xFF}" into buf. Returns bytes written.
int cap_token_describe(cap_token_t tok, char *buf, int buflen);

// Hot-path resolver. Returns cap_object_t * on success, NULL on any failure.
// Non-inline in Phase 15a: see unit 5 of plan. Still lock-free; generation
// loaded atomic-acquire. ~20-30 cycles typical.
struct cap_object *cap_token_resolve(int32_t calling_pid, cap_token_t tok,
                                     uint64_t required_rights);
