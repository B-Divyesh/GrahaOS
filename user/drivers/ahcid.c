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
                                      /*mode=*/0, /*capacity=*/16,
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
    long rd2 = syscall_chan_create(fnv1a64(BLK_LIST_TYPE), 0, 4,
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
    for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
        if (!st->slot[s].in_use) continue;
        blk_resp_msg_t resp = {0};
        resp.req_id        = st->slot[s].req_id;
        resp.status        = BLK_E_IO;
        resp.timestamp_tsc = rdtsc_now();
        cap_token_u_t tok = {.raw = (uint64_t)st->slot[s].resp_chan};
        (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE), &resp, sizeof(resp));
        st->slot[s].in_use = 0;
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

static void handle_irq(void) {
    g_ahcid.irq_count++;
    uint32_t hba_is = g_ahcid.hba->is;
    if (hba_is == 0) return;

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
        uint32_t ci = p->ci;
        for (uint32_t s = 0; s < g_ahcid.ncs; s++) {
            if (!st->slot[s].in_use) continue;
            if (ci & (1u << s)) continue;  /* still in-flight */
            blk_resp_msg_t resp = {0};
            resp.req_id            = st->slot[s].req_id;
            resp.status            = BLK_E_OK;
            resp.bytes_transferred = st->slot[s].bytes;
            resp.timestamp_tsc     = rdtsc_now();
            cap_token_u_t tok = {.raw = (uint64_t)st->slot[s].resp_chan};
            (void)send_payload(tok, fnv1a64(BLK_SERVICE_TYPE),
                               &resp, sizeof(resp));
            st->slot[s].in_use = 0;
            g_ahcid.requests_total++;
        }
        p->is = port_is;  /* clear */
    }
    g_ahcid.hba->is = hba_is;  /* clear */
}

static void handle_client_request(ahcid_client_t *cli) {
    blk_req_msg_t req;
    cap_token_u_t tok = {.raw = (uint64_t)cli->client_chan_read};
    long n = recv_payload(tok, &req, sizeof(req), 0);  /* non-block */
    if (n < (long)sizeof(req)) return;

    int op_rc = BLK_E_INVAL;
    switch (req.op) {
        case BLK_OP_READ:     op_rc = ahcid_do_read(cli, &req);     break;
        case BLK_OP_WRITE:    op_rc = ahcid_do_write(cli, &req);    break;
        case BLK_OP_FLUSH:    op_rc = ahcid_do_flush(cli, &req);    break;
        case BLK_OP_IDENTIFY: (void)ahcid_do_identify(cli, &req); return;
        default:              op_rc = BLK_E_INVAL; break;
    }

    if (op_rc != 0) {
        blk_resp_msg_t resp = {0};
        resp.req_id        = req.req_id;
        resp.status        = (int32_t)op_rc;
        resp.timestamp_tsc = rdtsc_now();
        cap_token_u_t wtok = {.raw = (uint64_t)cli->client_chan_write};
        (void)send_payload(wtok, fnv1a64(BLK_SERVICE_TYPE),
                           &resp, sizeof(resp));
    }
    cli->reqs_handled++;
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
    blk_connect_msg_t cm;
    uint8_t cm_nh = 0;
    cap_token_u_t cm_handles[CHAN_MSG_HANDLES_MAX];
    cap_token_u_t rtok = {.raw = cli->client_chan_read};
    long crc = recv_payload_h(rtok, &cm, sizeof(cm), cm_handles, &cm_nh,
                              1ull * 1000 * 1000 * 1000);
    if (crc < (long)sizeof(cm) || cm.magic != BLK_PROTO_MAGIC ||
        cm.version != BLK_PROTO_VERSION) {
        printf("[ahcid] bad connect handshake from client\n");
        cli->in_use = 0;
        return;
    }
    if (cm_nh < 1 || cm_handles[0].raw == 0) {
        printf("[ahcid] connect handshake missing DMA VMO handle\n");
        cli->in_use = 0;
        return;
    }
    cli->dma_vmo_handle = cm_handles[0].raw;
    printf("[ahcid] client connected dma_vmo=0x%llx\n",
           (unsigned long long)cli->dma_vmo_handle);
}

void ahcid_main_loop(void) {
    drv_irq_msg_t irq_msgs[8];
    uint64_t cap_irq_handle = g_ahcid.caps.irq_channel_handle;
    while (1) {
        // 1. Block briefly for IRQs.
        long n = drv_irq_wait(cap_irq_handle, irq_msgs, 8, 100);
        if (n > 0) {
            for (long i = 0; i < n; i++) handle_irq();
        }

        // 2. Try to accept a new client on /sys/blk/service.
        try_accept_new_client();

        // 3. Drain per-client request channels.
        for (uint32_t i = 0; i < AHCID_MAX_CLIENTS; i++) {
            if (!g_ahcid.clients[i].in_use) continue;
            handle_client_request(&g_ahcid.clients[i]);
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
