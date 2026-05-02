// kernel/snap/chan_freeze.c
// Phase 24 W18: channel freeze / thaw / drain primitives for the COW
// snapshot subsystem.
//
// When a snapshot captures a chan_endpoint_t whose peer is also in scope
// (W14.6 FROZEN case), we need to stop the channel completely so neither
// side advances past the captured state until the snapshot is restored
// or deleted. The freeze stamps `channel_t::frozen_at_snap = snap_id`,
// saves the ring head/tail/msgcount, and wakes any blocked senders /
// receivers with CAP_V2_EFROZEN.
//
// chan_thaw clears the freeze stamp. snap_restore passes restored head/
// tail values (rewinding the ring); snap_delete passes the CHAN_THAW_KEEP
// sentinel (leave the queue alone, just drop the freeze stamp).
//
// chan_drain_to_vmo and chan_redrain_from_vmo are stubs in this iteration
// — they return -ENOSYS until the W14.6 capture path is in tree, since
// drain semantics require VMO allocation tied to the snapshot lifecycle
// (and writing the drain logic without a caller invites bit-rot). The
// shape of the API is preserved so W14.6 can land its calls and have the
// implementations follow without churn.
//
// Lookups go through chan_lookup_by_id, which walks the global channel
// registry installed in W18.2. The registry is small (channels are tens
// to low hundreds in steady state); a hash table is unnecessary at this
// scale.

#include "snapshot.h"

#include <stdint.h>
#include <stddef.h>

#include "../ipc/channel.h"
#include "../cap/token.h"
#include "../log.h"

// Mirror the "leave head/tail untouched" sentinel used by chan_thaw_locked.
#ifndef CHAN_THAW_KEEP
#define CHAN_THAW_KEEP UINT32_MAX
#endif

// ---------------------------------------------------------------------------
// chan_freeze: stamp the channel as held by snap_id. Idempotent for the
// SAME snap_id (so W14.6 can call freeze on both endpoints without
// double-counting); EBUSY if the channel is already held by a DIFFERENT
// snapshot. EINVAL on bad inputs / unresolved chan_id.
// ---------------------------------------------------------------------------
int chan_freeze(uint64_t chan_id, uint64_t snap_id) {
    if (chan_id == 0 || snap_id == 0) return CAP_V2_EINVAL;
    channel_t *c = chan_lookup_by_id(chan_id);
    if (!c) return CAP_V2_EINVAL;
    int rc = chan_freeze_locked(c, snap_id);
    if (rc == CAP_V2_OK) {
        klog(KLOG_INFO, SUBSYS_CAP,
             "chan_freeze: chan_id=%lu snap_id=%lu",
             (unsigned long)chan_id, (unsigned long)snap_id);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// chan_thaw: drop the freeze and (optionally) rewind head/tail. Used both
// by snap_restore (rewind) and snap_delete (just drop, keep the queue).
// snap_delete should pass head=tail=UINT32_MAX which chan_thaw_locked
// recognises as CHAN_THAW_KEEP.
// ---------------------------------------------------------------------------
int chan_thaw(uint64_t chan_id, uint32_t head, uint32_t tail) {
    if (chan_id == 0) return CAP_V2_EINVAL;
    channel_t *c = chan_lookup_by_id(chan_id);
    if (!c) return CAP_V2_EINVAL;
    int rc = chan_thaw_locked(c, head, tail);
    if (rc == CAP_V2_OK) {
        klog(KLOG_INFO, SUBSYS_CAP,
             "chan_thaw: chan_id=%lu head=%u tail=%u",
             (unsigned long)chan_id, (unsigned)head, (unsigned)tail);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// chan_drain_to_vmo / chan_redrain_from_vmo: stubs until W14.6 lands the
// caller. Returning -ENOSYS keeps the API shape intact while the actual
// drain semantics (which need VMO allocation, ring-walk + serialise, and
// per-snapshot lifetime tracking) are designed alongside the capture
// pass.
// ---------------------------------------------------------------------------
int chan_drain_to_vmo(uint64_t chan_id, uint64_t vmo_id) {
    (void)chan_id;
    (void)vmo_id;
    return CAP_V2_ENOSYS;
}

int chan_redrain_from_vmo(uint64_t chan_id, uint64_t vmo_id) {
    (void)chan_id;
    (void)vmo_id;
    return CAP_V2_ENOSYS;
}
