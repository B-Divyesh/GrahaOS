// user/drivers/e1000d.c
//
// Phase 21.1 — Userspace Intel E1000 (82540EM) NIC driver. See e1000d.h
// for the channel-message schema and the rationale for shared-VMO ring
// frame transport (rather than inline channel payloads).
//
// Lifecycle:
//   1. _start: pledge self-check, drv_register, drv_mmio_map BAR, allocate
//      RX/TX rings + descriptor rings via drv_dma_alloc.
//   2. Reset device, read MAC from EEPROM, program RAL/RAH for receive
//      filtering, init RX/TX descriptor rings.
//   3. Send ANNOUNCE message on caps.downstream_handle so the kernel-side
//      proxy can cache MAC + link + ring physical addresses.
//   4. Enable interrupts (IMS = RXT0 | RXSEQ | LSC | TXDW).
//   5. Main loop:
//        - drv_irq_wait(caps.irq_channel_handle, msgs, 16, 100)
//        - For each IRQ: read ICR, drain RX descriptors → RX_NOTIFY,
//          handle LSC link-state changes, ack TX completions
//        - Non-blocking chan_recv on caps.upstream_handle for TX_NOTIFY:
//          copy slot data into next free TX descriptor + bump TDT.
//   6. On any -EPIPE / -EBADF from channels (kernel revoked our caps),
//      exit cleanly with status 1 so init can respawn.
//
// Phase 21.1 U9 lands the skeleton through ANNOUNCE; U10 fills in the
// RX/TX/IRQ main loop.

#include "e1000d.h"
#include "../libdriver.h"
#include "../libnet/libnet.h"     // Phase 22 Stage A: /sys/net/rawframe publish.
#include "../libnet/rawframe.h"   // Phase 22 Stage B: rawframe wire schema.
#include "../syscalls.h"

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);

// Phase 22 Stage A/B/C: libnet-published rawframe service + per-peer RX/TX
// fanout. Stage A proved publish/connect; Stage B landed ANNOUNCE with shared
// VMO hand-off; Stage C lights up the actual frame path. During Stages B-E
// per plan D8 we dual-send to both the kernel proxy (old path) and every
// rawframe peer (new path). Stage F deletes the kernel proxy.
#define E1000D_MAX_RAWFRAME_PEERS  4
typedef struct rawframe_peer {
    uint8_t       in_use;
    int32_t       connector_pid;
    cap_token_u_t rd_req;       // netd → us (TX_REQ)
    cap_token_u_t wr_resp;      // us → netd (RX_NOTIFY / LINK_*)
} rawframe_peer_t;

static libnet_service_ctx_t s_rawframe_svc;
static int                  s_rawframe_ready = 0;
static int                  s_rawframe_accepts = 0;
static rawframe_peer_t      s_rawframe_peers[E1000D_MAX_RAWFRAME_PEERS];

// ====================================================================
// MMIO accessors. The mapped BAR pointer is set in _start after
// drv_mmio_map returns. Volatile reads/writes; mfence as needed for
// device ordering.
// ====================================================================
static volatile uint8_t *s_mmio = (volatile uint8_t *)0;

// ====================================================================
// FNV-1a 64-bit hash. Phase 21.1: kernel sys_drv_register creates the
// downstream + upstream channels with type_hash = fnv1a_hash64(
// "grahaos.pipe.bytes.v1") (substrate fallback path); chan_send fails with
// -EPROTOTYPE if the message's type_hash doesn't match exactly. So the
// daemon computes the same hash at startup and stamps every outgoing
// message header.
// ====================================================================
static uint64_t fnv1a_hash64_local(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; s[i]; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t s_chan_type_hash = 0;

static inline uint32_t mmio_read(uint32_t off) {
    return *(volatile uint32_t *)(s_mmio + off);
}
static inline void mmio_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(s_mmio + off) = val;
}
static inline void mfence(void) { asm volatile("mfence" ::: "memory"); }
static inline void cpu_pause(void) { asm volatile("pause"); }

