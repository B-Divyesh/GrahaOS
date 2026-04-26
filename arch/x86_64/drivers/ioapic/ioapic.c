// arch/x86_64/drivers/ioapic/ioapic.c
//
// Phase 21.1 — Minimal IOAPIC driver. See ioapic.h for scope and rationale.

#include "ioapic.h"
#include "../../mm/vmm.h"
#include "../../../../kernel/log.h"

// MMIO base. ACPI MADT parsing is deferred to Phase 22+; QEMU q35 + every
// Intel reference platform places the first IOAPIC here.
#define IOAPIC_PHYS_BASE   0xFEC00000ull
#define IOAPIC_MMIO_SIZE   0x1000ull

// Virtual address slot for IOAPIC MMIO. Distinct from LAPIC (which lives at
// 0xFFFFFFFF90000000) — pick the next 4 KiB-aligned slot.
#define IOAPIC_VIRT_BASE   0xFFFFFFFF90001000ull

// MMIO register offsets within the IOAPIC page.
#define IOAPIC_REG_SELECT  0x00u
#define IOAPIC_REG_WINDOW  0x10u

// Indirect register indices.
#define IOAPIC_IDX_ID      0x00u
#define IOAPIC_IDX_VERSION 0x01u
#define IOAPIC_IDX_REDTBL0 0x10u

// Redirection-table entry low-half bit layout (bits 0..31).
#define RTE_VECTOR_MASK    0x000000FFu
#define RTE_DM_PHYSICAL    (0u << 11)        // bit 11 = 0  → physical destination
#define RTE_DM_LOGICAL     (1u << 11)        // bit 11 = 1  → logical destination
#define RTE_POLARITY_LOW   (1u << 13)        // bit 13 = 1  → active-low
#define RTE_TRIGGER_LEVEL  (1u << 15)        // bit 15 = 1  → level-triggered
#define RTE_MASK_BIT       (1u << 16)        // bit 16 = 1  → masked
// Delivery mode is bits 8..10; we only use 000 (Fixed). bits 8,9,10 already 0.

static volatile uint32_t *s_ioapic_vaddr = NULL;
static bool   s_initialized   = false;
static uint8_t s_max_redir    = 0;

// IOAPIC indirect-register access. The 82093AA spec says writes to IOREGSEL
// must be 32-bit; same for IOREGWIN. Compiler can't be trusted to emit a
// 32-bit access through a `volatile uint8_t *`, hence the cast.
static inline void ioapic_write_idx(uint8_t idx, uint32_t value) {
    s_ioapic_vaddr[IOAPIC_REG_SELECT / 4] = (uint32_t)idx;
    s_ioapic_vaddr[IOAPIC_REG_WINDOW / 4] = value;
}

static inline uint32_t ioapic_read_idx(uint8_t idx) {
    s_ioapic_vaddr[IOAPIC_REG_SELECT / 4] = (uint32_t)idx;
    return s_ioapic_vaddr[IOAPIC_REG_WINDOW / 4];
}

