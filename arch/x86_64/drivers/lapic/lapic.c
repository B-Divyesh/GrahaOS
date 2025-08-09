// arch/x86_64/drivers/lapic/lapic.c
#include "lapic.h"
#include "../../mm/vmm.h"
#include "../../../../drivers/video/framebuffer.h"
#include <stddef.h>

// Default physical base address for the LAPIC
#define LAPIC_PHYS_BASE 0xFEE00000
#define LAPIC_SIZE      0x1000      // 4KB for LAPIC registers

// Virtual address where we'll map the LAPIC
#define LAPIC_VIRT_BASE 0xFFFFFFFF90000000

// Global virtual address for LAPIC registers
static volatile uint32_t *lapic_vaddr = NULL;
static bool lapic_initialized = false;

// External HHDM offset
extern uint64_t g_hhdm_offset;

// Helper function to read from a LAPIC register
static inline uint32_t lapic_read(uint32_t reg) {
    if (!lapic_vaddr) return 0;
    return lapic_vaddr[reg / 4];
}

// Helper function to write to a LAPIC register
static inline void lapic_write(uint32_t reg, uint32_t value) {
    if (!lapic_vaddr) return;
    lapic_vaddr[reg / 4] = value;
    // Read back to ensure write completion
    lapic_read(LAPIC_REG_ID);
}

// Check if LAPIC is available via CPUID
static bool lapic_check_support(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Check CPUID.01h:EDX[9] for APIC support
    asm volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0)
    );
    
    return (edx & (1 << 9)) != 0;
}

void lapic_init(void) {
    // Check if LAPIC is supported
    if (!lapic_check_support()) {
        framebuffer_draw_string("LAPIC not supported by CPU!", 10, 10, COLOR_RED, COLOR_BLACK);
        return;
    }
    
    // Map LAPIC registers if not already mapped
    if (!lapic_vaddr) {
        // Map the LAPIC MMIO region to a fixed virtual address in kernel space
        vmm_map_page(vmm_get_kernel_space(), 
                     LAPIC_VIRT_BASE, 
                     LAPIC_PHYS_BASE, 
                     PTE_PRESENT | PTE_WRITABLE | PTE_NX);
        
        lapic_vaddr = (volatile uint32_t *)LAPIC_VIRT_BASE;
    }
    
    // Clear Error Status Register
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);
    
    // Enable LAPIC by setting the enable bit in the Spurious Interrupt Vector register
    // Set spurious vector to 0xFF (255)
    uint32_t siv = lapic_read(LAPIC_REG_SIV);
    siv |= LAPIC_SIV_ENABLE | 0xFF;
    lapic_write(LAPIC_REG_SIV, siv);
    
    // Set Task Priority to 0 to accept all interrupts
    lapic_write(LAPIC_REG_TPR, 0);
    
    // Mask all LVT entries initially
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, LAPIC_LVT_MASKED);
    
    // Clear any pending interrupts
    lapic_eoi();
    
    lapic_initialized = true;
}

void lapic_eoi(void) {
    if (!lapic_vaddr || !lapic_initialized) return;
    // Write 0 to the EOI register to signal end of interrupt
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void) {
    if (!lapic_vaddr) return 0;
    // The LAPIC ID is in bits 24-31 of the ID register
    return (lapic_read(LAPIC_REG_ID) >> 24) & 0xFF;
}

bool lapic_is_enabled(void) {
    return lapic_initialized && lapic_vaddr != NULL;
}

volatile uint32_t* lapic_get_base(void) {
    return lapic_vaddr;
}