// ====================================================================
// EEPROM MAC read. Mirrors the kernel driver's three-word EERD dance.
// On EERD timeout we fall back to RAL/RAH (already programmed by BIOS).
// ====================================================================
static void read_mac_eeprom(uint8_t mac[6]) {
    for (int i = 0; i < 3; i++) {
        mmio_write(E1000_EERD, ((uint32_t)i << 8) | E1000_EERD_START);

        uint32_t val = 0;
        int timeout = 10000;
        do {
            val = mmio_read(E1000_EERD);
            if (--timeout <= 0) break;
        } while (!(val & E1000_EERD_DONE));

        if (timeout <= 0) {
            // Fallback: BIOS-programmed RAL/RAH.
            uint32_t ral = mmio_read(E1000_RAL);
            uint32_t rah = mmio_read(E1000_RAH);
            mac[0] = ral & 0xFF; mac[1] = (ral >> 8) & 0xFF;
            mac[2] = (ral >> 16) & 0xFF; mac[3] = (ral >> 24) & 0xFF;
            mac[4] = rah & 0xFF; mac[5] = (rah >> 8) & 0xFF;
            printf("[e1000d] MAC fallback via RAL/RAH (EERD timeout)\n");
            return;
        }

        uint16_t word = (val >> 16) & 0xFFFF;
        mac[i * 2]     = word & 0xFF;
        mac[i * 2 + 1] = (word >> 8) & 0xFF;
    }
}

// ====================================================================
// Device reset + bring-up. Mirrors arch/.../e1000.c lines 277-295.
// ====================================================================
static void device_reset_and_link(void) {
    uint32_t ctrl = mmio_read(E1000_CTRL);
    mmio_write(E1000_CTRL, ctrl | E1000_CTRL_RST);
    // ~10ms busy-wait — the device clears the reset bit when ready.
    for (volatile int i = 0; i < 100000; i++) cpu_pause();

    // Mask all device IRQs while we set up the rings.
    mmio_write(E1000_IMC, 0xFFFFFFFFu);
    (void)mmio_read(E1000_ICR);

    // SLU + ASDE: Set Link Up + Auto-Speed Detection.
    ctrl = mmio_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_RST;
    mmio_write(E1000_CTRL, ctrl);
}

// ====================================================================
// Daemon globals — allocated once at startup, used through the lifetime.
// Phase 21.1 U9 only allocates them and sends ANNOUNCE; U10 wires the
// main RX/TX loop on top.
// ====================================================================
static drv_caps_t s_caps;
static uint8_t    s_mac[6];

// Frame ring VMOs (shared with kernel proxy via phys addresses in ANNOUNCE,
// and with netd via VMO_CLONE_SHARED — see Phase 22 Stage B rawframe ANNOUNCE).
static uint8_t    *s_rx_ring_va = (uint8_t *)0;
static uint64_t    s_rx_ring_phys = 0;
static cap_token_u_t s_rx_ring_handle = {.raw = 0};    // Phase 22 Stage B
static uint8_t    *s_tx_ring_va = (uint8_t *)0;
static uint64_t    s_tx_ring_phys = 0;
static cap_token_u_t s_tx_ring_handle = {.raw = 0};    // Phase 22 Stage B

// Descriptor rings (separate from frame-data rings — the NIC's RDBAL/RDBAH
// programming wants the descriptor base, not the frame buffer base).
static e1000_rx_desc_t *s_rx_descs = (e1000_rx_desc_t *)0;
static uint64_t s_rx_descs_phys = 0;
static e1000_tx_desc_t *s_tx_descs = (e1000_tx_desc_t *)0;
static uint64_t s_tx_descs_phys = 0;

// Phase 21.1: zero-copy ring buffers. Instead of allocating 32 separate
// VMOs for "hw buffers", the descriptors point directly at the shared
// rx_ring_va / tx_ring_va slots. The NIC DMAs into those pages; the proxy
// reads from the same pages via HHDM. This collapses 32 cap objects into 0
// extra and eliminates the descriptor→hw-buf copy on RX. (Standard
// virtio-net pattern.)
//
// Round-robin tail cursors for RX/TX descriptor recycling.
static uint32_t s_rx_tail = 0;
static uint32_t s_tx_tail = 0;
// Last observed link state — used to emit LINK_UP / LINK_DOWN deltas.
static uint8_t  s_link_state_last = 0;

