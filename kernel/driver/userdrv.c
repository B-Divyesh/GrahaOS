// kernel/driver/userdrv.c — Phase 21.
//
// U4: drv_irq_channel SPSC ring primitives. Subsequent units extend this file
// with the ownership table (U5), syscall handlers (U6-U8), ISR dispatcher
// (U9), and owner-death cleanup (U10).

#include "userdrv.h"

#include <string.h>

#include "../mm/kheap.h"
#include "../mm/slab.h"
#include "../mm/mmio_vmo.h"
#include "../sync/spinlock.h"
#include "../log.h"
#include "../audit.h"
#include "../cap/object.h"
#include "../cap/handle_table.h"
#include "../cap/pledge.h"
#include "../../arch/x86_64/cpu/sched/sched.h"
#include "../../arch/x86_64/cpu/tsc.h"

// Forward decl from interrupts.c (PIC-line helper).
extern void pic_unmask_irq(uint8_t line);
extern void pic_mask_irq(uint8_t line);

// Phase 21.1: IOAPIC routing — under LAPIC mode, pic_unmask_irq is a no-op
// (smp_init's pic_disable masks all 16 PIC lines globally). Without an IOAPIC
// redirection-table entry, IRQ 11 (E1000 typical) cannot reach a userdrv
// vector. ioapic_route_irq programs the entry, ioapic_{un}mask flips bit 16.
extern void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id,
                             bool level, bool active_low);
extern void ioapic_mask_irq(uint8_t gsi);
extern void ioapic_unmask_irq(uint8_t gsi);

// ---------------------------------------------------------------------------
// Slab cache for drv_irq_channel_t (~1100 bytes including embedded channel_t
// and the 1 KiB ring). One allocation per claimed device, max ~32. Slab is
// overkill for ~32 objects but keeps allocation deterministic.
// ---------------------------------------------------------------------------
static kmem_cache_t *g_irq_chan_cache = NULL;

// ===========================================================================
// Ownership table. One entry per enumerated PCI function. Indexed parallel
// to g_pci_table[i]. Initialised by userdrv_init from the post-enumeration
// PCI table; mutated only under g_userdrv_registry_lock.
// ===========================================================================
userdrv_entry_t g_userdrv_entries[PCI_TABLE_MAX];
spinlock_t g_userdrv_registry_lock = SPINLOCK_INITIALIZER("userdrv_registry");

void userdrv_init(void) {
    g_irq_chan_cache = kmem_cache_create("drv_irq_channel_t",
                                         sizeof(drv_irq_channel_t),
                                         _Alignof(drv_irq_channel_t),
                                         NULL, SUBSYS_CORE);
    if (!g_irq_chan_cache) {
        klog(KLOG_FATAL, SUBSYS_CORE, "userdrv_init: irq channel cache failed");
        return;
    }

    // Populate the ownership table from the post-enumeration PCI table.
    // Kernel-side `_expose_to_userdrv` calls (e.g., e1000_expose_to_userdrv)
    // run AFTER this and flip is_claimable=1 on their device's entry.
    spinlock_acquire(&g_userdrv_registry_lock);
    for (uint32_t i = 0; i < PCI_TABLE_MAX; i++) {
        memset(&g_userdrv_entries[i], 0, sizeof(userdrv_entry_t));
    }
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        pci_table_entry_t *p = &g_pci_table[i];
        userdrv_entry_t *e = &g_userdrv_entries[i];
        e->pci_addr        = pci_pack_addr(p->bus, p->device, p->function);
        e->vendor_id       = p->vendor_id;
        e->device_id       = p->device_id;
        e->device_class    = p->class_code;
        e->device_subclass = p->subclass;
        e->is_claimable    = 0;        // Drivers must explicitly expose.
        e->driver_owner_pid = 0;
        e->bar_phys        = p->bars[0];
        e->bar_size        = p->bar_sizes[0];
    }
    spinlock_release(&g_userdrv_registry_lock);

    klog(KLOG_INFO, SUBSYS_CORE,
         "userdrv_init: %u devices tracked (cache_sizeof=%u)",
         (unsigned)g_pci_table_count, (unsigned)sizeof(drv_irq_channel_t));
}

int userdrv_mark_claimable(uint16_t vendor_id, uint16_t device_id) {
    spinlock_acquire(&g_userdrv_registry_lock);
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        userdrv_entry_t *e = &g_userdrv_entries[i];
        if (e->vendor_id == vendor_id && e->device_id == device_id) {
            e->is_claimable = 1;
            spinlock_release(&g_userdrv_registry_lock);
            klog(KLOG_INFO, SUBSYS_CORE,
                 "userdrv: vendor=0x%04x device=0x%04x marked claimable (pci_addr=0x%06x)",
                 (unsigned)vendor_id, (unsigned)device_id,
                 (unsigned)e->pci_addr);
            return 0;
        }
    }
    spinlock_release(&g_userdrv_registry_lock);
    return -19;  // -ENODEV
}