void ioapic_init(void) {
    if (s_initialized) return;

    // Map the IOAPIC MMIO page into kernel virtual space. PTE_CACHEDISABLE +
    // PTE_WRITETHROUGH are mandatory for MMIO — caching IOAPIC register
    // accesses produces nondeterministic IRQ behaviour.
    vmm_map_page(vmm_get_kernel_space(),
                 IOAPIC_VIRT_BASE,
                 IOAPIC_PHYS_BASE,
                 PTE_PRESENT | PTE_WRITABLE | PTE_CACHEDISABLE | PTE_WRITETHROUGH | PTE_NX);

    s_ioapic_vaddr = (volatile uint32_t *)IOAPIC_VIRT_BASE;

    // Read IOAPIC ID and VERSION to confirm we're talking to real hardware.
    // ID lives in bits 24..27 of the IOAPICID register; VERSION holds the
    // version in low 8 bits and (max_redir_entry) in bits 16..23.
    uint32_t id_reg  = ioapic_read_idx(IOAPIC_IDX_ID);
    uint32_t ver_reg = ioapic_read_idx(IOAPIC_IDX_VERSION);

    uint8_t  apic_id    = (uint8_t)((id_reg >> 24) & 0x0F);
    uint8_t  version    = (uint8_t)(ver_reg & 0xFF);
    s_max_redir         = (uint8_t)((ver_reg >> 16) & 0xFF);

    // QEMU q35 reports version 0x11 + max_redir 0x17 (24 entries). Real
    // Intel parts report version 0x11..0x20 and max_redir 0x17 or 0x3F.
    // A wildly bogus value (>= 240 entries) means we read garbage — bail
    // and leave s_initialized = false so callers can detect.
    if (s_max_redir >= 240) {
        klog(KLOG_ERROR, SUBSYS_DRV,
             "[IOAPIC] bogus version register 0x%lx (max_redir=%lu) — driver disabled",
             (unsigned long)ver_reg, (unsigned long)s_max_redir);
        s_ioapic_vaddr = NULL;
        return;
    }

    // Mask every redirection entry. We don't know what BIOS / firmware left
    // in the table — explicit safe-state-on-init is the right discipline.
    // Set RTE_MASK_BIT in the low half; high half (destination) cleared.
    for (uint8_t i = 0; i <= s_max_redir; i++) {
        ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (i * 2),     RTE_MASK_BIT);
        ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (i * 2) + 1, 0u);
    }

    s_initialized = true;
    klog(KLOG_INFO, SUBSYS_DRV,
         "[IOAPIC] init ok: id=%lu version=0x%lx max_redir_entry=%lu (%lu RTEs masked)",
         (unsigned long)apic_id, (unsigned long)version,
         (unsigned long)s_max_redir, (unsigned long)(s_max_redir + 1));

    // Self-test: read back RTE[0] low half and confirm the mask bit reads as 1.
    uint32_t rte0_lo = ioapic_read_idx(IOAPIC_IDX_REDTBL0);
    if (!(rte0_lo & RTE_MASK_BIT)) {
        klog(KLOG_WARN, SUBSYS_DRV,
             "[IOAPIC] self-test: RTE[0] mask bit did not stick (lo=0x%lx)",
             (unsigned long)rte0_lo);
    } else {
        klog(KLOG_INFO, SUBSYS_DRV,
             "[IOAPIC] self-test ok: RTE[0] lo=0x%lx (masked)",
             (unsigned long)rte0_lo);
    }
}

void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t lapic_id,
                      bool level, bool active_low) {
    if (!s_initialized || gsi > s_max_redir) {
        klog(KLOG_WARN, SUBSYS_DRV,
             "[IOAPIC] route_irq(gsi=%lu vec=%lu) refused: %s",
             (unsigned long)gsi, (unsigned long)vector,
             s_initialized ? "gsi out of range" : "not initialized");
        return;
    }

    // Compose the low half. Leave masked at first; ioapic_unmask_irq drops
    // the mask bit. Delivery mode = 000 (Fixed). Dest mode = physical.
    uint32_t lo = (uint32_t)vector
                | RTE_DM_PHYSICAL
                | (active_low ? RTE_POLARITY_LOW    : 0u)
                | (level      ? RTE_TRIGGER_LEVEL   : 0u)
                | RTE_MASK_BIT;

    // Compose the high half. Physical destination mode places the LAPIC ID
    // in bits 56..63 of the 64-bit RTE, which is bits 24..31 of the high
    // 32-bit half.
    uint32_t hi = ((uint32_t)lapic_id) << 24;

    // Write high half FIRST then low half. Per the 82093AA datasheet, the
    // hardware latches the entry when the LOW half is written, so the
    // destination must already be in place. Writing the low half also
    // clears any spurious in-flight delivery state.
    ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2) + 1, hi);
    ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2),     lo);

    klog(KLOG_INFO, SUBSYS_DRV,
         "[IOAPIC] route gsi=%lu → vec=%lu lapic=%lu level=%lu polarity_low=%lu (masked)",
         (unsigned long)gsi, (unsigned long)vector, (unsigned long)lapic_id,
         (unsigned long)(level ? 1 : 0), (unsigned long)(active_low ? 1 : 0));
}

void ioapic_mask_irq(uint8_t gsi) {
    if (!s_initialized || gsi > s_max_redir) return;
    uint32_t lo = ioapic_read_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2));
    lo |= RTE_MASK_BIT;
    ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2), lo);
}

void ioapic_unmask_irq(uint8_t gsi) {
    if (!s_initialized || gsi > s_max_redir) return;
    uint32_t lo = ioapic_read_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2));
    lo &= ~RTE_MASK_BIT;
    ioapic_write_idx(IOAPIC_IDX_REDTBL0 + (gsi * 2), lo);
}

bool ioapic_is_present(void) { return s_initialized; }
uint8_t ioapic_max_redir_entry(void) { return s_max_redir; }