// ====================================================================
// Send the ANNOUNCE message on the downstream channel. The proxy will
// receive this on its first proxy_try_bind() poll and transition to
// PROXY_READY. Until ANNOUNCE arrives, all proxy queries return defaults.
// ====================================================================
static int send_announce(void) {
    chan_msg_user_t m;
    memset(&m, 0, sizeof(m));
    m.header.inline_len = (uint16_t)sizeof(e1000_msg_t);
    m.header.nhandles   = 0;
    m.header.flags      = 0;
    m.header.type_hash  = s_chan_type_hash;  // matches channel's type_hash
    e1000_msg_t *body = (e1000_msg_t *)m.inline_payload;
    body->op = E1000_OP_ANNOUNCE;
    for (int i = 0; i < 6; i++) body->mac[i] = s_mac[i];
    body->link_up      = (mmio_read(E1000_STATUS) & E1000_STATUS_LU) ? 1u : 0u;
    body->rx_ring_phys = s_rx_ring_phys;
    body->tx_ring_phys = s_tx_ring_phys;
    body->slot_count   = E1000D_RING_SLOTS;
    body->slot_size    = E1000D_SLOT_SIZE;

    cap_token_u_t wr; wr.raw = s_caps.downstream_handle;
    long rc = syscall_chan_send(wr, &m, 1000000000ull /* 1s timeout */);
    return (rc >= 0) ? 0 : (int)rc;
}

