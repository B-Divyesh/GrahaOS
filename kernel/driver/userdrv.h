// kernel/driver/userdrv.h
//
// Phase 21 — Userspace driver framework.
//
// This module is the kernel-side substrate for moving device drivers out of
// the kernel and into userspace daemons. The three pillars:
//
//   1. `userdrv_entry_t` — one per enumerated PCI device. Tracks whether a
//      userspace daemon has claimed the device, and if so its PID, the IRQ
//      vector routed to it, and handles to its MMIO + IRQ + downstream
//      channels.
//
//   2. `drv_irq_channel_t` — a specialised channel variant. Regular Phase 17
//      channels take a spinlock on `chan_send`; that is too expensive in an
//      ISR. This variant uses a fixed 64-entry single-producer / single-
//      consumer (SPSC) ring with `_Atomic` head/tail/dropped counters. The
//      ISR (sole producer) posts lock-free; the daemon (sole consumer)
//      drains lock-free. The embedded `channel_t` is reused only for cap
//      integration (so the channel handle is a regular CAP_KIND_IRQ_CHANNEL
//      cap_token) and for the wake-queue (daemon blocks on
//      `embedded.read_waiters`; ISR calls `sched_wake_one_on_channel`).
//
//   3. `drv_caps_t` — the structure returned by `sys_drv_register` carrying
//      the three handles + BAR metadata + IRQ vector.
//
// Phase 21 implementation lands in stages: U4 brings up the SPSC primitives,
// U5 the ownership table, U6/U7/U8 the syscalls, U9 the ISR forwarder,
// U10 the death-cleanup hook.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../ipc/channel.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/cpu/pci_enum.h"

// ===========================================================================
// drv_irq_msg_t — 16 bytes; produced by the ISR, consumed by the daemon via
// SYS_DRV_IRQ_WAIT. Small + fixed-size so ISR-side cost is constant.
// ===========================================================================
typedef struct drv_irq_msg {
    uint8_t  vector;          //   0    IDT vector that fired (50..65)
    uint8_t  _pad0[7];        //   1..7 alignment padding (must be zero)
    uint64_t timestamp_tsc;   //   8..15 rdtsc at ISR entry
} drv_irq_msg_t;

_Static_assert(sizeof(drv_irq_msg_t) == 16, "drv_irq_msg_t must be 16 bytes");

#define DRV_IRQ_RING_SIZE   64u   // power-of-two; SPSC ring depth.
#define DRV_IRQ_RING_MASK   (DRV_IRQ_RING_SIZE - 1u)

// ===========================================================================
// drv_irq_channel_t — embedded channel_t for cap integration + the SPSC ring.
// Memory ordering rules (every access tagged inline):
//   ISR producer:    relaxed-load tail, bounds-check, write entry, RELEASE-
//                    store head. If full, RELAXED-fetch-add dropped.
//   Daemon consumer: ACQUIRE-load head, copy entries [tail..min(head, ...)],
//                    RELEASE-store tail.
// The embedded channel_t.lock is NEVER taken in either path (only by the
// owner-death cleanup which marks the channel dead).
// ===========================================================================
typedef struct drv_irq_channel {
    channel_t      base;                          // For cap integration + wake queue.
    drv_irq_msg_t  ring[DRV_IRQ_RING_SIZE];       // 64 * 16 = 1024 bytes.
    _Atomic uint32_t head;                        // Producer index (modulo SIZE).
    _Atomic uint32_t tail;                        // Consumer index (modulo SIZE).
    _Atomic uint64_t dropped;                     // Cumulative ring-overflow count.
    uint64_t       last_dropped_audit_tsc;        // Throttle 1/sec/channel.
    uint8_t        dead;                          // Set by owner-death cleanup.
    uint8_t        _pad[7];
} drv_irq_channel_t;

// ===========================================================================
// Lifecycle.
// ===========================================================================
// Initialise the userdrv subsystem: slab caches, lock, ownership table.
// Called from kmain after pci_enumerate_all and channel_subsystem_init.
void userdrv_init(void);

// Allocate a fresh drv_irq_channel_t with the embedded channel_t initialised
// (type_hash = 0, mode = BLOCKING, capacity = DRV_IRQ_RING_SIZE — these
// fields are set so the cap-handle accounting matches a regular channel even
// though the SPSC ring is what holds messages). owner_pid becomes the
// channel_t.owner_pid.
drv_irq_channel_t *drv_irq_channel_create(int32_t owner_pid);

