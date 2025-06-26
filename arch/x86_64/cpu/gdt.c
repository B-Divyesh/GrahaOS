#include "gdt.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h" // For g_hhdm_offset
#include "./sched/sched.h"  // For KERNEL_STACK_SIZE

// --- GDT is now larger and reordered for sysretq ---
// 0: NULL
// 1: Kernel Code (0x08)
// 2: Kernel Data (0x10)
// 3: User Data   (0x18) <-- Swapped
// 4: User Code   (0x20) <-- Swapped
// 5,6: TSS (takes 2 entries, starts at 0x28)
#define GDT_ENTRIES 7

// GDT Selectors
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define USER_DATA_SELECTOR   0x18
#define USER_CODE_SELECTOR   0x20
#define TSS_SELECTOR         0x28

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_pointer;

// --- THE FIX ---
// The TSS must be a global, non-static variable so the scheduler can access it.
struct tss kernel_tss;

// External assembly functions
extern void gdt_load(struct gdt_ptr *gdt_ptr);
extern void tss_load(uint16_t selector);

// Helper function to set up a standard GDT entry
static void gdt_set_gate(int num, uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = 0x0000;
    gdt[num].base_middle = 0x00;
    gdt[num].base_high   = 0x00;
    gdt[num].limit_low   = 0xFFFF; // Limit is ignored in 64-bit mode but set for completeness
    gdt[num].granularity = granularity;
    gdt[num].access      = access;
}

// Helper to set the TSS gate
static void gdt_set_tss(int num, uint64_t base, uint16_t limit) {
    struct tss_entry *tss_desc = (struct tss_entry *)&gdt[num];
    tss_desc->limit_low = limit;
    tss_desc->base_low = base & 0xFFFF;
    tss_desc->base_mid1 = (base >> 16) & 0xFF;
    tss_desc->access = 0x89; // Present, DPL=0, Type=TSS (Available)
    tss_desc->limit_high_and_flags = 0x00; // Granularity is 0 for TSS
    tss_desc->base_mid2 = (base >> 24) & 0xFF;
    tss_desc->base_high = base >> 32;
    tss_desc->reserved = 0;
}

void gdt_init(void) {
    // Set up GDT pointer
    gdt_pointer.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gdt_pointer.base  = (uint64_t)&gdt;

    // 0: NULL descriptor
    gdt_set_gate(0, 0x00, 0x00);
    
    // 1: Kernel Code Segment (Selector 0x08)
    // Access: 0x9A = P=1, DPL=0, S=1, Type=Code, Conforming=0, Readable=1
    // Granularity: 0xA0 = G=1, D/B=0, L=1 (64-bit)
    gdt_set_gate(1, 0x9A, 0xA0);
    
    // 2: Kernel Data Segment (Selector 0x10)
    // Access: 0x92 = P=1, DPL=0, S=1, Type=Data, Writable=1
    // Granularity: 0xC0 = G=1, D/B=1 (32-bit stack)
    gdt_set_gate(2, 0x92, 0xC0);

    // --- User mode gates (REORDERED for sysretq) ---
    // 3: User Data (Selector 0x18)
    // Access: 0xF2 = P=1, DPL=3, S=1, Type=Data, Writable=1
    gdt_set_gate(3, 0xF2, 0xC0);
    
    // 4: User Code (Selector 0x20)
    // Access: 0xFA = P=1, DPL=3, S=1, Type=Code, Readable=1
    gdt_set_gate(4, 0xFA, 0xA0);

    // --- TSS setup ---
    // Allocate and map a stack for the TSS RSP0 field.
    // This code now runs AFTER PMM and VMM are initialized.
    // FIXED: Allocate multiple pages for TSS RSP0 stack
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;  // This requires including sched.h
    void *tss_stack_phys = pmm_alloc_pages(num_pages);
    if (!tss_stack_phys) {
        asm volatile ("cli; hlt");
    }
    
    // The stack pointer must be a VIRTUAL address pointing to the TOP of the stack
    uint64_t tss_stack_virt_base = (uint64_t)tss_stack_phys + g_hhdm_offset;
    uint64_t tss_stack_virt_top = tss_stack_virt_base + KERNEL_STACK_SIZE;
    
    // CRITICAL: Map ALL pages of the kernel stack
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = tss_stack_virt_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)tss_stack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }
    // Initialize TSS structure, setting only the kernel stack pointer.
    kernel_tss.rsp0 = tss_stack_virt_top;
    
    // Set the TSS descriptor in the GDT (spans two entries).
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(kernel_tss) - 1);

    // Load GDT and then TSS.
    gdt_load(&gdt_pointer);
    tss_load(TSS_SELECTOR);
}