// ====================================================================
// Entry point. Phase 21.1 U9: register, reset, MAC, ANNOUNCE, then
// stub-loop forever. U10 will replace the stub with the IRQ + TX loop.
// ====================================================================
void _start(void) {
    printf("[e1000d] starting (Phase 21.1 daemon)\n");

    // Compute the type_hash once. Must match the kernel-side substrate's
    // chan_create fallback in userdrv.c (which uses the same string).
    s_chan_type_hash = fnv1a_hash64_local("grahaos.pipe.bytes.v1");

    // 1. Pledge self-check. /etc/init.conf grants these via SYS_SPAWN_EX
    //    pledge_subset; on misconfiguration libdriver exits with 99.
    drv_self_pledge_check(PLEDGE_NET_SERVER | PLEDGE_IPC_SEND |
                          PLEDGE_IPC_RECV | PLEDGE_SYS_QUERY |
                          PLEDGE_SYS_CONTROL);

    // 2. Claim the device. The substrate gates this on (a) PLEDGE_SYS_CONTROL,
    //    (b) class-specific pledge (NET_SERVER for class 0x02), (c) the
    //    device entry being marked claimable (e1000_expose_to_userdrv at boot).
    int rc = drv_register(E1000_VENDOR_ID, E1000_DEVICE_ID, 0x02 /*PCI_CLASS_NETWORK*/, &s_caps);
    if (rc < 0) {
        printf("[e1000d] FATAL: drv_register failed rc=%d\n", rc);
        syscall_exit(1);
    }
    printf("[e1000d] registered: bar_phys=0x%lx bar_size=0x%lx irq_vec=%u\n",
           (unsigned long)s_caps.bar_phys, (unsigned long)s_caps.bar_size,
           (unsigned)s_caps.irq_vector);

    // 3. Map the BAR. Phase 21 substrate's drv_mmio_map wraps SYS_MMIO_VMO_CREATE
    //    + SYS_VMO_MAP and forces PTE_CACHEDISABLE | PTE_WRITETHROUGH on map.
    s_mmio = (volatile uint8_t *)drv_mmio_map(s_caps.bar_phys, s_caps.bar_size,
                                              PROT_READ | PROT_WRITE);
    if (!s_mmio) {
        printf("[e1000d] FATAL: drv_mmio_map failed\n");
        syscall_exit(2);
    }
    printf("[e1000d] BAR mapped at va=%p\n", (void *)s_mmio);

    // 4. Allocate frame ring VMOs (shared with kernel proxy via HHDM,
    //    and with netd via the rawframe ANNOUNCE path — see Phase 22
    //    Stage B). We now surface the VMO handles via drv_dma_alloc_ex so
    //    we can vmo_clone(SHARED) + hand off to netd.
    //    16 slots × 4 KiB each = 64 KiB per direction.
    s_rx_ring_va = (uint8_t *)drv_dma_alloc_ex(E1000D_RING_SLOTS,
                                               &s_rx_ring_phys,
                                               &s_rx_ring_handle);
    s_tx_ring_va = (uint8_t *)drv_dma_alloc_ex(E1000D_RING_SLOTS,
                                               &s_tx_ring_phys,
                                               &s_tx_ring_handle);
    if (!s_rx_ring_va || !s_tx_ring_va) {
        printf("[e1000d] FATAL: drv_dma_alloc(rx/tx ring) failed\n");
        syscall_exit(3);
    }
    printf("[e1000d] frame rings: rx_phys=0x%lx tx_phys=0x%lx (16 × 4 KiB each)\n",
           (unsigned long)s_rx_ring_phys, (unsigned long)s_tx_ring_phys);

    // 5. Allocate descriptor rings (separate VMOs — 1 page each).
    //    Each ring holds E1000D_RING_SLOTS (16) × 16-byte descriptors = 256
    //    bytes — well under one page, so a single page per ring is plenty.
    //    Phase 21.1 trims the ring depth from the kernel driver's 256 since
    //    QEMU under normal traffic never needs more.
    s_rx_descs = (e1000_rx_desc_t *)drv_dma_alloc(1, &s_rx_descs_phys);
    s_tx_descs = (e1000_tx_desc_t *)drv_dma_alloc(1, &s_tx_descs_phys);
    if (!s_rx_descs || !s_tx_descs) {
        printf("[e1000d] FATAL: drv_dma_alloc(descriptor rings) failed\n");
        syscall_exit(4);
    }
    // Zero descriptor rings (drv_dma_alloc passes VMO_ZEROED, but be paranoid).
    memset(s_rx_descs, 0, 4096);
    memset(s_tx_descs, 0, 4096);

    // Phase 21.1: no separate hw-buffer allocations. Descriptor[i].addr
    // points directly at slot i of the shared rx/tx ring VMOs (zero-copy
    // RX, single-copy TX). 32 cap objects saved.

    // 6. Reset device + bring link up. After this the device is in a known
    //    state with all IRQs masked. Ring init (U10) will program RDBAL/RDBAH
    //    and TDBAL/TDBAH; for now we just want to read MAC + send ANNOUNCE.
    device_reset_and_link();
    read_mac_eeprom(s_mac);
    printf("[e1000d] MAC: %x:%x:%x:%x:%x:%x\n",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    // 7. Program MAC into RAL/RAH so the device accepts unicast for our MAC.
    uint32_t ral = (uint32_t)s_mac[0] | ((uint32_t)s_mac[1] << 8) |
                   ((uint32_t)s_mac[2] << 16) | ((uint32_t)s_mac[3] << 24);
    uint32_t rah = (uint32_t)s_mac[4] | ((uint32_t)s_mac[5] << 8) | (1u << 31);
    mmio_write(E1000_RAL, ral);
    mmio_write(E1000_RAH, rah);

    // 8. Initialise RX descriptor ring. Phase 21.1: each descriptor.addr
    //    points directly at slot i of the shared rx_ring VMO (zero-copy);
    //    the NIC DMAs frames into the shared pages, proxy reads via HHDM.
    //    RDBAL/RDBAH/RDLEN program the ring base + length; RDH=0; RDT = N-1
    //    means all descriptors are owned by the NIC initially.
    for (uint32_t i = 0; i < E1000D_RING_SLOTS; i++) {
        s_rx_descs[i].addr   = s_rx_ring_phys + (uint64_t)i * E1000D_SLOT_SIZE;
        s_rx_descs[i].status = 0;
    }
    mmio_write(E1000_RDBAL, (uint32_t)(s_rx_descs_phys & 0xFFFFFFFFu));
    mmio_write(E1000_RDBAH, (uint32_t)(s_rx_descs_phys >> 32));
    mmio_write(E1000_RDLEN, E1000D_RING_SLOTS * (uint32_t)sizeof(e1000_rx_desc_t));
    mmio_write(E1000_RDH, 0);
    mmio_write(E1000_RDT, E1000D_RING_SLOTS - 1);
    s_rx_tail = 0;
    mmio_write(E1000_RCTL,
               E1000_RCTL_EN | E1000_RCTL_BAM |
               E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC);

    // 9. Initialise TX descriptor ring. status=DD up front so all slots are
    //    "available". RDBAL/H/LEN/H/T parallel the RX side. TCTL.EN enables
    //    TX with collision threshold + collision distance set per Intel
    //    recommended defaults for full-duplex Gigabit.
    for (uint32_t i = 0; i < E1000D_RING_SLOTS; i++) {
        s_tx_descs[i].addr   = 0;
        s_tx_descs[i].status = E1000_DESC_DD;
    }
    mmio_write(E1000_TDBAL, (uint32_t)(s_tx_descs_phys & 0xFFFFFFFFu));
    mmio_write(E1000_TDBAH, (uint32_t)(s_tx_descs_phys >> 32));
    mmio_write(E1000_TDLEN, E1000D_RING_SLOTS * (uint32_t)sizeof(e1000_tx_desc_t));
    mmio_write(E1000_TDH, 0);
    mmio_write(E1000_TDT, 0);
    s_tx_tail = 0;
    mmio_write(E1000_TCTL,
               E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    mmio_write(E1000_TIPG, 0x0060200Au);

    // 10. Phase 22 Stage F: kernel-proxy ANNOUNCE retired (proxy deleted).
    //     The downstream substrate channel is created by userdrv but has no
    //     kernel-side reader after Stage F.  We send a no-op ANNOUNCE so any
    //     leftover test harness watching the channel sees a pulse, then move
    //     on; failure is logged but non-fatal.
    s_link_state_last = (mmio_read(E1000_STATUS) & E1000_STATUS_LU) ? 1u : 0u;
    int arc = send_announce();
    if (arc < 0) {
        printf("[e1000d] (P22.F) downstream ANNOUNCE rc=%d (proxy retired — non-fatal)\n", arc);
    } else {
        printf("[e1000d] downstream ANNOUNCE sent (link=%u)\n", (unsigned)s_link_state_last);
    }

    // 10b. Phase 22 Stage F: rawframe publish IS the live path.  netd reads
    //      frames directly via /sys/net/rawframe; failure here IS fatal —
    //      no kernel proxy fallback exists anymore.
    uint64_t frame_hash = /* grahaos.net.frame.v1 */
        ({ uint64_t h = 0xcbf29ce484222325ull;
           const char *n = "grahaos.net.frame.v1";
           for (size_t i = 0; n[i]; i++) { h ^= (uint8_t)n[i]; h *= 0x100000001b3ull; }
           h; });
    // Phase 22 closeout (G1): publish-with-retry to handle daemon
    // respawn races (rawnet_on_peer_death GC takes a brief moment).
    // Try up to 5 × 5 ms (~25 ms total) — short enough not to perturb
    // e1000dtest's 5 ms-poll-budget assertion 8.
    int prc = -1;
    for (int retry = 0; retry < 5; retry++) {
        prc = libnet_publish_service(&s_rawframe_svc, "/sys/net/rawframe",
                                     frame_hash);
        if (prc == 0) break;
        for (volatile int i = 0; i < 500000; i++) { __asm__ __volatile__("pause"); }
    }
    if (prc == 0) {
        s_rawframe_ready = 1;
        printf("[e1000d] rawframe published: /sys/net/rawframe\n");
    } else {
        printf("[e1000d] WARN: rawframe publish rc=%d after retries — exiting non-fatal\n", prc);
        syscall_exit(7);
    }

    // 11. Enable device interrupts. RXT0 = packet RX, RXSEQ = sequence error
    //     (sometimes seen during startup), LSC = link-status change, TXDW =
    //     TX descriptor write-back done. The kernel ISR forwarder posts a
    //     drv_irq_msg into our SPSC ring on every fire.
    mmio_write(E1000_IMS, E1000_IMS_RXT0 | E1000_IMS_RXSEQ |
                          E1000_IMS_LSC | E1000_IMS_TXDW);

    // 12. Main loop. Two paths interleave:
    //     (a) Block on the IRQ channel (1 s timeout). On any IRQ, read+ack
    //         ICR, walk RX descriptor ring while DD set, copy frames into
    //         shared-VMO slots + send RX_NOTIFY, recycle descriptor + bump
    //         RDT. Handle LSC link-state changes and TX completion.
    //     (b) Non-blocking chan_recv on the upstream channel for TX_NOTIFY.
    //         Each TX_NOTIFY says "slot X has a frame of len Y ready" —
    //         copy from shared TX VMO into next free hw TX buf, fill
    //         descriptor, bump TDT.
    //
    //     The loop drains as much as it can each iteration before going
    //     back to drv_irq_wait. On any -EBADF / -EPIPE we exit cleanly.
    drv_irq_msg_t msgs[16];
    chan_msg_user_t txm;
    cap_token_u_t up_rd; up_rd.raw = s_caps.upstream_handle;
    cap_token_u_t down_wr; down_wr.raw = s_caps.downstream_handle;

    while (1) {
        // ---- (a) IRQ-driven RX + LSC + TX completion ----
        long n = drv_irq_wait(s_caps.irq_channel_handle, msgs, 16, 100);
        if (n == -32 /*-ESHUTDOWN*/) {
            printf("[e1000d] IRQ channel shutdown — exiting\n");
            syscall_exit(0);
        }
        if (n < 0 && n != -110 /*-ETIMEDOUT placeholder*/) {
            printf("[e1000d] drv_irq_wait error: %ld — exiting\n", n);
            syscall_exit(6);
        }

        if (n > 0) {
            // Read+ack ICR (writing it actually clears bits in some E1000
            // variants; the 82540EM clears on read).
            uint32_t icr = mmio_read(E1000_ICR);

            // Drain RX descriptor ring — any with DD set has a fresh frame
            // already DMA'd into the shared rx_ring VMO at slot s_rx_tail.
            // No copy needed: just notify each rawframe peer. (Zero-copy RX.)
            //
            // Phase 22 Stage F: dual-fanout retired.  The kernel-side proxy
            // is gone with Mongoose, so RX_NOTIFY only flows to userspace
            // rawframe peers (netd today; future named clients).
            while (s_rx_descs[s_rx_tail].status & E1000_DESC_DD) {
                uint16_t pkt_len = s_rx_descs[s_rx_tail].length;
                if (pkt_len > E1000D_SLOT_SIZE) pkt_len = E1000D_SLOT_SIZE;
                if (pkt_len > 0) {
                    // Rawframe peer fanout.
                    for (int pi = 0; pi < E1000D_MAX_RAWFRAME_PEERS; pi++) {
                        if (!s_rawframe_peers[pi].in_use) continue;
                        chan_msg_user_t rm;
                        memset(&rm, 0, sizeof(rm));
                        rm.header.inline_len =
                            (uint16_t)sizeof(rawframe_slot_msg_t);
                        rm.header.type_hash  =
                            s_rawframe_svc.payload_type_hash;
                        rawframe_slot_msg_t *sm =
                            (rawframe_slot_msg_t *)rm.inline_payload;
                        sm->op     = RAWFRAME_OP_RX_NOTIFY;
                        sm->slot   = s_rx_tail;
                        sm->length = pkt_len;
                        long rcp = syscall_chan_send(
                            s_rawframe_peers[pi].wr_resp, &rm, 0);
                        if (rcp == -32 /*-EPIPE*/) {
                            s_rawframe_peers[pi].in_use = 0;
                        }
                    }
                }
                // Recycle: clear status, advance tail, bump RDT so NIC owns it.
                s_rx_descs[s_rx_tail].status = 0;
                uint32_t old = s_rx_tail;
                s_rx_tail = (s_rx_tail + 1) % E1000D_RING_SLOTS;
                mmio_write(E1000_RDT, old);
            }

            // Link-status change: re-read STATUS, emit LINK_UP/DOWN if it flipped.
            if (icr & 0x4u /* LSC bit in ICR */) {
                uint8_t up = (mmio_read(E1000_STATUS) & E1000_STATUS_LU) ? 1u : 0u;
                if (up != s_link_state_last) {
                    s_link_state_last = up;
                    printf("[e1000d] link state -> %s\n", up ? "UP" : "DOWN");

                    // Phase 22 Stage F: kernel-proxy LINK send retired.
                    // Fanout the link state change to rawframe peers.
                    for (int pi = 0; pi < E1000D_MAX_RAWFRAME_PEERS; pi++) {
                        if (!s_rawframe_peers[pi].in_use) continue;
                        chan_msg_user_t lm;
                        memset(&lm, 0, sizeof(lm));
                        lm.header.inline_len =
                            (uint16_t)sizeof(rawframe_slot_msg_t);
                        lm.header.type_hash  =
                            s_rawframe_svc.payload_type_hash;
                        rawframe_slot_msg_t *sm =
                            (rawframe_slot_msg_t *)lm.inline_payload;
                        sm->op = up ? RAWFRAME_OP_LINK_UP
                                    : RAWFRAME_OP_LINK_DOWN;
                        sm->slot   = 0;
                        sm->length = 0;
                        (void)syscall_chan_send(
                            s_rawframe_peers[pi].wr_resp, &lm, 0);
                    }
                }
            }
        }

        // ---- (b) Drain pending TX_NOTIFY messages from the proxy ----
        // Bound the inner loop so a flood doesn't starve the IRQ side.
        for (int tx_drain = 0; tx_drain < 32; tx_drain++) {
            long rc = syscall_chan_recv(up_rd, &txm, 0 /*non-blocking*/);
            if (rc < 0) break;  // No message ready / channel closed.
            if (txm.header.inline_len < (uint16_t)sizeof(e1000_msg_t)) continue;
            e1000_msg_t *m = (e1000_msg_t *)txm.inline_payload;
            if (m->op != E1000_OP_TX_NOTIFY) continue;
            uint32_t slot = m->slot;
            uint32_t len  = m->length;
            if (slot >= E1000D_RING_SLOTS || len == 0 || len > E1000D_SLOT_SIZE) continue;

            // Wait for the next TX descriptor to be available (DD set).
            // Brief spin — under load this should always succeed quickly.
            int spin = 0;
            while (!(s_tx_descs[s_tx_tail].status & E1000_DESC_DD) && spin++ < 1000) {
                cpu_pause();
            }
            if (!(s_tx_descs[s_tx_tail].status & E1000_DESC_DD)) {
                // TX ring exhausted — drop frame. Mongoose will retry.
                continue;
            }

            // Phase 21.1: zero-copy. Point the TX descriptor directly at the
            // shared TX ring slot the proxy filled. The NIC DMAs out of that
            // page; no intermediate buffer needed. Note: descriptor index and
            // slot index are independent (descriptors round-robin via
            // s_tx_tail; slots round-robin in proxy via its own cursor).
            s_tx_descs[s_tx_tail].addr    = s_tx_ring_phys + (uint64_t)slot * E1000D_SLOT_SIZE;
            s_tx_descs[s_tx_tail].length  = (uint16_t)len;
            s_tx_descs[s_tx_tail].cmd     = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
            s_tx_descs[s_tx_tail].status  = 0;  // NIC sets DD when done
            s_tx_descs[s_tx_tail].cso     = 0;
            s_tx_descs[s_tx_tail].css     = 0;
            s_tx_descs[s_tx_tail].special = 0;

            mfence();  // Ensure descriptor visible before TDT poke.
            s_tx_tail = (s_tx_tail + 1) % E1000D_RING_SLOTS;
            mmio_write(E1000_TDT, s_tx_tail);
        }

        // ---- (c) Phase 22 Stage B/C: rawframe accept + TX_REQ drain ----
        // (c.1) Drain any pending netd connect attempts. On each new
        //       connection we clone both ring VMOs (SHARED) and send an
        //       ANNOUNCE carrying the clones + MAC + slot geometry. Stage C
        //       also registers the peer in s_rawframe_peers[] so subsequent
        //       ticks fan RX_NOTIFY + link events out to it and accept
        //       TX_REQ on its rd_req.
        if (s_rawframe_ready) {
            for (int a = 0; a < 4; a++) {
                libnet_server_ctx_t srv;
                int arc2 = libnet_service_accept(&s_rawframe_svc, &srv, 0);
                if (arc2 <= 0) break;
                s_rawframe_accepts++;
                printf("[e1000d] rawframe accept #%d: connector_pid=%d conn_id=%u\n",
                       s_rawframe_accepts, srv.connector_pid,
                       (unsigned)srv.connection_id);

                // Clone rx/tx ring VMOs SHARED. Both sides hold live refs;
                // both can map; writes are immediately visible.
                cap_token_u_t rx_clone = {.raw = 0};
                cap_token_u_t tx_clone = {.raw = 0};
                long rc_rx = 0, rc_tx = 0;
                if (s_rx_ring_handle.raw != 0) {
                    rc_rx = syscall_vmo_clone(s_rx_ring_handle,
                                              VMO_CLONE_SHARED);
                }
                if (s_tx_ring_handle.raw != 0) {
                    rc_tx = syscall_vmo_clone(s_tx_ring_handle,
                                              VMO_CLONE_SHARED);
                }
                if (rc_rx <= 0 || rc_tx <= 0) {
                    printf("[e1000d] WARN: rawframe clone rx=%ld tx=%ld — ANNOUNCE skipped\n",
                           rc_rx, rc_tx);
                    continue;
                }
                rx_clone.raw = (uint64_t)rc_rx;
                tx_clone.raw = (uint64_t)rc_tx;

                // Build ANNOUNCE: inline body + two handles.
                chan_msg_user_t am;
                memset(&am, 0, sizeof(am));
                am.header.inline_len = (uint16_t)sizeof(rawframe_announce_t);
                am.header.nhandles   = 2;
                am.header.type_hash  = s_rawframe_svc.payload_type_hash;
                rawframe_announce_t *ab =
                    (rawframe_announce_t *)am.inline_payload;
                ab->op          = RAWFRAME_OP_ANNOUNCE;
                for (int i = 0; i < 6; i++) ab->mac[i] = s_mac[i];
                ab->link_up     = s_link_state_last;
                ab->slot_count  = E1000D_RING_SLOTS;
                ab->slot_size   = E1000D_SLOT_SIZE;
                ab->reserved    = 0;
                am.handles[0]   = rx_clone.raw;
                am.handles[1]   = tx_clone.raw;

                long rc_send = syscall_chan_send(srv.wr_resp, &am,
                                                 1000000000ull /* 1 s */);
                if (rc_send < 0) {
                    printf("[e1000d] WARN: rawframe ANNOUNCE send rc=%ld (connector_pid=%d)\n",
                           rc_send, srv.connector_pid);
                    continue;
                }
                printf("[e1000d] rawframe ANNOUNCE delivered to pid=%d "
                       "(rx_vmo=0x%lx tx_vmo=0x%lx link=%u)\n",
                       srv.connector_pid,
                       (unsigned long)rx_clone.raw,
                       (unsigned long)tx_clone.raw,
                       (unsigned)s_link_state_last);

                // Register the peer so subsequent RX ticks fan frames out.
                int slot = -1;
                for (int pi = 0; pi < E1000D_MAX_RAWFRAME_PEERS; pi++) {
                    if (!s_rawframe_peers[pi].in_use) { slot = pi; break; }
                }
                if (slot < 0) {
                    printf("[e1000d] WARN: rawframe peer table full — "
                           "connector_pid=%d dropped from fanout\n",
                           srv.connector_pid);
                    continue;
                }
                s_rawframe_peers[slot].in_use        = 1;
                s_rawframe_peers[slot].connector_pid = srv.connector_pid;
                s_rawframe_peers[slot].rd_req        = srv.rd_req;
                s_rawframe_peers[slot].wr_resp       = srv.wr_resp;
            }
        }

        // ---- (d) Phase 22 Stage C: drain TX_REQ from rawframe peers ----
        //
        // netd fills its mapped tx ring slot N with a frame and then sends
        // rawframe_slot_msg_t{op=TX_REQ, slot=N, length=L}. We read from the
        // same physical page (the SHARED clone makes the addresses alias)
        // and point the next free tx descriptor at that page, bumping TDT.
        //
        // Bounded inner cap (max 8 per peer per tick) so a chatty peer can't
        // starve the IRQ path.
        for (int pi = 0; pi < E1000D_MAX_RAWFRAME_PEERS; pi++) {
            if (!s_rawframe_peers[pi].in_use) continue;
            for (int tx = 0; tx < 8; tx++) {
                chan_msg_user_t rm;
                memset(&rm, 0, sizeof(rm));
                long rc = syscall_chan_recv(s_rawframe_peers[pi].rd_req,
                                            &rm, 0);
                if (rc == -32 /*-EPIPE*/) {
                    s_rawframe_peers[pi].in_use = 0;
                    break;
                }
                if (rc < 0) break;
                if (rm.header.inline_len < sizeof(rawframe_slot_msg_t))
                    continue;
                rawframe_slot_msg_t *sm =
                    (rawframe_slot_msg_t *)rm.inline_payload;
                if (sm->op != RAWFRAME_OP_TX_REQ) continue;
                if (sm->slot >= E1000D_RING_SLOTS ||
                    sm->length == 0 || sm->length > E1000D_SLOT_SIZE)
                    continue;

                // Spin briefly for next free descriptor (same pattern as
                // the kernel-proxy TX path above).
                int spin = 0;
                while (!(s_tx_descs[s_tx_tail].status & E1000_DESC_DD) &&
                       spin++ < 1000) {
                    cpu_pause();
                }
                if (!(s_tx_descs[s_tx_tail].status & E1000_DESC_DD)) continue;

                s_tx_descs[s_tx_tail].addr    =
                    s_tx_ring_phys + (uint64_t)sm->slot * E1000D_SLOT_SIZE;
                s_tx_descs[s_tx_tail].length  = (uint16_t)sm->length;
                s_tx_descs[s_tx_tail].cmd     = E1000_TXD_CMD_EOP |
                                                E1000_TXD_CMD_RS;
                s_tx_descs[s_tx_tail].status  = 0;
                s_tx_descs[s_tx_tail].cso     = 0;
                s_tx_descs[s_tx_tail].css     = 0;
                s_tx_descs[s_tx_tail].special = 0;
                mfence();
                s_tx_tail = (s_tx_tail + 1) % E1000D_RING_SLOTS;
                mmio_write(E1000_TDT, s_tx_tail);
            }
        }
    }
}