userdrv_entry_t *userdrv_find_by_vector(uint8_t vector) {
    // No lock — called from ISR context. The fields the ISR reads
    // (irq_channel pointer, irq_count) are written under the registry lock
    // at register/unregister time, but the ISR's worst case is "device just
    // unclaimed" → irq_channel == NULL → ISR drops the message + LAPIC EOI.
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        if (g_userdrv_entries[i].irq_vector == vector &&
            g_userdrv_entries[i].driver_owner_pid != 0) {
            return &g_userdrv_entries[i];
        }
    }
    return NULL;
}

drv_irq_channel_t *drv_irq_channel_create(int32_t owner_pid) {
    if (!g_irq_chan_cache) return NULL;
    drv_irq_channel_t *c = (drv_irq_channel_t *)kmem_cache_alloc(g_irq_chan_cache);
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    // Initialise embedded channel_t fields so cap accounting works the same
    // as a regular channel (refcount, lock, owner_pid, mode/capacity).
    c->base.magic     = CHANNEL_MAGIC;
    c->base.mode      = CHAN_MODE_BLOCKING;
    c->base.capacity  = DRV_IRQ_RING_SIZE;
    c->base.refcount  = 1;
    c->base.owner_pid = owner_pid;
    spinlock_init(&c->base.lock, "drv_irq_chan");
    // SPSC counters start at zero; ring[] already memset.
    return c;
}

void drv_irq_channel_destroy(drv_irq_channel_t *c) {
    if (!c) return;
    // Mark dead so any post-teardown ISR (shouldn't happen — the IRQ vector
    // is unbound first by userdrv_on_owner_death) drops messages cleanly.
    c->dead = 1;
    // Wake any blocked consumer with -ESHUTDOWN.
    sched_wake_all_on_channel(&c->base.read_waiters, -32 /* ESHUTDOWN */);
    c->base.magic = 0;
    kmem_cache_free(g_irq_chan_cache, c);
}

