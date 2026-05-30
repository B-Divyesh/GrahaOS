// user/drivers/ahcid.c — Phase 23 S3.
//
// Userspace AHCI driver daemon. Mirrors the in-kernel AHCI driver
// programming model in user space, using the Phase 21 userdrv framework
// (drv_register, drv_mmio_map, drv_irq_wait, drv_dma_alloc) for hardware
// access. Single-threaded event loop:
//   1. drv_register claims the AHCI HBA (vendor=0x8086, device=0x2922).
//   2. MMIO BAR5 is mapped uncached.
//   3. BIOS hand-off + GHC.AE re-asserted (kernel did them already at boot;
//      we re-do because the Phase 23 spec says ahcid owns the controller).
//   4. Each present port: stop CMD engine, allocate command list/FIS/command
//      tables, write phys addrs to PxCLB/PxFB, start CMD engine, IDENTIFY.
//   5. Publish /sys/blk/service via SYS_CHAN_PUBLISH.
//   6. Main loop: drv_irq_wait + non-blocking accept on /sys/blk/service +
//      non-blocking drain of per-client request channels.
//
// Phase 23 S3 is a structural delivery — the daemon exists, builds, links,
// passes ahcid_register tests when run standalone. Production cutover
// (kernel forwarder strip, channel-mediated FS I/O) follows in the
// closeout phase once we have soak data on the daemon's stability.

#include "ahcid.h"
#include "../libdriver.h"
#include "../syscalls.h"
#include "../../kernel/fs/blk_proto.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);

// Global daemon state.
ahcid_state_t g_ahcid;

// --- Small helpers -------------------------------------------------------
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void spin_ticks(uint64_t ticks) {
    uint64_t end = rdtsc_now() + ticks;
    while (rdtsc_now() < end) {
        asm volatile("pause" ::: "memory");
    }
}

static inline ahcid_port_mmio_t *port_mmio(uint8_t idx) {
    uint8_t *base = (uint8_t *)g_ahcid.hba;
    return (ahcid_port_mmio_t *)(base + 0x100 + (idx * 0x80));
}

// FNV-1a 64-bit (matches kernel manifest_type_known + libnet pattern).
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001B3ull;
    }
    return h;
}

// Send a fixed-size payload through a channel. Wraps the chan_msg_user_t
// structure required by SYS_CHAN_SEND. Returns 0 on success, negative on
// failure.
static long send_payload(cap_token_u_t wr, uint64_t type_hash,
                         const void *payload, size_t len) {
    if (len > CHAN_MSG_INLINE_MAX) return -22;
    chan_msg_user_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.type_hash  = type_hash;
    msg.header.inline_len = (uint16_t)len;
    msg.header.nhandles   = 0;
    msg.header.flags      = 0;
    memcpy(msg.inline_payload, payload, len);
    return syscall_chan_send(wr, &msg, 100ull * 1000 * 1000);  /* 100 ms */
}

// Receive a payload (non-blocking by default; uses chan_msg_user_t wrapper
// internally). Copies up to `len` bytes of inline_payload into out_buf and
// returns the inline_len, or 0 if no message, or negative on error.
static long recv_payload(cap_token_u_t rd, void *out_buf, size_t len,
                         uint64_t timeout_ns) {
    chan_msg_user_t msg;
    memset(&msg, 0, sizeof(msg));
    long rc = syscall_chan_recv(rd, &msg, timeout_ns);
    if (rc < 0) return rc;
    size_t cp = msg.header.inline_len;
    if (cp > len) cp = len;
    if (cp > 0) memcpy(out_buf, msg.inline_payload, cp);
    return (long)msg.header.inline_len;
}

// Receive a payload that may carry handles. Returns inline_len, fills
// *nh with the count of handles delivered, copies them into out_handles[].
static long recv_payload_h(cap_token_u_t rd, void *out_buf, size_t len,
                           cap_token_u_t *out_handles, uint8_t *nh,
                           uint64_t timeout_ns) {
    chan_msg_user_t msg;
    memset(&msg, 0, sizeof(msg));
    long rc = syscall_chan_recv(rd, &msg, timeout_ns);
    if (rc < 0) { *nh = 0; return rc; }
    size_t cp = msg.header.inline_len;
    if (cp > len) cp = len;
    if (cp > 0) memcpy(out_buf, msg.inline_payload, cp);
    *nh = msg.header.nhandles;
    if (out_handles && msg.header.nhandles > 0) {
        for (uint32_t i = 0; i < msg.header.nhandles && i < CHAN_MSG_HANDLES_MAX; i++) {
            out_handles[i].raw = msg.handles[i];
        }
    }
    return (long)msg.header.inline_len;
}

// ===========================================================================
// SECTION 1: STARTUP
// ===========================================================================

int ahcid_register_pci(void) {
    drv_self_pledge_check(PLEDGE_SYS_CONTROL | PLEDGE_SYS_QUERY | PLEDGE_STORAGE_SERVER);
    int rc = drv_register(AHCID_VENDOR_INTEL, AHCID_DEVICE_ICH9,
                          AHCID_PCI_CLASS_STORAGE, &g_ahcid.caps);
    if (rc != 0) {
        printf("[ahcid] drv_register rc=%d\n", rc);
        return rc;
    }
    printf("[ahcid] registered: BAR_phys=0x%llx size=%llu irq=%u\n",
           (unsigned long long)g_ahcid.caps.bar_phys,
           (unsigned long long)g_ahcid.caps.bar_size,
           (unsigned)g_ahcid.caps.irq_vector);
    return 0;
}

int ahcid_map_mmio(void) {
    void *va = drv_mmio_map(g_ahcid.caps.bar_phys, g_ahcid.caps.bar_size,
                            PROT_READ | PROT_WRITE);
    if (!va) {
        printf("[ahcid] drv_mmio_map failed\n");
        return -1;
    }
    g_ahcid.hba = (ahcid_hba_mem_t *)va;
    return 0;
}

int ahcid_take_bios_ownership(void) {
    // BOHC.OOS=1, spin until BOHC.BOS=0. Kernel did this at boot; spec
    // says re-assert when claiming the controller.
    if (!(g_ahcid.hba->bohc & AHCID_BOHC_BOS)) {
        // BIOS already released; nothing to do.
        return 0;
    }
    g_ahcid.hba->bohc |= AHCID_BOHC_OOS;
    int budget = 1000000;
    while ((g_ahcid.hba->bohc & AHCID_BOHC_BOS) && budget-- > 0) {
        asm volatile("pause" ::: "memory");
    }
    if (g_ahcid.hba->bohc & AHCID_BOHC_BOS) {
        printf("[ahcid] BIOS hand-off timeout\n");
        return -1;
    }
    printf("[ahcid] BOHC: OS-owned\n");
    return 0;
}

