/*
 * user/wasmd/proto.h — wasmd named-channel protocol.
 *
 * FU27.WASM Stage D1. The /sys/wasm/control endpoint accepts these
 * message types over a duplex chan pair. Type hashes are FNV-1a 64-bit
 * over the message-name string so the receiver can dispatch by type.
 *
 * V1 design notes (D1):
 *   - RUN_MODULE carries the path inline (256 bytes max). wasmd opens
 *     the file itself, copies bytes through /tmp/wasmd_pending.wasm,
 *     and spawns wasmd_worker via PLEDGE_FLAG_NARROW_EXEC.
 *   - Single-slot fixture-staging file (/tmp/wasmd_pending.wasm) means
 *     wasmd serializes RUN_MODULE requests in v1. D2 wasm_concurrent_16
 *     extends to per-instance staging.
 *   - RESPONSE inline_payload format: [int32 status][stdout bytes...]
 *     where stdout is up to 252 bytes (256 - 4). Beyond 252 bytes the
 *     output is truncated; D2 may add an out-VMO handle.
 */

#ifndef _WASMD_PROTO_H
#define _WASMD_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* Inline FNV-1a 64-bit hash — wasmd, wasm CLI, and tests all need to
 * compute the same hashes; libnet's helper is static-internal. Kept tiny
 * so a single include gives every consumer a uniform implementation. */
static inline uint64_t wasmd_fnv1a_hash64(const char *s, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static inline uint64_t wasmd_type_hash(void)        { return wasmd_fnv1a_hash64("grahaos.wasm.service.v1", 23); }
static inline uint64_t wasmd_run_module_hash(void)  { return wasmd_fnv1a_hash64("RUN_MODULE", 10); }
static inline uint64_t wasmd_run_response_hash(void){ return wasmd_fnv1a_hash64("RUN_RESPONSE", 12); }

#define WASMD_SERVICE_NAME           "/sys/wasm/control"
#define WASMD_SERVICE_NAME_LEN       17  /* strlen(WASMD_SERVICE_NAME) */

/* Channel-payload type — must match kernel/ipc/manifest.c slot 12.
 * Use wasmd_type_hash() at runtime to get the FNV-1a value (callers
 * include libnet/libnet.h which exports libnet_fnv1a_hash64). The
 * literal string here matches MANIFEST_NAME_WASM_SERVICE_V1 in
 * kernel/ipc/manifest.h. */
#define WASMD_TYPE_NAME              "grahaos.wasm.service.v1"
#define WASMD_TYPE_NAME_LEN          23

/* Op codes — placed at inline_payload[0..3] (uint32_t) to dispatch
 * RUN_MODULE vs KILL_INSTANCE vs INSPECT_INSTANCE. The channel header
 * type_hash carries the channel-payload type (wasmd_type_hash()) and
 * the kernel rejects mismatching channels at chan_send time. The op
 * code is OUR discriminator on top of the shared channel payload. */
#define WASMD_OP_RUN_MODULE         0x10000001u
#define WASMD_OP_RUN_RESPONSE       0x10000002u
#define WASMD_OP_KILL_INSTANCE      0x10000003u
#define WASMD_OP_INSPECT_INSTANCE   0x10000004u
/* FU29.X.wasmd_subprocess: run the module in a KILLABLE worker subprocess
 * (bin/wasmd_worker) instead of in-process.  Module bytes are hex-encoded
 * into the worker's argv (NO filesystem transport — the worker never holds
 * the global vfs_lock, so wasmd's deadline SIGKILL of a runaway worker lands
 * while the worker is in a pure-userspace wasm loop = orphan-free, per the
 * kernel kill-safety analysis).  Used for runaway / fuel-exhaust modules that
 * the in-process path fundamentally cannot contain (m3_CallV never returns). */
#define WASMD_OP_RUN_MODULE_ISOLATED 0x10000005u

/* RUN_MODULE message layout in inline_payload:
 *   [0..3]   uint32_t op = WASMD_OP_RUN_MODULE
 *   [4..259] zero-terminated path string (256 bytes including NUL)
 *   handles[]  unused in v1
 */

/* RUN_MODULE inline_payload format:
 *   [0..3]    uint32_t op = WASMD_OP_RUN_MODULE
 *   [4..7]    uint32_t wasm_len  (number of valid bytes that follow)
 *   [8..]     wasm module bytes (capped at 248 to fit in 256-byte payload)
 *
 * Inlining bytes through the channel avoids wasmd making any
 * filesystem syscalls — the kernel's VFS lock has a known cross-CPU
 * race that fires when wasmd reads under load. D2 will extend with
 * VMO-backed transfer for modules > 248 bytes.
 */
#define WASMD_BYTES_OFFSET           8    /* wasm bytes start after op + len */
#define WASMD_BYTES_MAX              248  /* 256 - 8 = 248 bytes per inline msg */

/* RESPONSE: wasmd → client.
 * inline_payload layout:
 *   [0..3]     uint32_t op = WASMD_OP_RUN_RESPONSE
 *   [4..7]     int32_t  status: 0 success, -E* on error
 *   [8..11]    uint32_t stdout_len  (number of valid bytes in payload[12..])
 *   [12..255]  stdout-bytes (zero-padded, optional)
 *   handles[]  unused in v1
 */
typedef struct wasmd_response_header {
    uint32_t op;
    int32_t  status;
    uint32_t stdout_len;
    uint32_t reserved;     /* 16-byte aligned for stdout buffer */
} wasmd_response_header_t;

/* Status codes returned in response. */
#define WASMD_OK                      0
#define WASMD_E_PATH_TOO_LONG       -1
#define WASMD_E_OPEN_FAILED         -2
#define WASMD_E_READ_FAILED         -3
#define WASMD_E_LOAD_FAILED         -4
#define WASMD_E_INSTANTIATE_FAILED  -5
#define WASMD_E_TRAP                -6
#define WASMD_E_WORKER_SPAWN        -7
#define WASMD_E_WORKER_CRASH        -8
#define WASMD_E_INTERNAL            -9
/* Phase 29 Session G — additional status codes for the FU27.WASM.D2_worker
 * orchestrator. Returned when the worker hits a host-binding cap-check
 * failure (-10), the wall-clock watchdog (-11), or an external SIGKILL
 * delivered to the worker mid-run (-12). */
#define WASMD_E_CAP_DENIED          -10
#define WASMD_E_FUEL_EXHAUSTED      -11
#define WASMD_E_WORKER_KILLED       -12

/* Single-slot staging filenames — D1 v1 only. D2 extends. */
#define WASMD_PENDING_PATH           "/tmp/wasmd_pending.wasm"
#define WASMD_OUTPUT_PATH            "/tmp/wasmd_output.txt"

#endif /* _WASMD_PROTO_H */
