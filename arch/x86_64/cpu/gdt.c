#include "gdt.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "./sched/sched.h"
#include "smp.h"  // For cpu_local_t

// GDT configuration constants
#define GDT_ENTRIES 7
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10
#define USER_DATA_SELECTOR   0x18
#define USER_CODE_SELECTOR   0x20
#define TSS_SELECTOR         0x28

// External assembly functions
extern void gdt_load(struct gdt_ptr *gdt_ptr);
extern void tss_load(uint16_t selector);

// Helper function to set up a standard GDT entry
static void gdt_set_gate(struct gdt_entry *gdt, int num, uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = 0x0000;
    gdt[num].base_middle = 0x00;
    gdt[num].base_high   = 0x00;
    gdt[num].limit_low   = 0xFFFF;
    gdt[num].granularity = granularity;
    gdt[num].access      = access;
}

// Helper to set the TSS gate
static void gdt_set_tss(struct gdt_entry *gdt, int num, uint64_t base, uint16_t limit) {
    struct tss_entry *tss_desc = (struct tss_entry *)&gdt[num];
    tss_desc->limit_low = limit;
    tss_desc->base_low = base & 0xFFFF;
    tss_desc->base_mid1 = (base >> 16) & 0xFF;
    tss_desc->access = 0x89; // Present, DPL=0, Type=TSS (Available)
    tss_desc->limit_high_and_flags = 0x00;
    tss_desc->base_mid2 = (base >> 24) & 0xFF;
    tss_desc->base_high = base >> 32;
    tss_desc->reserved = 0;
}

// Initialize GDT for a specific CPU
void gdt_init_for_cpu(uint32_t cpu_id) {
    // Get the per-CPU data structure
    cpu_local_t *cpu_local = &g_cpu_locals[cpu_id];
    
    // Set up GDT pointer for this CPU's GDT
    struct gdt_ptr gdt_pointer;
    gdt_pointer.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gdt_pointer.base = (uint64_t)&cpu_local->gdt;
    
    // Set up GDT entries
    gdt_set_gate(cpu_local->gdt, 0, 0x00, 0x00); // NULL
    gdt_set_gate(cpu_local->gdt, 1, 0x9A, 0xA0); // Kernel Code
    gdt_set_gate(cpu_local->gdt, 2, 0x92, 0xC0); // Kernel Data
    gdt_set_gate(cpu_local->gdt, 3, 0xF2, 0xC0); // User Data
    gdt_set_gate(cpu_local->gdt, 4, 0xFA, 0xA0); // User Code
    
    // Allocate kernel stack for TSS RSP0 field
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    void *tss_stack_phys = pmm_alloc_pages(num_pages);
    if (!tss_stack_phys) {
        asm volatile ("cli; hlt");
    }
    
    uint64_t tss_stack_virt_base = (uint64_t)tss_stack_phys + g_hhdm_offset;
    uint64_t tss_stack_virt_top = tss_stack_virt_base + KERNEL_STACK_SIZE;
    
    // Map all pages of the kernel stack
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t page_virt = tss_stack_virt_base + (i * PAGE_SIZE);
        uint64_t page_phys = (uint64_t)tss_stack_phys + (i * PAGE_SIZE);
        vmm_map_page(vmm_get_kernel_space(), page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE);
    }
    
    // Initialize this CPU's TSS
    cpu_local->tss.rsp0 = tss_stack_virt_top;
    
    // Set the TSS descriptor in this CPU's GDT
    gdt_set_tss(cpu_local->gdt, 5, (uint64_t)&cpu_local->tss, sizeof(cpu_local->tss) - 1);
    
    // Load this CPU's GDT and TSS
    gdt_load(&gdt_pointer);
    tss_load(TSS_SELECTOR);
}

// Legacy function for backward compatibility
void gdt_init(void) {
    gdt_init_for_cpu(0);  // Initialize for BSP (CPU 0)
}