int ahcid_enable_ahci(void) {
    g_ahcid.hba->ghc |= AHCID_GHC_AE;
    /* Phase 23 Step 3.C: enable HBA-level interrupts.  The kernel-resident
     * ahci_init only set GHC.AE (kernel-direct used polling), so GHC.IE
     * stays 0 unless ahcid sets it explicitly.  Without IE the HBA never
     * fires hardware IRQs on command completion, and ahcid's drv_irq_wait
     * blocks forever.  This was the missing link for channel-mode I/O. */
    g_ahcid.hba->ghc |= AHCID_GHC_IE;
    g_ahcid.ncs = ((g_ahcid.hba->cap >> AHCID_CAP_NCS_SHIFT)
                   & AHCID_CAP_NCS_MASK) + 1;
    g_ahcid.np  = ((g_ahcid.hba->cap >> AHCID_CAP_NP_SHIFT)
                   & AHCID_CAP_NP_MASK) + 1;
    g_ahcid.pi  = g_ahcid.hba->pi;
    if (g_ahcid.ncs > 32) g_ahcid.ncs = 32;
    if (g_ahcid.np  > 32) g_ahcid.np  = 32;
    printf("[ahcid] AHCI enabled: NCS=%u NP=%u PI=0x%x\n",
           g_ahcid.ncs, g_ahcid.np, g_ahcid.pi);
    return 0;
}

static void port_stop_cmd(ahcid_port_mmio_t *p) {
    p->cmd &= ~(AHCID_PxCMD_ST | AHCID_PxCMD_FRE);
    int budget = 200000;
    while ((p->cmd & (AHCID_PxCMD_FR | AHCID_PxCMD_CR)) && budget-- > 0) {
        asm volatile("pause" ::: "memory");
    }
}

static void port_start_cmd(ahcid_port_mmio_t *p) {
    int budget = 200000;
    while ((p->cmd & AHCID_PxCMD_CR) && budget-- > 0) {
        asm volatile("pause" ::: "memory");
    }
    p->cmd |= AHCID_PxCMD_FRE;
    p->cmd |= AHCID_PxCMD_ST;
}

int ahcid_port_init(uint8_t idx) {
    if (idx >= AHCID_MAX_PORTS) return -1;
    ahcid_port_state_t *st = &g_ahcid.ports[idx];
    ahcid_port_mmio_t  *p  = port_mmio(idx);

    st->port_idx = idx;
    st->mmio     = p;

    uint32_t det = p->ssts & 0x0F;
    uint32_t ipm = (p->ssts >> 8) & 0x0F;
    if (det != AHCID_PORT_DEV_PRESENT || ipm != AHCID_PORT_IPM_ACTIVE) {
        return -1;
    }
    if (p->sig != AHCID_SATA_SIG_ATA) {
        return -1;
    }

    port_stop_cmd(p);

    // Allocate command list (1 KB, must be 1 KB aligned). VMO_CONTIGUOUS
    // gives us page-aligned which is sufficient.
    uint64_t phys;
    void *clist = drv_dma_alloc(1, &phys);
    if (!clist) { printf("[ahcid] port %u clist alloc failed\n", idx); return -1; }
    st->cmd_list_va  = clist;
    st->cmd_list_phys = phys;
    memset(clist, 0, 4096);

    // Allocate FIS receive buffer (256 B). Use a full page.
    void *fis = drv_dma_alloc(1, &phys);
    if (!fis) { printf("[ahcid] port %u fis alloc failed\n", idx); return -1; }
    st->fis_va   = fis;
    st->fis_phys = phys;
    memset(fis, 0, 4096);

    // Wire registers.
    p->clb = st->cmd_list_phys;
    p->fb  = st->fis_phys;

    // Allocate command tables (one per slot, 4 KB each).
    ahcid_cmd_header_t *hdr = (ahcid_cmd_header_t *)clist;
    for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
        void *tbl = drv_dma_alloc(1, &phys);
        if (!tbl) {
            printf("[ahcid] port %u cmd_table %u alloc failed\n", idx, s);
            return -1;
        }
        st->cmd_table_va[s]   = tbl;
        st->cmd_table_phys[s] = phys;
        memset(tbl, 0, 4096);
        hdr[s].ctba  = phys;
        hdr[s].prdtl = 0;
    }

    // Clear interrupt status, enable receiving.
    p->is = 0xFFFFFFFFu;
    p->ie = 0xFFFFFFFFu;

    port_start_cmd(p);

    st->present = 1;
    return 0;
}