// Tear down a drv_irq_channel_t. Wakes any blocked consumer with -ESHUTDOWN
// (so SYS_DRV_IRQ_WAIT returns cleanly), marks `dead`, frees storage.
void drv_irq_channel_destroy(drv_irq_channel_t *c);

// ===========================================================================
// Hot paths.
// ===========================================================================
// ISR-side enqueue. Returns true on success, false if the ring was full
// (caller atomically increments dropped via the call itself). Runs with
// interrupts already disabled. ~10 instructions on the success path.
bool userdrv_isr_post(drv_irq_channel_t *c, const drv_irq_msg_t *msg);

// Daemon-side drain. Copies up to `max` messages into out_msgs, returns
// the number copied (0..max). Lock-free. Caller (sys_drv_irq_wait) handles
// the blocking semantics around this call.
uint32_t userdrv_irq_drain(drv_irq_channel_t *c, drv_irq_msg_t *out_msgs,
                           uint32_t max);

// ===========================================================================
// drv_caps_t — populated by sys_drv_register, returned via R10 user pointer.
// Layout MUST stay in sync with user/syscalls.h::drv_caps_t.
//
// Phase 21.1: added `upstream_handle` (CAP_KIND_CHANNEL READ end). The
// downstream channel carries daemon→kernel traffic (RX frames + ANNOUNCE);
// the upstream channel carries kernel→daemon traffic (TX_NOTIFY messages
// pointing at slots in the shared DMA ring). Struct grows 56 → 64 bytes.
// ===========================================================================
typedef struct drv_caps {
    uint64_t mmio_handle;        // cap_token raw, CAP_KIND_MMIO_REGION
    uint64_t irq_channel_handle; // cap_token raw, CAP_KIND_IRQ_CHANNEL
    uint64_t downstream_handle;  // cap_token raw, CAP_KIND_CHANNEL (daemon's WRITE end)
    uint64_t upstream_handle;    // Phase 21.1: cap_token raw, CAP_KIND_CHANNEL (daemon's READ end)
    uint64_t bar_phys;           // physical base of mapped BAR
    uint64_t bar_size;           // size of BAR in bytes
    uint32_t pci_addr;           // bus<<16 | dev<<8 | func
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq_vector;         // IDT vector assigned to this device
    uint8_t  _pad[7];
} drv_caps_t;

_Static_assert(sizeof(drv_caps_t) == 64, "drv_caps_t layout mismatch (must be 64 bytes after Phase 21.1)");

// ===========================================================================
// userdrv_entry_t — one per enumerated PCI function. Tracks userspace
// ownership state. Lives in g_userdrv_entries[], indexed parallel to
// g_pci_table[] — entry i corresponds to pci device i.
// ===========================================================================
typedef struct userdrv_entry {
    uint32_t pci_addr;             // bus<<16 | dev<<8 | func (cached from g_pci_table)
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  is_claimable;         // 1 = a kernel-side `_expose_to_userdrv` has tagged
                                   //     this entry as eligible for sys_drv_register.
    uint8_t  _pad0;
    int32_t  driver_owner_pid;     // 0 = unclaimed; otherwise the daemon's PID.
    uint8_t  irq_vector;           // IDT vector assigned at register time (0 = unbound).
    uint8_t  _pad1[3];
    uint32_t mmio_cap_idx;         // cap_object idx for the MMIO VMO (0 if unbound).
    uint32_t irq_chan_cap_idx;     // cap_object idx for the IRQ channel.
    uint32_t down_chan_read_idx;   // cap_object idx for the downstream channel READ end
                                   //   (kernel-held; e1000_proxy reads frames here).
    uint32_t down_chan_write_idx;  // cap_object idx for the downstream channel WRITE end
                                   //   (passed to daemon).
    uint32_t up_chan_read_idx;     // Phase 21.1: cap_object idx for upstream READ end
                                   //   (passed to daemon for TX_NOTIFY drain).
    uint32_t up_chan_write_idx;    // Phase 21.1: cap_object idx for upstream WRITE end
                                   //   (kernel-held; e1000_proxy writes TX requests here).
    drv_irq_channel_t *irq_channel;  // back-pointer for ISR fast lookup.
    uint64_t registered_at_tsc;
    uint64_t irq_count;            // total IRQs delivered since registration.
    uint64_t bar_phys;             // primary BAR base (cached from g_pci_table[i].bars[0]).
    uint64_t bar_size;             // primary BAR size.
} userdrv_entry_t;

extern userdrv_entry_t g_userdrv_entries[PCI_TABLE_MAX];
extern spinlock_t g_userdrv_registry_lock;