// ---------------------------------------------------------------------------
// SPSC ring — lock-free producer (ISR) + lock-free consumer (daemon).
//
// Memory ordering pairing:
//   Producer:
//     1. RELAXED-load head (we are the sole producer; nobody else writes it).
//     2. ACQUIRE-load tail (synchronizes with consumer's RELEASE-store tail).
//     3. If (head - tail) >= SIZE → RELAXED fetch-add dropped, return false.
//     4. Write ring[head & MASK].
//     5. RELEASE-store head + 1 (publishes the write to the consumer).
//
//   Consumer:
//     1. RELAXED-load tail (we are the sole consumer).
//     2. ACQUIRE-load head (synchronizes with producer's RELEASE-store head).
//     3. While (tail < head) and out_count < max: copy ring[tail & MASK];
//        increment tail.
//     4. RELEASE-store tail (publishes the read to the producer for backpressure).
//
// The "dead" flag is read RELAXED — if a producer fires after teardown the
// drop counter just bumps; correctness preserved.
// ---------------------------------------------------------------------------
bool userdrv_isr_post(drv_irq_channel_t *c, const drv_irq_msg_t *msg) {
    if (!c || c->dead) return false;
    uint32_t head = __atomic_load_n(&c->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= DRV_IRQ_RING_SIZE) {
        __atomic_fetch_add(&c->dropped, 1, __ATOMIC_RELAXED);
        return false;
    }
    c->ring[head & DRV_IRQ_RING_MASK] = *msg;
    __atomic_store_n(&c->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t userdrv_irq_drain(drv_irq_channel_t *c, drv_irq_msg_t *out_msgs,
                           uint32_t max) {
    if (!c || !out_msgs || max == 0) return 0;
    uint32_t tail = __atomic_load_n(&c->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
    uint32_t available = head - tail;
    if (available == 0) return 0;
    uint32_t to_copy = available < max ? available : max;
    for (uint32_t i = 0; i < to_copy; i++) {
        out_msgs[i] = c->ring[(tail + i) & DRV_IRQ_RING_MASK];
    }
    __atomic_store_n(&c->tail, tail + to_copy, __ATOMIC_RELEASE);
    return to_copy;
}

// ===========================================================================
// Helper: find the userdrv_entry for a (vendor, device) pair AND the index
// in g_userdrv_entries (the index doubles as the IRQ-vector offset).
// Returns NULL on miss.
// ===========================================================================
static userdrv_entry_t *find_entry_by_id(uint16_t vendor, uint16_t device,
                                         uint32_t *idx_out) {
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        userdrv_entry_t *e = &g_userdrv_entries[i];
        if (e->vendor_id == vendor && e->device_id == device) {
            if (idx_out) *idx_out = i;
            return e;
        }
    }
    return NULL;
}

// ===========================================================================
// SYS_DRV_REGISTER backend.
// ===========================================================================
int userdrv_register_device(int32_t caller_pid,
                            uint16_t vendor_id, uint16_t device_id,
                            uint8_t device_class,
                            drv_caps_t *out_caps_kern) {
    if (!out_caps_kern) return CAP_V2_EFAULT;
    memset(out_caps_kern, 0, sizeof(*out_caps_kern));

    task_t *cur = sched_get_task(caller_pid);
    if (!cur) return CAP_V2_EINVAL;

    // Pledge gate: SYS_CONTROL is mandatory for all device classes; the
    // class-specific pledge depends on PCI class code.
    if (!pledge_allows(cur, PLEDGE_CLASS_SYS_CONTROL))
        return CAP_V2_EPLEDGE;
    uint8_t class_pledge = 0xFF;
    switch (device_class) {
        case 0x01: class_pledge = PLEDGE_CLASS_STORAGE_SERVER; break;
        case 0x02: class_pledge = PLEDGE_CLASS_NET_SERVER; break;
        // Class 0x09 = input controller (PS/2, etc); reserved for Phase 27.
        case 0x09: class_pledge = PLEDGE_CLASS_INPUT_SERVER; break;
        default:   class_pledge = PLEDGE_CLASS_SYS_CONTROL; break;  // permissive
    }
    if (class_pledge != PLEDGE_CLASS_SYS_CONTROL &&
        !pledge_allows(cur, class_pledge))
        return CAP_V2_EPLEDGE;

    spinlock_acquire(&g_userdrv_registry_lock);

    uint32_t entry_idx = 0;
    userdrv_entry_t *e = find_entry_by_id(vendor_id, device_id, &entry_idx);
    if (!e) {
        spinlock_release(&g_userdrv_registry_lock);
        return -19;  // -ENODEV
    }
    if (!e->is_claimable) {
        spinlock_release(&g_userdrv_registry_lock);
        return -19;  // -ENODEV (device not exposed by any kernel-side stub)
    }
    if (e->driver_owner_pid != 0) {
        spinlock_release(&g_userdrv_registry_lock);
        return -16;  // -EBUSY
    }
    if (e->bar_size == 0) {
        spinlock_release(&g_userdrv_registry_lock);
        return -22;  // -EINVAL (device has no primary BAR — unusable)
    }

    // -------- Allocate the MMIO VMO + cap --------
    vmo_t *mmio = mmio_vmo_create(e->bar_phys, e->bar_size, caller_pid);
    if (!mmio) {
        spinlock_release(&g_userdrv_registry_lock);
        return CAP_V2_ENOMEM;
    }
    int32_t audience[CAP_AUDIENCE_MAX + 1];
    audience[0] = caller_pid;
    audience[1] = PID_NONE;
    int mmio_idx = cap_object_create(CAP_KIND_MMIO_REGION,
                                     RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                         RIGHT_REVOKE,
                                     audience, 0,
                                     (uintptr_t)mmio, caller_pid,
                                     CAP_OBJECT_IDX_NONE);
    if (mmio_idx < 0) {
        vmo_free(mmio);
        spinlock_release(&g_userdrv_registry_lock);
        return mmio_idx;
    }
    mmio->cap_object_idx = (uint32_t)mmio_idx;

    // -------- Allocate the IRQ channel + cap --------
    drv_irq_channel_t *ic = drv_irq_channel_create(caller_pid);
    if (!ic) {
        cap_object_destroy((uint32_t)mmio_idx);
        spinlock_release(&g_userdrv_registry_lock);
        return CAP_V2_ENOMEM;
    }
    int irq_idx = cap_object_create(CAP_KIND_IRQ_CHANNEL,
                                    RIGHT_READ | RIGHT_RECV | RIGHT_INSPECT |
                                        RIGHT_REVOKE,
                                    audience, 0,
                                    (uintptr_t)ic, caller_pid,
                                    CAP_OBJECT_IDX_NONE);
    if (irq_idx < 0) {
        drv_irq_channel_destroy(ic);
        cap_object_destroy((uint32_t)mmio_idx);
        spinlock_release(&g_userdrv_registry_lock);
        return irq_idx;
    }

    // -------- Allocate the downstream channel pair (raw frames) --------
    // The downstream channel carries raw Ethernet frames (or storage cmds for
    // AHCI in Phase 23). Daemon writes to its WRITE end; the READ cap_object
    // is created but unused after Phase 22 Stage F (legacy e1000_proxy bridge
    // retired — netd now reads frames directly via /sys/net/rawframe).
    cap_token_t ds_rd_tok = {0}, ds_wr_tok = {0};
    int rc_chan = chan_create(0x0000000000000000ULL,  // type=untyped raw bytes (manifest_init registers '0' as wildcard for now)
                              CHAN_MODE_BLOCKING, 64,
                              caller_pid, &ds_rd_tok, &ds_wr_tok);
    // If chan_create rejected the type_hash (Phase 17 manifest validation),
    // fall back to a known type_hash. The manifest currently registers
    // 'grahaos.pipe.bytes.v1' (FNV-1a hash). Use a Phase-21 reserved type
    // 'grahaos.driver.rawframe.v1' if it gets registered later; for now use
    // the bytes-pipe hash.
    extern uint64_t fnv1a_hash64(const void *data, size_t len);
    if (rc_chan == CAP_V2_EPROTOTYPE) {
        const char *type_name = "grahaos.pipe.bytes.v1";
        uint64_t h = 0;
        // Compute strlen inline (manifest_lookup may use a different code path).
        size_t n = 0; while (type_name[n]) n++;
        h = fnv1a_hash64(type_name, n);
        rc_chan = chan_create(h, CHAN_MODE_BLOCKING, 64,
                              caller_pid, &ds_rd_tok, &ds_wr_tok);
    }
    if (rc_chan < 0) {
        cap_object_destroy((uint32_t)irq_idx);
        cap_object_destroy((uint32_t)mmio_idx);
        drv_irq_channel_destroy(ic);
        spinlock_release(&g_userdrv_registry_lock);
        return rc_chan;
    }
    // chan_create inserts both ends into caller's handle table. We want only
    // the WRITE end to stay there; remove the READ slot but keep the
    // cap_object alive so kernel readers (proxy) can resolve it later.
    // The cap_token's idx is the cap_object idx; the slot in the handle
    // table is a separate sparse index. We remove the matching slot.
    uint32_t ds_rd_obj_idx = cap_token_idx(ds_rd_tok);
    uint32_t ds_wr_obj_idx = cap_token_idx(ds_wr_tok);
    // Walk caller's handle table to find the slot that holds ds_rd_obj_idx.
    spinlock_acquire(&cur->cap_handles.lock);
    for (uint32_t s = 0; s < cur->cap_handles.capacity; s++) {
        if (cur->cap_handles.entries[s].object_idx == ds_rd_obj_idx) {
            spinlock_release(&cur->cap_handles.lock);
            cap_handle_remove(&cur->cap_handles, s);
            spinlock_acquire(&cur->cap_handles.lock);
            break;
        }
    }
    spinlock_release(&cur->cap_handles.lock);

    // -------- Phase 21.1: Allocate the upstream channel pair (proxy→daemon)
    // Daemon reads TX_NOTIFY (and future control) messages from the READ end;
    // kernel proxy holds the WRITE end. Symmetric to downstream — we keep the
    // WRITE cap_object alive kernel-side and remove its handle-table slot.
    cap_token_t up_rd_tok = {0}, up_wr_tok = {0};
    int rc_up = chan_create(0x0000000000000000ULL,
                            CHAN_MODE_BLOCKING, 64,
                            caller_pid, &up_rd_tok, &up_wr_tok);
    if (rc_up == CAP_V2_EPROTOTYPE) {
        const char *type_name = "grahaos.pipe.bytes.v1";
        size_t n = 0; while (type_name[n]) n++;
        uint64_t h = fnv1a_hash64(type_name, n);
        rc_up = chan_create(h, CHAN_MODE_BLOCKING, 64,
                            caller_pid, &up_rd_tok, &up_wr_tok);
    }
    if (rc_up < 0) {
        cap_object_destroy((uint32_t)irq_idx);
        cap_object_destroy((uint32_t)mmio_idx);
        cap_object_destroy(ds_rd_obj_idx);
        cap_object_destroy(ds_wr_obj_idx);
        drv_irq_channel_destroy(ic);
        spinlock_release(&g_userdrv_registry_lock);
        return rc_up;
    }
    uint32_t up_rd_obj_idx = cap_token_idx(up_rd_tok);
    uint32_t up_wr_obj_idx = cap_token_idx(up_wr_tok);
    // Daemon keeps the READ end (up_rd_tok); strip the WRITE slot from its
    // handle table — kernel proxy holds the WRITE cap_object kernel-side.
    spinlock_acquire(&cur->cap_handles.lock);
    for (uint32_t s = 0; s < cur->cap_handles.capacity; s++) {
        if (cur->cap_handles.entries[s].object_idx == up_wr_obj_idx) {
            spinlock_release(&cur->cap_handles.lock);
            cap_handle_remove(&cur->cap_handles, s);
            spinlock_acquire(&cur->cap_handles.lock);
            break;
        }
    }
    spinlock_release(&cur->cap_handles.lock);

    // -------- Insert MMIO + IRQ caps in caller's handle table --------
    uint32_t mmio_slot = 0, irq_slot = 0;
    if (cap_handle_insert(&cur->cap_handles, (uint32_t)mmio_idx, 0, &mmio_slot) < 0 ||
        cap_handle_insert(&cur->cap_handles, (uint32_t)irq_idx, 0, &irq_slot) < 0) {
        cap_object_destroy((uint32_t)irq_idx);
        cap_object_destroy((uint32_t)mmio_idx);
        cap_object_destroy(ds_rd_obj_idx);
        cap_object_destroy(ds_wr_obj_idx);
        drv_irq_channel_destroy(ic);
        spinlock_release(&g_userdrv_registry_lock);
        return CAP_V2_ENOMEM;
    }
    cap_object_t *mmio_obj = g_cap_object_ptrs[mmio_idx];
    cap_object_t *irq_obj  = g_cap_object_ptrs[irq_idx];
    uint32_t mmio_gen = mmio_obj ? __atomic_load_n(&mmio_obj->generation, __ATOMIC_ACQUIRE) : 0;
    uint32_t irq_gen  = irq_obj  ? __atomic_load_n(&irq_obj->generation,  __ATOMIC_ACQUIRE) : 0;

    // -------- Assign IRQ vector + PIC-unmask --------
    uint8_t vector = 0;
    if (entry_idx < (USERDRV_VEC_MAX - USERDRV_VEC_BASE + 1)) {
        vector = (uint8_t)(USERDRV_VEC_BASE + entry_idx);
    }
    // Find the underlying pci entry to learn the legacy IRQ line.
    pci_table_entry_t *pe = pci_table_find_by_address(e->pci_addr);
    if (pe && pe->irq_line < 16 && vector != 0) {
        // Belt-and-suspenders PIC unmask. Real delivery comes from the
        // IOAPIC redirection table below (PIC is masked globally in LAPIC
        // mode), but matching the death-path's pic_mask keeps the API
        // symmetric and is harmless if PIC is later re-enabled.
        pic_unmask_irq(pe->irq_line);

        // Phase 21.1: program the IOAPIC redirection entry. PCI INTx is
        // level-triggered active-low. Destination = BSP (LAPIC ID 0). Then
        // unmask so the entry can fire. The MMIO region the daemon will
        // poke (BAR0) is mapped via mmio_vmo_create above.
        ioapic_route_irq(pe->irq_line, vector, 0 /*lapic_id*/,
                         true /*level*/, true /*active_low*/);
        ioapic_unmask_irq(pe->irq_line);
    }

    // -------- Commit entry + audit --------
    e->driver_owner_pid    = caller_pid;
    e->irq_vector          = vector;
    e->mmio_cap_idx        = (uint32_t)mmio_idx;
    e->irq_chan_cap_idx    = (uint32_t)irq_idx;
    e->down_chan_read_idx  = ds_rd_obj_idx;
    e->down_chan_write_idx = ds_wr_obj_idx;
    e->up_chan_read_idx    = up_rd_obj_idx;
    e->up_chan_write_idx   = up_wr_obj_idx;
    e->irq_channel         = ic;
    e->registered_at_tsc   = tsc_is_ready() ? rdtsc() : 0;
    e->irq_count           = 0;

    spinlock_release(&g_userdrv_registry_lock);

    // Fill caller's drv_caps_t (kernel-side; dispatcher copies to user).
    out_caps_kern->mmio_handle        = cap_token_pack(mmio_gen, (uint32_t)mmio_idx, 0).raw;
    out_caps_kern->irq_channel_handle = cap_token_pack(irq_gen, (uint32_t)irq_idx, 0).raw;
    out_caps_kern->downstream_handle  = ds_wr_tok.raw;
    out_caps_kern->upstream_handle    = up_rd_tok.raw;   // Phase 21.1: daemon reads from this
    out_caps_kern->bar_phys           = e->bar_phys;
    out_caps_kern->bar_size           = e->bar_size;
    out_caps_kern->pci_addr           = e->pci_addr;
    out_caps_kern->vendor_id          = vendor_id;
    out_caps_kern->device_id          = device_id;
    out_caps_kern->irq_vector         = vector;

    audit_write_drv_registered(caller_pid, e->pci_addr, vendor_id, device_id, vector);
    klog(KLOG_INFO, SUBSYS_CORE,
         "userdrv: pid=%d claimed pci=0x%06x vendor=0x%04x device=0x%04x irq=%u",
         (int)caller_pid, (unsigned)e->pci_addr,
         (unsigned)vendor_id, (unsigned)device_id, (unsigned)vector);
    return 0;
}

// ===========================================================================
// SYS_DRV_IRQ_WAIT backend.
// ===========================================================================
long userdrv_irq_wait(int32_t caller_pid, cap_token_t handle,
                      drv_irq_msg_t *out_msgs_kern, uint32_t max_msgs,
                      uint32_t timeout_ms) {
    if (!out_msgs_kern || max_msgs == 0) return CAP_V2_EFAULT;
    if (max_msgs > DRV_IRQ_RING_SIZE) max_msgs = DRV_IRQ_RING_SIZE;

    cap_object_t *obj = cap_token_resolve(caller_pid, handle, RIGHT_RECV);
    if (!obj) return CAP_V2_EBADF;
    if (obj->kind != CAP_KIND_IRQ_CHANNEL) return CAP_V2_EBADF;
    drv_irq_channel_t *c = (drv_irq_channel_t *)obj->kind_data;
    if (!c) return CAP_V2_EBADF;
    if (c->dead) return -32;  // -ESHUTDOWN equivalent

    // Fast path: try to drain immediately.
    uint32_t got = userdrv_irq_drain(c, out_msgs_kern, max_msgs);
    if (got > 0) return (long)got;

    // Poll-only mode.
    if (timeout_ms == 0) return 0;

    // Block on the embedded channel's read_waiters list. We rely on
    // sched_block_on_channel + sched_wake_one_on_channel from sched.c
    // (Phase 17 chan_recv uses the same primitives). The ISR (U9) calls
    // sched_wake_one_on_channel(&c->base.read_waiters, 0) after posting.
    task_t *cur = sched_get_task(caller_pid);
    if (!cur) return CAP_V2_EINVAL;

    // Convert ms to ns (UINT32_MAX → block-forever sentinel passed as 0 to
    // sched_block_on_channel's "no timeout" semantics).
    uint64_t timeout_ns = (timeout_ms == 0xFFFFFFFFu)
        ? 0
        : (uint64_t)timeout_ms * 1000000ull;

    // sched_block_on_channel(channel*, dir, timeout_ns, list_head). Direction
    // bit is informational; pass CHAN_ENDPOINT_READ since the daemon is the
    // reader. The channel pointer doubles as a debug breadcrumb in oops.
    int rc = sched_block_on_channel(&c->base, CHAN_ENDPOINT_READ,
                                    timeout_ns, &c->base.read_waiters);
    if (rc == -110 /* ETIMEDOUT */) {
        // After timeout, drain whatever happens to be there (non-empty
        // ring + missed wake = race; recover the messages).
        uint32_t late = userdrv_irq_drain(c, out_msgs_kern, max_msgs);
        return (long)late;  // 0 on true timeout, >0 if races against post.
    }
    if (rc < 0) return rc;

    // Woken up — drain.
    if (c->dead) return -32;
    got = userdrv_irq_drain(c, out_msgs_kern, max_msgs);
    return (long)got;
}

// ===========================================================================
// SYS_MMIO_VMO_CREATE backends — op=CREATE and op=PHYS_QUERY.
// ===========================================================================
long userdrv_mmio_vmo_create(int32_t caller_pid, uint64_t phys, uint64_t size,
                             uint32_t flags) {
    (void)flags;  // VMO_FLAG_CACHE_DISABLE is forced; flags currently unused.
    if ((phys & 0xFFFu) || (size & 0xFFFu) || size == 0) return CAP_V2_EINVAL;

    task_t *cur = sched_get_task(caller_pid);
    if (!cur) return CAP_V2_EINVAL;
    if (!pledge_allows(cur, PLEDGE_CLASS_SYS_CONTROL)) return CAP_V2_EPLEDGE;

    // Validate that the requested phys range falls within a BAR of a device
    // OWNED BY THIS CALLER. We walk g_userdrv_entries for entries whose
    // driver_owner_pid == caller_pid; for each, check phys+size fits in any
    // of that device's BARs.
    bool owned = false;
    spinlock_acquire(&g_userdrv_registry_lock);
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        userdrv_entry_t *e = &g_userdrv_entries[i];
        if (e->driver_owner_pid != caller_pid) continue;
        pci_table_entry_t *pe = pci_table_find_by_address(e->pci_addr);
        if (!pe) continue;
        for (int b = 0; b < 6; b++) {
            uint64_t bar_base = pe->bars[b];
            uint64_t bar_size = pe->bar_sizes[b];
            if (bar_size == 0) continue;
            if (phys >= bar_base && phys + size <= bar_base + bar_size) {
                owned = true;
                break;
            }
        }
        if (owned) break;
    }
    spinlock_release(&g_userdrv_registry_lock);
    if (!owned) {
        audit_write_mmio_denied(caller_pid, phys, size, "not_owned_bar");
        return -13;  // -EACCES
    }

    vmo_t *v = mmio_vmo_create(phys, size, caller_pid);
    if (!v) return CAP_V2_ENOMEM;

    int32_t audience[CAP_AUDIENCE_MAX + 1];
    audience[0] = caller_pid;
    audience[1] = PID_NONE;
    int idx = cap_object_create(CAP_KIND_MMIO_REGION,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_INSPECT |
                                    RIGHT_REVOKE,
                                audience, 0, (uintptr_t)v, caller_pid,
                                CAP_OBJECT_IDX_NONE);
    if (idx < 0) {
        vmo_free(v);
        return idx;
    }
    v->cap_object_idx = (uint32_t)idx;
    uint32_t slot = 0;
    int rc_ins = cap_handle_insert(&cur->cap_handles, (uint32_t)idx, 0, &slot);
    if (rc_ins < 0) {
        cap_object_destroy((uint32_t)idx);
        return rc_ins;
    }
    cap_object_t *o = g_cap_object_ptrs[idx];
    uint32_t gen = o ? __atomic_load_n(&o->generation, __ATOMIC_ACQUIRE) : 0;
    return (long)cap_token_pack(gen, (uint32_t)idx, 0).raw;
}

long userdrv_mmio_vmo_phys_query(int32_t caller_pid, cap_token_t handle,
                                 uint32_t page_idx, uint64_t *phys_out_kern) {
    if (!phys_out_kern) return CAP_V2_EFAULT;
    cap_object_t *obj = cap_token_resolve(caller_pid, handle, RIGHT_INSPECT);
    if (!obj) return CAP_V2_EBADF;
    // Accept either a regular VMO or an MMIO region for this query — drivers
    // need physical addresses for both DMA rings (regular contiguous VMOs)
    // and BAR mappings.
    if (obj->kind != CAP_KIND_VMO && obj->kind != CAP_KIND_MMIO_REGION)
        return CAP_V2_EBADF;
    vmo_t *v = (vmo_t *)obj->kind_data;
    uint64_t phys = vmo_get_phys(v, page_idx);
    if (phys == 0) return CAP_V2_EINVAL;
    *phys_out_kern = phys;
    return 0;
}

// ===========================================================================
// ISR dispatch (Phase 21 U9). Called from interrupts.c for vectors 50..65.
// Runs with interrupts already disabled. Lock-free fast path:
//   1. Find the userdrv_entry by vector (no lock — entries are stable once
//      registered; race with owner-death just yields irq_channel==NULL,
//      which we treat as "drop silently").
//   2. Read the entry's irq_channel pointer (RELAXED — kernel-side write
//      under registry lock; ISR-side read accepts a stale NULL benignly).
//   3. Post the message via userdrv_isr_post (lock-free SPSC).
//   4. Wake the daemon if it's blocked on the channel's read_waiters.
// ===========================================================================
void userdrv_isr_dispatch(uint8_t vector) {
    userdrv_entry_t *e = userdrv_find_by_vector(vector);
    if (!e) return;  // No owner — drop. LAPIC EOI sent by interrupt_handler.
    drv_irq_channel_t *c = e->irq_channel;
    if (!c) return;
    drv_irq_msg_t msg;
    msg.vector = vector;
    msg._pad0[0] = msg._pad0[1] = msg._pad0[2] = msg._pad0[3] = 0;
    msg._pad0[4] = msg._pad0[5] = msg._pad0[6] = 0;
    msg.timestamp_tsc = tsc_is_ready() ? rdtsc() : 0;
    bool ok = userdrv_isr_post(c, &msg);
    e->irq_count++;
    if (ok) {
        // Wake at most one waiter — daemon drains all available messages
        // on its next drv_irq_wait call.
        sched_wake_one_on_channel(&c->base.read_waiters, 0);
    } else {
        // Phase 21.1: throttled audit. At most 1 AUDIT_IRQ_DROPPED emit
        // per channel per wall-second. Use g_tsc_hz (calibrated at boot)
        // to convert the 1-second window into TSC ticks. Compare-and-
        // exchange so two CPUs racing on the same overflow only emit once.
        extern uint64_t g_tsc_hz;
        uint64_t now = tsc_is_ready() ? rdtsc() : 0;
        uint64_t last = __atomic_load_n(&c->last_dropped_audit_tsc,
                                         __ATOMIC_ACQUIRE);
        // Bypass throttle on the very first drop (last==0) so the operator
        // gets immediate visibility; afterwards enforce the 1-second gap.
        bool emit = (now == 0) || (last == 0) ||
                    (g_tsc_hz != 0 && (now - last) > g_tsc_hz);
        if (emit) {
            // Race resolution: the first CAS winner emits, others skip.
            if (__atomic_compare_exchange_n(&c->last_dropped_audit_tsc,
                                            &last, now,
                                            false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                audit_write_irq_dropped(
                    e->pci_addr, vector,
                    __atomic_load_n(&c->dropped, __ATOMIC_RELAXED));
            }
        }
    }
}

// ===========================================================================
// Owner-death cleanup (Phase 21 U10).
// Called from sched_reap_zombie / SYS_EXIT path. Walks the registry,
// reaping every device owned by the dying PID.
// ===========================================================================
void userdrv_on_owner_death(int32_t pid) {
    if (pid <= 0) return;
    spinlock_acquire(&g_userdrv_registry_lock);
    for (uint32_t i = 0; i < g_pci_table_count; i++) {
        userdrv_entry_t *e = &g_userdrv_entries[i];
        if (e->driver_owner_pid != pid) continue;

        // 1. Mask the IRQ line at both the legacy PIC and the IOAPIC so
        //    no further ISRs fire. IOAPIC is the actually-routed path under
        //    LAPIC mode (Phase 21.1); PIC mask is symmetric belt-and-braces.
        pci_table_entry_t *pe = pci_table_find_by_address(e->pci_addr);
        if (pe && pe->irq_line < 16) {
            ioapic_mask_irq(pe->irq_line);
            pic_mask_irq(pe->irq_line);
        }
        // 2. Mark the IRQ channel dead so any in-flight ISR drops cleanly.
        if (e->irq_channel) {
            e->irq_channel->dead = 1;
        }
        // 2b. Phase 22 Stage F: notify the named-channel registry so any
        //     /sys/net/* publish slot owned by this pid is torn down and
        //     subscribers see -EPIPE.
        {
            extern void rawnet_on_peer_death(int32_t pid);
            rawnet_on_peer_death(pid);
        }
        // 3. Audit BEFORE we revoke + zero (so the entry's pci_addr is
        //    still readable here).
        audit_write_drv_died(pid, e->pci_addr, "exit");
        klog(KLOG_INFO, SUBSYS_CORE,
             "userdrv: pid=%d died; freed pci=0x%06x irq=%u",
             (int)pid, (unsigned)e->pci_addr, (unsigned)e->irq_vector);

        // 4. Revoke MMIO + IRQ + downstream + upstream caps. revoke() flips
        //    obj->deleted=1 and bumps generation so existing handle resolves
        //    fail. Phase 21.1 added the upstream pair.
        if (e->mmio_cap_idx)        cap_object_revoke(e->mmio_cap_idx);
        if (e->irq_chan_cap_idx)    cap_object_revoke(e->irq_chan_cap_idx);
        if (e->down_chan_read_idx)  cap_object_revoke(e->down_chan_read_idx);
        if (e->down_chan_write_idx) cap_object_revoke(e->down_chan_write_idx);
        if (e->up_chan_read_idx)    cap_object_revoke(e->up_chan_read_idx);
        if (e->up_chan_write_idx)   cap_object_revoke(e->up_chan_write_idx);

        // 4b. Phase 23 S1 (P22.G.4 fix): synchronously DESTROY the MMIO and
        //     IRQ-channel cap_objects so the kind-specific deactivate hooks
        //     run immediately, releasing kernel-side resources before the
        //     next spawn. The down/up channel caps are deliberately left
        //     to the lazy cleanup path because they may have a live peer
        //     (publisher/connector) holding the OTHER endpoint via
        //     /sys/net/rawnet — destroying them here would EPIPE the peer
        //     mid-flight. cap_object_destroy is idempotent (NULL check on
        //     the ptr slot) so the eventual handle_table cleanup is a no-op.
        //     Caveat: this fix covers the kernel-side resource lifecycle;
        //     production validation across N-cycle stress is via the
        //     dedicated user/tests/userdrv_respawn_stress test (run
        //     interactively, not in the gate sequence yet — see Phase 23
        //     production cutover).
        if (e->mmio_cap_idx)        cap_object_destroy(e->mmio_cap_idx);
        if (e->irq_chan_cap_idx)    cap_object_destroy(e->irq_chan_cap_idx);

        // 4c. Phase 23 P23.deferred.2: AHCI-class drivers (vendor=0x8086,
        //     device=0x2922 on QEMU) overwrote our PxCLB/PxFB during
        //     port_init. The userspace daemon's DMA VMOs are now unmapped,
        //     so kernel ahci_read/ahci_write would touch invalid memory.
        //     Restore the kernel's saved port state and restart the command
        //     engine. Effectively a no-op if no AHCI driver was ever
        //     active. Cheap (32 register writes worst case).
        if (pe && pe->class_code == 0x01) {
            extern void ahci_restore_after_userdrv_death(void);
            ahci_restore_after_userdrv_death();
        }

        // 5. Clear the entry — device becomes claimable again.
        e->driver_owner_pid    = 0;
        e->irq_vector          = 0;
        e->mmio_cap_idx        = 0;
        e->irq_chan_cap_idx    = 0;
        e->down_chan_read_idx  = 0;
        e->down_chan_write_idx = 0;
        e->up_chan_read_idx    = 0;
        e->up_chan_write_idx   = 0;
        e->irq_channel         = NULL;
        e->registered_at_tsc   = 0;
        e->irq_count           = 0;
    }
    spinlock_release(&g_userdrv_registry_lock);
}