// ATA IDENTIFY DEVICE issued through the same command machinery.
int ahcid_identify_device(uint8_t idx) {
    if (idx >= AHCID_MAX_PORTS) return -1;
    ahcid_port_state_t *st = &g_ahcid.ports[idx];
    if (!st->present) return -1;

    // Allocate a 512 B identify buffer. Use a fresh contiguous page.
    uint64_t id_phys;
    void *id_va = drv_dma_alloc(1, &id_phys);
    if (!id_va) return -1;
    memset(id_va, 0, 4096);

    // Build the command on slot 0. (Daemon hasn't issued anything yet so
    // slot 0 is free.)
    ahcid_cmd_header_t *hdr = (ahcid_cmd_header_t *)st->cmd_list_va;
    hdr[0].cfl  = sizeof(ahcid_fis_h2d_t) / 4;
    hdr[0].w    = 0;
    hdr[0].prdtl = 1;
    hdr[0].c    = 1;

    ahcid_cmd_table_t *tbl = (ahcid_cmd_table_t *)st->cmd_table_va[0];
    memset(tbl, 0, sizeof(*tbl));
    ahcid_fis_h2d_t *fis = (ahcid_fis_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->c        = 1;
    fis->command  = AHCID_ATA_IDENTIFY;
    fis->device   = 0;

    tbl->prdt_entry[0].dba  = id_phys;
    tbl->prdt_entry[0].dbc  = 512 - 1;
    tbl->prdt_entry[0].i    = 1;

    asm volatile("mfence" ::: "memory");
    st->mmio->ci = 1u << 0;

    int budget = 5000000;
    while ((st->mmio->ci & 1u) && budget-- > 0) {
        asm volatile("pause" ::: "memory");
        if (st->mmio->is & (1u << 30)) {
            printf("[ahcid] port %u IDENTIFY failed (TFD error)\n", idx);
            return -1;
        }
    }
    if (st->mmio->ci & 1u) {
        printf("[ahcid] port %u IDENTIFY timeout\n", idx);
        return -1;
    }

    // Cache the response.
    memcpy(st->identify, id_va, 512);
    // Sector count from words 100..103 (LBA48).
    uint16_t *w = (uint16_t *)st->identify;
    st->sector_count = (uint64_t)w[100]
                     | ((uint64_t)w[101] << 16)
                     | ((uint64_t)w[102] << 32)
                     | ((uint64_t)w[103] << 48);
    if (st->sector_count == 0) {
        // Fallback to LBA28 (words 60..61).
        st->sector_count = (uint32_t)w[60] | ((uint32_t)w[61] << 16);
    }
    // Sector size: word 106 bit 12 = 1 means logical sector size in 117..118.
    if (w[106] & (1u << 12)) {
        st->sector_size = (uint16_t)w[117];  // low 16 bits good enough
    } else {
        st->sector_size = 512;
    }
    printf("[ahcid] port %u identified: %llu sectors x %u bytes\n",
           idx, (unsigned long long)st->sector_count,
           (unsigned)st->sector_size);
    return 0;
}

int ahcid_enumerate_ports(void) {
    g_ahcid.port_count = 0;
    for (uint32_t i = 0; i < g_ahcid.np && i < AHCID_MAX_PORTS; i++) {
        if (!(g_ahcid.pi & (1u << i))) continue;
        if (ahcid_port_init((uint8_t)i) == 0) {
            if (ahcid_identify_device((uint8_t)i) == 0) {
                g_ahcid.port_count++;
            }
        }
    }
    printf("[ahcid] %u port(s) enumerated\n", g_ahcid.port_count);
    return (int)g_ahcid.port_count;
}

// ===========================================================================
// SECTION 2: SERVICE PUBLICATION
// ===========================================================================

// Accept channel for /sys/blk/service.
static cap_token_u_t s_accept_chan_wr = {.raw = 0};
static cap_token_u_t s_accept_chan_rd = {.raw = 0};
// Diagnostic channel for /sys/blk/list.
static cap_token_u_t s_list_chan_wr   = {.raw = 0};
static cap_token_u_t s_list_chan_rd   = {.raw = 0};

int ahcid_publish_service(void) {
    // Create the accept channel pair: kernel/clients connect → kernel
    // sends to our READ end the new sub-channel handles. SYS_CHAN_CREATE
    // signature: (type_hash, mode, capacity, *wr_out) → returns rd_handle.
    long rd_raw = syscall_chan_create(fnv1a64(BLK_SERVICE_TYPE),
                                      /*mode=*/CHAN_MODE_BLOCKING,
                                      /*capacity=*/16,
                                      &s_accept_chan_wr);
    if (rd_raw < 0) {
        printf("[ahcid] chan_create(/sys/blk/service) rc=%ld\n", rd_raw);
        return -1;
    }
    s_accept_chan_rd.raw = (uint64_t)rd_raw;

    const char *svc_name = "/sys/blk/service";
    uint32_t svc_len = 0;
    while (svc_name[svc_len]) svc_len++;
    long rcp = syscall_chan_publish(svc_name, svc_len,
                                    fnv1a64(BLK_SERVICE_TYPE),
                                    s_accept_chan_wr);
    if (rcp != 0) {
        printf("[ahcid] chan_publish /sys/blk/service rc=%ld\n", rcp);
        return -1;
    }
    printf("[ahcid] published /sys/blk/service\n");

    // /sys/blk/list — diagnostic channel for blkctl.
    long rd2 = syscall_chan_create(fnv1a64(BLK_LIST_TYPE),
                                   CHAN_MODE_BLOCKING, 4,
                                   &s_list_chan_wr);
    if (rd2 >= 0) {
        s_list_chan_rd.raw = (uint64_t)rd2;
        const char *list_name = "/sys/blk/list";
        uint32_t list_len = 0;
        while (list_name[list_len]) list_len++;
        long rcp2 = syscall_chan_publish(list_name, list_len,
                                         fnv1a64(BLK_LIST_TYPE),
                                         s_list_chan_wr);
        if (rcp2 == 0) {
            printf("[ahcid] published /sys/blk/list\n");
        }
    }
    return 0;
}

// ===========================================================================
// SECTION 3: OP DISPATCH
// ===========================================================================

// Find a free command slot for the given port. Returns -1 if all busy.
static int find_free_slot(ahcid_port_state_t *st) {
    uint32_t ci = st->mmio->ci;
    for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
        if (!(ci & (1u << s)) && !st->slot[s].in_use) return (int)s;
    }
    return -1;
}