// Tag a PCI device as claimable by a userspace driver. Called from
// kernel-side stubs (e.g., e1000_expose_to_userdrv) that previously held the
// device but have stripped their ownership in Phase 21. Identifies the entry
// by (vendor_id, device_id) — first match wins (PCI guarantees uniqueness
// per bus/dev/func, but devices like e1000 typically appear once).
//
// Returns 0 on success, -ENODEV if no entry matches.
int userdrv_mark_claimable(uint16_t vendor_id, uint16_t device_id);

// Find the userdrv entry whose IRQ vector matches `vector`, returning a
// pointer or NULL. Used by the universal ISR forwarder (U9) for fast lookup.
userdrv_entry_t *userdrv_find_by_vector(uint8_t vector);

// USERDRV-allocated IDT vector range. Vector 32 = timer, 33 = keyboard,
// 48 = IPI_VEC_WAKEUP. 50..65 (16 slots) is the userdrv pool — one per
// claimable PCI device. Vector i = USERDRV_VEC_BASE + entry_index.
#define USERDRV_VEC_BASE  50u
#define USERDRV_VEC_MAX   65u

// ===========================================================================
// ISR dispatch (called from arch/x86_64/cpu/interrupts.c for vectors 50..65).
// Looks up the userdrv_entry whose irq_vector == vector, posts a fresh
// drv_irq_msg into its SPSC ring (lock-free), wakes the daemon if blocked,
// increments irq_count. If the entry has been reaped (driver_owner_pid==0)
// the message is silently dropped — the LAPIC EOI is still sent by the
// outer interrupt_handler.
// ===========================================================================
void userdrv_isr_dispatch(uint8_t vector);

// Owner-death cleanup hook. Called from sched_reap_zombie (and any other
// task-exit path) with the exiting PID. Walks g_userdrv_entries; for each
// matching driver_owner_pid: PIC-masks the IRQ line, revokes the MMIO/IRQ/
// downstream caps, audits AUDIT_DRV_DIED, clears the entry. Idempotent.
void userdrv_on_owner_death(int32_t pid);

// SYS_DRV_REGISTER backend. Resolves and validates the caller's pledge +
// CAP_KIND_DRIVER_REGISTRAR token (caller-side), then performs the device
// claim, allocates MMIO VMO + drv_irq_channel + downstream channel pair,
// inserts the MMIO + IRQ + downstream-WRITE caps into caller's handle table,
// assigns an IRQ vector, PIC-unmasks the line, and fills *out_caps.
//
// The downstream channel's READ endpoint cap_object is created but NOT
// inserted into any handle table — it stays kernel-resident; subsystems
// that consume daemon frames (e.g., e1000_proxy in U14) look it up via
// the entry's `down_chan_read_idx` field.
//
// Returns 0 on success, negative CAP_V2_* / -ENODEV / -EBUSY on failure.
int userdrv_register_device(int32_t caller_pid,
                            uint16_t vendor_id, uint16_t device_id,
                            uint8_t device_class,
                            drv_caps_t *out_caps_kern);

// SYS_DRV_IRQ_WAIT backend. Resolves the IRQ-channel cap, drains up to
// max_msgs entries (lock-free), or blocks on the embedded channel's
// read_waiters list when the ring is empty (subject to timeout_ms).
// Returns the count copied (0..max_msgs), 0 on timeout, negative on error.
long userdrv_irq_wait(int32_t caller_pid, cap_token_t handle,
                      drv_irq_msg_t *out_msgs_kern, uint32_t max_msgs,
                      uint32_t timeout_ms);

// SYS_MMIO_VMO_CREATE backend (op=CREATE). Validates phys+size against the
// caller's owned BARs, allocates an MMIO-backed VMO, wraps in a fresh
// CAP_KIND_MMIO_REGION cap_object, inserts in caller's handle table.
// Returns the cap_token_t on success or negative errno.
long userdrv_mmio_vmo_create(int32_t caller_pid, uint64_t phys, uint64_t size,
                             uint32_t flags);

// SYS_MMIO_VMO_CREATE backend (op=PHYS_QUERY). Resolves a VMO handle owned
// by the caller and writes the physical address backing page `page_idx`
// to *phys_out_kern (kernel pointer; copy_to_user happens in dispatcher).
// Returns 0 on success or negative errno.
long userdrv_mmio_vmo_phys_query(int32_t caller_pid, cap_token_t handle,
                                 uint32_t page_idx, uint64_t *phys_out_kern);