static int issue_io(ahcid_client_t *cli, const blk_req_msg_t *req,
                    uint8_t ata_cmd, uint8_t write_dir) {
    if (req->dev >= AHCID_MAX_PORTS) return BLK_E_NODEV;
    ahcid_port_state_t *st = &g_ahcid.ports[req->dev];
    if (!st->present) return BLK_E_NODEV;
    if (req->lba + req->count > st->sector_count) return BLK_E_INVAL;
    if ((req->vmo_offset & 0x1FFu) != 0) return BLK_E_INVAL;
    if (req->count == 0 || req->count > 128) return BLK_E_INVAL;

    int slot = find_free_slot(st);
    if (slot < 0) return BLK_E_IO;  // pool exhausted; should not happen at NCS=32

    // Resolve client's DMA VMO physical address for the requested offset.
    uint32_t page_idx = req->vmo_offset / 4096;
    uint32_t intra_off = req->vmo_offset & 0xFFFu;
    uint64_t base_phys = 0;
    long rc = syscall_vmo_phys(cli->dma_vmo_handle, page_idx, &base_phys);
    if (rc < 0 || base_phys == 0) return BLK_E_ACCES;

    ahcid_cmd_header_t *hdr = (ahcid_cmd_header_t *)st->cmd_list_va;
    hdr += slot;
    hdr->cfl   = sizeof(ahcid_fis_h2d_t) / 4;
    hdr->w     = write_dir;
    hdr->prdtl = 1;
    hdr->c     = 1;
    hdr->prdbc = 0;

    ahcid_cmd_table_t *tbl = (ahcid_cmd_table_t *)st->cmd_table_va[slot];
    memset(tbl, 0, sizeof(*tbl));
    ahcid_fis_h2d_t *fis = (ahcid_fis_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->c        = 1;
    fis->command  = ata_cmd;
    fis->lba0     = (uint8_t)(req->lba);
    fis->lba1     = (uint8_t)(req->lba >> 8);
    fis->lba2     = (uint8_t)(req->lba >> 16);
    fis->device   = 1u << 6;  /* LBA mode */
    fis->lba3     = (uint8_t)(req->lba >> 24);
    fis->lba4     = (uint8_t)(req->lba >> 32);
    fis->lba5     = (uint8_t)(req->lba >> 40);
    fis->countl   = (uint8_t)(req->count);
    fis->counth   = (uint8_t)(req->count >> 8);

    tbl->prdt_entry[0].dba = base_phys + intra_off;
    tbl->prdt_entry[0].dbc = (uint32_t)req->count * 512u - 1u;
    tbl->prdt_entry[0].i   = 1;

    // Stash waiter info so the IRQ path can match completion → response.
    st->slot[slot].in_use   = 1;
    st->slot[slot].req_id   = req->req_id;
    st->slot[slot].resp_chan = cli->client_chan_write;
    st->slot[slot].start_tsc = rdtsc_now();
    st->slot[slot].bytes    = (uint32_t)req->count * 512u;
    /* W5 Phase 2: stash ring info iff client connected with SPSC ring.
     * ring_slot_idx is derivable from vmo_offset (kernel writes to slot N's
     * 4 KiB DMA page → vmo_offset = N*4096; matches blk_spsc_post_req). */
    st->slot[slot].spsc_ring     = cli->spsc_ring;
    st->slot[slot].ring_slot_idx = req->vmo_offset / 4096u;
    st->slot[slot].cli_idx       = (uint8_t)(cli - g_ahcid.clients);

    asm volatile("mfence" ::: "memory");
    st->mmio->ci = 1u << slot;
    return 0;
}

int ahcid_do_read(ahcid_client_t *cli, const blk_req_msg_t *req) {
    return issue_io(cli, req, AHCID_ATA_READ_DMA_EXT, 0);
}

int ahcid_do_write(ahcid_client_t *cli, const blk_req_msg_t *req) {
    return issue_io(cli, req, AHCID_ATA_WRITE_DMA_EXT, 1);
}

int ahcid_do_flush(ahcid_client_t *cli, const blk_req_msg_t *req) {
    if (req->dev >= AHCID_MAX_PORTS) return BLK_E_NODEV;
    ahcid_port_state_t *st = &g_ahcid.ports[req->dev];
    if (!st->present) return BLK_E_NODEV;

    int slot = find_free_slot(st);
    if (slot < 0) return BLK_E_IO;

    ahcid_cmd_header_t *hdr = (ahcid_cmd_header_t *)st->cmd_list_va;
    hdr += slot;
    hdr->cfl   = sizeof(ahcid_fis_h2d_t) / 4;
    hdr->w     = 0;
    hdr->prdtl = 0;
    hdr->c     = 1;
    hdr->prdbc = 0;

    ahcid_cmd_table_t *tbl = (ahcid_cmd_table_t *)st->cmd_table_va[slot];
    memset(tbl, 0, sizeof(*tbl));
    ahcid_fis_h2d_t *fis = (ahcid_fis_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->c        = 1;
    fis->command  = AHCID_ATA_FLUSH_CACHE_EXT;
    fis->device   = 1u << 6;

    st->slot[slot].in_use    = 1;
    st->slot[slot].req_id    = req->req_id;
    st->slot[slot].resp_chan = cli->client_chan_write;
    st->slot[slot].start_tsc = rdtsc_now();
    st->slot[slot].bytes     = 0;
    /* W5 Phase 2: ring info for IRQ-path response coalescing. FLUSH has
     * no DMA → we can't derive ring_slot_idx from vmo_offset. The kernel
     * still allocates a waiter slot for FLUSH and uses req_id as the demux
     * key, so we compute ring_slot_idx by scanning the ring on completion
     * (kernel's waiter slot index == ring slot index by design). Stash 0xFF
     * as a sentinel and let the IRQ path scan-by-req_id. */
    st->slot[slot].spsc_ring     = cli->spsc_ring;
    st->slot[slot].ring_slot_idx = 0xFFFFFFFFu;
    st->slot[slot].cli_idx       = (uint8_t)(cli - g_ahcid.clients);

    asm volatile("mfence" ::: "memory");
    st->mmio->ci = 1u << slot;
    return 0;
}

int ahcid_do_identify(ahcid_client_t *cli, const blk_req_msg_t *req) {
    if (req->dev >= AHCID_MAX_PORTS) return BLK_E_NODEV;
    ahcid_port_state_t *st = &g_ahcid.ports[req->dev];
    if (!st->present) return BLK_E_NODEV;

    // IDENTIFY is cached at startup. Send the response immediately by
    // building a synthetic resp + writing the cached payload into the
    // client's DMA VMO at the requested offset. We don't issue a hardware
    // command. The IRQ path won't fire so do the response inline.
    if (req->vmo_handle != 0) {
        // The blk_proto contract says vmo_offset is unused for IDENTIFY
        // in the request; we put the IDENTIFY data in the response only
        // through the inline channel, but kernel blk_client passes the
        // shared VMO so we can also write it there. For now, return ok
        // status; the cached data is accessed via /sys/blk/list only.
    }

    /* W5 Phase 2: if client has SPSC ring, write resp into ring slot +
     * send 1-byte doorbell. Otherwise legacy 24-byte chan_send. */
    if (cli->spsc_ring) {
        blk_spsc_slot_t *ring = (blk_spsc_slot_t *)cli->spsc_ring;
        uint32_t ring_idx = req->vmo_offset / 4096u;
        if (ring_idx < BLK_SPSC_RING_SLOTS) {
            blk_spsc_slot_t *s = &ring[ring_idx];
            s->status = BLK_E_OK;
            s->bytes  = 512;
            asm volatile("mfence" ::: "memory");
            __atomic_store_n(&s->done, 1u, __ATOMIC_RELEASE);
            uint8_t db = 0;
            cap_token_u_t tok = {.raw = (uint64_t)cli->client_chan_write};
            (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &db, 1);
            return 0;
        }
        /* fall through to legacy on bad ring_idx */
    }
    blk_resp_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.req_id            = req->req_id;
    resp.status            = BLK_E_OK;
    resp.bytes_transferred = 512;
    resp.timestamp_tsc     = rdtsc_now();
    cap_token_u_t tok = {.raw = (uint64_t)cli->client_chan_write};
    (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &resp, sizeof(resp));
    return 0;
}

// ===========================================================================
// SECTION 4: ERROR RECOVERY
// ===========================================================================

/* Forward decl for W5 Phase 2 ring lookup used by both ahcid_port_reset
 * (error path) and handle_irq (completion path). The body is alongside
 * handle_irq below. */
static uint32_t ring_slot_for_req_id(const blk_spsc_slot_t *ring,
                                     uint32_t req_id);

int ahcid_port_reset(uint8_t idx) {
    if (idx >= AHCID_MAX_PORTS) return -1;
    ahcid_port_state_t *st = &g_ahcid.ports[idx];
    if (!st->present) return -1;
    ahcid_port_mmio_t *p = st->mmio;

    port_stop_cmd(p);
    p->sctl |= 0x1u;  /* DET = 1 */
    spin_ticks(2000ull * 1000 * 1000);  /* 1 ms @ ~2 GHz approx */
    p->sctl &= ~0x1u;

    int budget = AHCID_PORT_RESET_POLL_BUDGET;
    while (((p->ssts & 0x0F) != AHCID_PORT_DEV_PRESENT) && budget-- > 0) {
        spin_ticks(10ull * 1000 * 1000);  /* 5 ms approx */
    }
    if ((p->ssts & 0x0F) != AHCID_PORT_DEV_PRESENT) {
        printf("[ahcid] port %u reset timeout — exit for supervisor respawn\n", idx);
        syscall_exit(1);
    }
    p->serr = 0xFFFFFFFFu;
    p->is   = 0xFFFFFFFFu;
    port_start_cmd(p);

    // Drop all in-flight slots with -EIO.
    uint8_t doorbell_pending[AHCID_MAX_CLIENTS];
    memset(doorbell_pending, 0, sizeof(doorbell_pending));
    for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
        if (!st->slot[s].in_use) continue;
        blk_spsc_slot_t *ring = (blk_spsc_slot_t *)st->slot[s].spsc_ring;
        uint32_t ring_idx = st->slot[s].ring_slot_idx;
        if (ring && ring_idx == 0xFFFFFFFFu) {
            ring_idx = ring_slot_for_req_id(ring, st->slot[s].req_id);
        }
        if (ring && ring_idx < BLK_SPSC_RING_SLOTS) {
            blk_spsc_slot_t *rs = &ring[ring_idx];
            rs->status = BLK_E_IO;
            rs->bytes  = 0;
            asm volatile("mfence" ::: "memory");
            __atomic_store_n(&rs->done, 1u, __ATOMIC_RELEASE);
            if (st->slot[s].cli_idx < AHCID_MAX_CLIENTS) {
                doorbell_pending[st->slot[s].cli_idx] = 1u;
            }
        } else {
            blk_resp_msg_t resp = {0};
            resp.req_id        = st->slot[s].req_id;
            resp.status        = BLK_E_IO;
            resp.timestamp_tsc = rdtsc_now();
            cap_token_u_t tok = {.raw = (uint64_t)st->slot[s].resp_chan};
            (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &resp, sizeof(resp));
        }
        st->slot[s].in_use    = 0;
        st->slot[s].spsc_ring = NULL;
    }
    /* Coalesced doorbells (one per client). */
    for (uint32_t c = 0; c < AHCID_MAX_CLIENTS; c++) {
        if (!doorbell_pending[c]) continue;
        if (!g_ahcid.clients[c].in_use) continue;
        uint8_t db = 0;
        cap_token_u_t tok = {.raw = (uint64_t)g_ahcid.clients[c].client_chan_write};
        (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &db, 1);
    }
    return 0;
}

// ===========================================================================
// SECTION 5: MAIN LOOP
// ===========================================================================

// Allocate a free client slot. Returns NULL on exhaustion.
static ahcid_client_t *new_client_slot(void) {
    for (uint32_t i = 0; i < AHCID_MAX_CLIENTS; i++) {
        if (!g_ahcid.clients[i].in_use) {
            memset(&g_ahcid.clients[i], 0, sizeof(g_ahcid.clients[i]));
            g_ahcid.clients[i].in_use = 1;
            return &g_ahcid.clients[i];
        }
    }
    return NULL;
}

// W5 Phase 2: resolve a ring-slot index for an HBA slot whose request had
// no vmo_offset (FLUSH). We scan the ring for the matching req_id. Returns
// 0xFFFFFFFF if not found (caller falls back to legacy chan_send).
static uint32_t ring_slot_for_req_id(const blk_spsc_slot_t *ring,
                                     uint32_t req_id) {
    for (uint32_t i = 0; i < BLK_SPSC_RING_SLOTS; i++) {
        if (ring[i].req_id == req_id) return i;
    }
    return 0xFFFFFFFFu;
}

/* Harvest every completed (PxCI-bit-clear) in_use slot on port `st`.
 * For each: publish the completion (SPSC ring done=1 + flag a coalesced
 * doorbell, or the legacy 24-byte resp chan_send) and free the slot
 * (in_use=0). A clear PxCI bit is the HBA-authoritative "command complete"
 * signal, so this is correct whether the harvest is triggered by an IRQ
 * (handle_irq) OR by the main-loop poll (poll_complete_slots) — in-flight
 * commands keep their CI bit set and are skipped. doorbell_pending[cli]
 * is OR'd so the caller can emit one coalesced doorbell per client. Does
 * NOT touch PxIS (that is the IRQ-acknowledge path's responsibility). */
static void harvest_port_completed(ahcid_port_state_t *st,
                                   uint8_t *doorbell_pending) {
    ahcid_port_mmio_t *p = st->mmio;
    uint32_t ci = p->ci;
    for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
        if (!st->slot[s].in_use) continue;
        if (ci & (1u << s)) continue;  /* still in-flight */

        /* W5 Phase 2: prefer ring path if client connected with one.
         * Avoids the 24-byte blk_resp_msg_t chan_send on the hot path;
         * doorbell is coalesced by the caller. */
        blk_spsc_slot_t *ring = (blk_spsc_slot_t *)st->slot[s].spsc_ring;
        uint32_t ring_idx = st->slot[s].ring_slot_idx;
        if (ring && ring_idx == 0xFFFFFFFFu) {
            ring_idx = ring_slot_for_req_id(ring, st->slot[s].req_id);
        }
        if (ring && ring_idx < BLK_SPSC_RING_SLOTS) {
            blk_spsc_slot_t *rs = &ring[ring_idx];
            rs->status = BLK_E_OK;
            rs->bytes  = st->slot[s].bytes;
            asm volatile("mfence" ::: "memory");
            __atomic_store_n(&rs->done, 1u, __ATOMIC_RELEASE);
            if (st->slot[s].cli_idx < AHCID_MAX_CLIENTS) {
                doorbell_pending[st->slot[s].cli_idx] = 1u;
            }
        } else {
            /* Legacy: full 24-byte resp via chan_send. */
            blk_resp_msg_t resp = {0};
            resp.req_id            = st->slot[s].req_id;
            resp.status            = BLK_E_OK;
            resp.bytes_transferred = st->slot[s].bytes;
            resp.timestamp_tsc     = rdtsc_now();
            cap_token_u_t tok = {.raw = (uint64_t)st->slot[s].resp_chan};
            (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE),
                               &resp, sizeof(resp));
        }

        st->slot[s].in_use    = 0;
        st->slot[s].spsc_ring = NULL;  /* defensive */
        g_ahcid.requests_total++;
    }
}

/* Emit one coalesced 1-byte doorbell per client that had ≥1 completion.
 * The kt task scans the whole ring on every doorbell, so coalescing (and
 * even a redundant doorbell across handle_irq + poll_complete_slots) is
 * safe — kt picks up all done=1 slots. */
static void emit_coalesced_doorbells(const uint8_t *doorbell_pending) {
    for (uint32_t c = 0; c < AHCID_MAX_CLIENTS; c++) {
        if (!doorbell_pending[c]) continue;
        if (!g_ahcid.clients[c].in_use) continue;
        uint8_t db = 0;
        cap_token_u_t tok = {.raw = (uint64_t)g_ahcid.clients[c].client_chan_write};
        (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &db, 1);
    }
}

static void handle_irq(void) {
    g_ahcid.irq_count++;
    uint32_t hba_is = g_ahcid.hba->is;
    if (hba_is == 0) return;

    /* W5 Phase 2 coalescing: track which clients had ≥1 SPSC completion in
     * this IRQ. After all done flags are written we send ONE 1-byte
     * chan_send doorbell per client (vs. one per completion). For batched
     * 6-deep ops this collapses 6 chan_sends → 1. */
    uint8_t doorbell_pending[AHCID_MAX_CLIENTS];
    memset(doorbell_pending, 0, sizeof(doorbell_pending));

    for (uint32_t i = 0; i < AHCID_MAX_PORTS; i++) {
        if (!(hba_is & (1u << i))) continue;
        ahcid_port_state_t *st = &g_ahcid.ports[i];
        if (!st->present) continue;
        ahcid_port_mmio_t *p = st->mmio;
        uint32_t port_is = p->is;
        // Check for task-file error (any error fires the recovery path).
        if (port_is & (1u << 30)) {
            ahcid_port_reset((uint8_t)i);
            p->is = port_is;
            continue;
        }
        // Walk slots; for each slot with in_use=1 and CI bit cleared,
        // command completed → send response.
        harvest_port_completed(st, doorbell_pending);
        p->is = port_is;  /* clear */
    }
    g_ahcid.hba->is = hba_is;  /* clear */

    emit_coalesced_doorbells(doorbell_pending);
}

/* FU24.A/B (#660) fix: poll-harvest completions INDEPENDENT of IRQ delivery.
 *
 * handle_irq only runs when drv_irq_wait returns an IRQ message, and it
 * snapshots PxCI then unconditionally clears PxIS — so a completion that
 * lands in the snapshot→clear window, or an AHCI IRQ that is lost/coalesced
 * away under load, leaves an in_use slot whose command actually FINISHED.
 * The kernel waiter for that slot then sits until its 5 s blk_wait_response
 * deadline; under the spawn/txn-cluster burst this compounds into many
 * sequential 5 s per-op grinds that push the gate past its watchdog
 * (root cause of #660 — confirmed: spawn_argv's child-spawns were spaced
 * exactly 5 s/15 s apart = blk timeouts firing). Scanning PxCI every
 * main-loop iteration reaps any completed-but-unsignaled slot promptly, so
 * completion delivery never depends on IRQ delivery. Idempotent with
 * handle_irq (a slot it already reaped has in_use=0 and is skipped). */
static void poll_complete_slots(void) {
    uint8_t doorbell_pending[AHCID_MAX_CLIENTS];
    memset(doorbell_pending, 0, sizeof(doorbell_pending));
    for (uint32_t i = 0; i < AHCID_MAX_PORTS; i++) {
        ahcid_port_state_t *st = &g_ahcid.ports[i];
        if (!st->present) continue;
        harvest_port_completed(st, doorbell_pending);
    }
    emit_coalesced_doorbells(doorbell_pending);
}

// Phase 24a W3: dispatch a single blk_req_msg_t op. Returns 0 if the
// command was queued (response will fire from IRQ path), or BLK_E_*
// negative errno if the dispatch itself failed (response sent inline by
// caller). Extracted from handle_client_request so the batch path can
// reuse it.
static int dispatch_one_request(ahcid_client_t *cli,
                                const blk_req_msg_t *req) {
    switch (req->op) {
        case BLK_OP_READ:     return ahcid_do_read(cli, req);
        case BLK_OP_WRITE:    return ahcid_do_write(cli, req);
        case BLK_OP_FLUSH:    return ahcid_do_flush(cli, req);
        case BLK_OP_IDENTIFY: ahcid_do_identify(cli, req); return -1; /* sentinel: response sent inline */
        default:              return BLK_E_INVAL;
    }
}

static void handle_client_request(ahcid_client_t *cli) {
    /* Phase 24a W3: receive up to CHAN_MSG_INLINE_MAX bytes — large enough
     * for both a 32-byte single blk_req_msg_t and a 200-byte
     * blk_batch_req_t. Dispatch on the first byte (kind tag) when the
     * payload is large enough to be a batch. */
    uint8_t buf[256];  /* CHAN_MSG_INLINE_MAX */
    cap_token_u_t tok = {.raw = (uint64_t)cli->client_chan_read};
    long n = recv_payload(tok, buf, sizeof(buf), 0);  /* non-block */
    if (n < (long)sizeof(blk_req_msg_t)) return;

    if ((size_t)n >= sizeof(blk_batch_req_t) && buf[0] == BLK_KIND_BATCH_REQ) {
        const blk_batch_req_t *batch = (const blk_batch_req_t *)buf;
        uint8_t count = batch->count;
        if (count == 0u || count > BLK_BATCH_MAX) return;

        /* Issue each command. ahcid_do_read/write etc. queue the command
         * into an HBA slot and write PxCI bit immediately; AHCI HBA
         * processes them in parallel (NCS=32 supports up to 32 in flight).
         * On per-op dispatch failure we synthesise an immediate response
         * (in the SAME wire format as a single-op response) so the
         * kernel side can demux by req_id without distinguishing batch
         * vs single on the response channel. The SUCCESSFUL completions
         * fire later via the IRQ path. */
        cap_token_u_t wtok = {.raw = (uint64_t)cli->client_chan_write};
        for (uint8_t i = 0; i < count; i++) {
            const blk_req_msg_t *r = &batch->reqs[i];
            int op_rc = dispatch_one_request(cli, r);
            if (op_rc != 0 && op_rc != -1) {
                blk_resp_msg_t resp = {0};
                resp.req_id        = r->req_id;
                resp.status        = (int32_t)op_rc;
                resp.timestamp_tsc = rdtsc_now();
                (void)send_payload(wtok, fnv1a64(BLK_SERVICE_TYPE),
                                   &resp, sizeof(resp));
            }
            cli->reqs_handled++;
        }
        return;
    }

    /* Single-op (legacy) path. */
    const blk_req_msg_t *req = (const blk_req_msg_t *)buf;
    int op_rc = dispatch_one_request(cli, req);
    if (op_rc == -1) return;  /* IDENTIFY sent inline */

    if (op_rc != 0) {
        blk_resp_msg_t resp = {0};
        resp.req_id        = req->req_id;
        resp.status        = (int32_t)op_rc;
        resp.timestamp_tsc = rdtsc_now();
        cap_token_u_t wtok = {.raw = (uint64_t)cli->client_chan_write};
        (void)send_payload(wtok, fnv1a64(BLK_SERVICE_TYPE),
                           &resp, sizeof(resp));
    }
    cli->reqs_handled++;
}

// Phase 24a W5: drain SPSC ring slots. Polls all 64 slots; for each
// ready=1 slot, builds a synthetic blk_req_msg_t and dispatches via the
// existing handle_client_request machinery. The slot's `done` flag is
// reserved for Phase 2 (response also via ring); in Phase 1 the response
// goes via the legacy chan_send path (handle_client_request's send_payload
// at the bottom of dispatch). After dispatching we set ready=0 to mark
// the slot reusable; producer (kernel) clears done=0 on next post.
static void drain_spsc_ring(ahcid_client_t *cli) {
    if (!cli->spsc_ring) return;
    blk_spsc_slot_t *ring = (blk_spsc_slot_t *)cli->spsc_ring;
    for (uint32_t i = 0; i < BLK_SPSC_RING_SLOTS; i++) {
        blk_spsc_slot_t *s = &ring[i];
        /* Acquire load: synchronizes with kernel's release store that
         * publishes ready=1.  All req fields are visible iff we observe
         * ready==1. */
        if (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) == 0u) continue;

        /* Build a blk_req_msg_t from the slot's fields and dispatch.
         * vmo_handle = 0 because ahcid uses cli->dma_vmo_handle to access
         * DMA pages (slot index → page index). vmo_offset = slot * 4 KB
         * (the per-slot DMA region). */
        blk_req_msg_t req = {0};
        req.req_id     = s->req_id;
        req.op         = s->op;
        req.dev        = s->dev;
        req._pad       = 0;
        req.lba        = s->lba;
        req.count      = s->count;
        req.vmo_handle = 0;
        req.vmo_offset = i * 4096u;  /* slot N → DMA VMO page N */
        req.timeout_ms = s->timeout_ms;

        /* Slot consumed — flip ready=0 so the producer knows it can
         * reuse the slot once it has read `done`. */
        __atomic_store_n(&s->ready, 0u, __ATOMIC_RELEASE);

        /* Dispatch via existing machinery. ahcid_do_read/write/flush
         * queue the AHCI command; on completion the IRQ path writes
         * status+bytes to the ring slot + done=1 + coalesced doorbell. */
        int op_rc = dispatch_one_request(cli, &req);
        if (op_rc == -1) {
            /* IDENTIFY: response written inline by ahcid_do_identify. */
            cli->reqs_handled++;
            continue;
        }
        if (op_rc != 0) {
            /* Dispatch-time error — synthesise via ring (same slot we
             * just consumed). The slot has ready=0; setting status/bytes
             * + done=1 lets the kernel kt task pick it up on the next
             * doorbell scan. */
            s->status = (int32_t)op_rc;
            s->bytes  = 0;
            asm volatile("mfence" ::: "memory");
            __atomic_store_n(&s->done, 1u, __ATOMIC_RELEASE);
            uint8_t db = 0;
            cap_token_u_t wtok = {.raw = (uint64_t)cli->client_chan_write};
            (void)send_payload(wtok, fnv1a64(BLK_SERVICE_TYPE), &db, 1);
        }
        cli->reqs_handled++;
    }
}

static void try_accept_new_client(void) {
    // Phase 22 chan-publish/connect mints a new pair on accept; the new
    // pair's read+write endpoints arrive as handles on s_accept_chan_rd.
    uint8_t nh = 0;
    cap_token_u_t handles[CHAN_MSG_HANDLES_MAX];
    uint8_t inline_buf[CHAN_MSG_INLINE_MAX];
    long rc = recv_payload_h(s_accept_chan_rd, inline_buf, sizeof(inline_buf),
                             handles, &nh, 0);
    if (rc < 0) return;
    if (nh < 2) return;

    ahcid_client_t *cli = new_client_slot();
    if (!cli) {
        printf("[ahcid] client table full\n");
        return;
    }
    cli->client_chan_read  = handles[0].raw;
    cli->client_chan_write = handles[1].raw;

    // Receive the blk_connect_msg_t handshake (1 s timeout). Phase 23
    // closeout fix: the DMA VMO is delivered as a HANDLE (full 64-bit
    // cap_token) in chan_msg_user_t.handles[0]; the inline `dma_vmo`
    // uint32_t field is now informational/debug only because truncating
    // to 32 bits loses the cap_token generation and the kernel rejects
    // any subsequent syscall_vmo_phys with -EBADF. Using recv_payload_h
    // we get the receiver-generated 64-bit token correctly.
    //
    // Phase 24a W5: read up to sizeof(blk_connect_msg_v2_t) so the SPSC
    // fields (spsc_vmo, spsc_size) are available. v1 senders use the
    // smaller blk_connect_msg_t; we accept both by checking version.
    blk_connect_msg_v2_t cm;
    memset(&cm, 0, sizeof(cm));
    uint8_t cm_nh = 0;
    cap_token_u_t cm_handles[CHAN_MSG_HANDLES_MAX];
    cap_token_u_t rtok = {.raw = cli->client_chan_read};
    long crc = recv_payload_h(rtok, &cm, sizeof(cm), cm_handles, &cm_nh,
                              1ull * 1000 * 1000 * 1000);
    /* v1 connect msg is 24 bytes; v2 is 32 bytes. Accept either. */
    if (crc < (long)sizeof(blk_connect_msg_t) || cm.magic != BLK_PROTO_MAGIC ||
        (cm.version != BLK_PROTO_VERSION && cm.version != BLK_PROTO_VERSION_V2)) {
        printf("[ahcid] bad connect handshake from client crc=%ld magic=0x%x ver=%u nh=%u\n",
               crc, (unsigned)cm.magic, (unsigned)cm.version, (unsigned)cm_nh);
        cli->in_use = 0;
        return;
    }
    if (cm_nh < 1 || cm_handles[0].raw == 0) {
        printf("[ahcid] connect handshake missing DMA VMO handle\n");
        cli->in_use = 0;
        return;
    }
    cli->dma_vmo_handle = cm_handles[0].raw;

    /* Phase 24a W5: optional SPSC ring VMO in handles[1].  Map it into
     * our address space via syscall_vmo_map if available.  If mapping
     * fails, fall back to legacy chan_recv path (cli->spsc_ring stays
     * NULL). */
    cli->spsc_vmo_handle = 0;
    cli->spsc_ring       = NULL;
    if (cm.version == BLK_PROTO_VERSION_V2 && cm.spsc_vmo != 0u &&
        cm_nh >= 2 && cm_handles[1].raw != 0) {
        cli->spsc_vmo_handle = cm_handles[1].raw;
        cap_token_u_t spsc_tok = {.raw = cli->spsc_vmo_handle};
        long mrc = syscall_vmo_map(spsc_tok, /*addr_hint=*/0, /*offset=*/0,
                                   /*len=*/BLK_SPSC_RING_BYTES,
                                   /*prot=*/PROT_READ | PROT_WRITE);
        if (mrc < 0) {
            printf("[ahcid] spsc vmo_map rc=%ld — falling back to chan_send\n",
                   mrc);
            cli->spsc_vmo_handle = 0;
            cli->spsc_ring       = NULL;
        } else {
            cli->spsc_ring = (void *)mrc;
            printf("[ahcid] client connected dma_vmo=0x%llx spsc_vmo=0x%llx ring=%p\n",
                   (unsigned long long)cli->dma_vmo_handle,
                   (unsigned long long)cli->spsc_vmo_handle,
                   cli->spsc_ring);
            return;
        }
    }
    printf("[ahcid] client connected dma_vmo=0x%llx (legacy chan path)\n",
           (unsigned long long)cli->dma_vmo_handle);
}

void ahcid_main_loop(void) {
    drv_irq_msg_t irq_msgs[8];
    uint64_t cap_irq_handle = g_ahcid.caps.irq_channel_handle;
    while (1) {
        /* Phase 23 Step 3.C latency optimisation: drain pending client
         * requests FIRST so a kernel blk_req that arrived during the last
         * iteration is seen immediately (rather than after the 5 ms IRQ
         * wait).  Then handle any completed IRQs.  Then drv_irq_wait
         * with a SHORT 1 ms cap (was 5) — keeps idle CPU low while
         * minimising the IRQ-to-response window.
         *
         * Phase 24a W5: when any client has the SPSC ring mapped, the
         * 1 ms drv_irq_wait timeout is the dominant per-op latency cost
         * (kernel writes to ring → ahcid only sees it on next loop iter
         * up to 1 ms later). Switch to non-blocking drv_irq_wait + a
         * userspace sched_yield to give other tasks a chance without
         * adding wake-up latency. Burns CPU on ahcid's pinning but in
         * the gate (4 CPUs, ahcid mostly alone on its CPU) that's
         * acceptable. The yield itself is a syscall with ~us overhead;
         * total per-iter cost is ~us instead of ~ms. */
        int spsc_active = 0;

        // 1. Try to accept a new client on /sys/blk/service.
        try_accept_new_client();

        // 2. Drain per-client request paths FIRST (latency-critical).
        // Phase 24a W5: poll the shared SPSC ring (if mapped) before the
        // legacy chan_recv path. Ring polling is lock-free and ~ns per
        // slot; the legacy path stays as a compat fallback for v1
        // connect frames.
        for (uint32_t i = 0; i < AHCID_MAX_CLIENTS; i++) {
            if (!g_ahcid.clients[i].in_use) continue;
            if (g_ahcid.clients[i].spsc_ring) {
                drain_spsc_ring(&g_ahcid.clients[i]);
                spsc_active = 1;
            }
            handle_client_request(&g_ahcid.clients[i]);
        }

        // 3. Wait for IRQs. With SPSC ring active, use timeout=0
        //    (non-blocking) so we can re-check the ring next iteration
        //    without sleeping; without SPSC, retain the 1 ms cap.
        uint32_t to = spsc_active ? 0u : 1u;
        long n = drv_irq_wait(cap_irq_handle, irq_msgs, 8, to);
        if (n > 0) {
            for (long i = 0; i < n; i++) handle_irq();
        }

        // 3b. FU24.A/B (#660): unconditionally poll-harvest completed HBA
        //     slots every iteration, independent of whether an IRQ arrived.
        //     Closes the lost/missed-IRQ window in handle_irq that otherwise
        //     strands a finished command until the kernel's 5 s timeout,
        //     compounding into the per-op grind that tips the gate past its
        //     watchdog. Cheap: one PxCI MMIO read per present port + a scan
        //     that early-outs on !in_use slots.
        poll_complete_slots();

        // 4. With SPSC active and no IRQs, emit `pause` instructions so
        //    we don't burn 100% CPU for nothing while still resuming the
        //    ring poll within ~ns. SMT-friendly hint to the CPU; in TCG
        //    it's basically a NOP. No syscall = no context switch =
        //    request seen on next iteration with no added wake latency.
        if (spsc_active && n == 0) {
            for (int p = 0; p < 16; p++) asm volatile("pause" ::: "memory");
        }
    }
}

// ===========================================================================
// ENTRY
// ===========================================================================
void _start(void) {
    memset(&g_ahcid, 0, sizeof(g_ahcid));

    if (ahcid_register_pci() != 0) syscall_exit(2);
    if (ahcid_map_mmio()      != 0) syscall_exit(2);
    if (ahcid_take_bios_ownership() != 0) syscall_exit(2);
    if (ahcid_enable_ahci()   != 0) syscall_exit(2);
    if (ahcid_enumerate_ports() <= 0) {
        printf("[ahcid] no drives detected; exiting\n");
        syscall_exit(2);
    }
    if (ahcid_publish_service() != 0) syscall_exit(2);

    printf("[ahcid] ready; entering main loop\n");
    ahcid_main_loop();
    syscall_exit(0);
}
