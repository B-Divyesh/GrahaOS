#include "vmm.h"
#include "pmm.h"
#include "../../../drivers/video/framebuffer.h"

// Maximum number of address spaces (should match MAX_TASKS)
#define MAX_ADDRESS_SPACES 32

// --- Global Variable Definition ---
// This is where the global variable is actually created and stored.
uint64_t g_hhdm_offset = 0;

// The kernel's address space.
static vmm_address_space_t kernel_space;

// --- MODIFICATION: Make the pool non-static so sched.c can access it ---
vmm_address_space_t address_space_pool[MAX_ADDRESS_SPACES];
static int next_address_space_idx = 0;

// Internal helper functions
static void vmm_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n-- > 0) {
        *p++ = (uint8_t)c;
    }
}

// Helper function to get the physical address of the PML4 table.
static uint64_t get_pml4_phys(vmm_address_space_t *addr_space) {
    if (!addr_space || !addr_space->top_level) return 0;
    return (uint64_t)addr_space->top_level - g_hhdm_offset;
}

// Helper function to align addresses to page boundaries
static uint64_t align_down(uint64_t addr) {
    return addr & PAGE_MASK;
}

static uint64_t align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & PAGE_MASK;
}

void vmm_switch_address_space(vmm_address_space_t *addr_space) {
    uint64_t pml4_phys = get_pml4_phys(addr_space);
    asm volatile ("mov %0, %%cr3" : : "r" (pml4_phys) : "memory");
}

// Simple function to convert a hex value to a string
static void vmm_hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17]; // 16 hex digits + null terminator
    int i = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0 && i < 16) {
        temp[i++] = hex_chars[value & 0xF];
        value >>= 4;
    }

    // Reverse the string
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}

bool vmm_map_page(vmm_address_space_t *addr_space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!addr_space || !addr_space->top_level) return false;

    // Calculate indices for each page table level.
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    // Level 1: Page Map Level 4 (PML4)
    uint64_t *pml4 = addr_space->top_level;
    if (!(pml4[pml4_index] & PTE_PRESENT)) {
        void *pdpt_phys = pmm_alloc_page();
        if (!pdpt_phys) return false;
        uint64_t *pdpt_virt = (uint64_t*)((uint64_t)pdpt_phys + g_hhdm_offset);
        vmm_memset(pdpt_virt, 0, PAGE_SIZE);
        pml4[pml4_index] = (uint64_t)pdpt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Level 2: Page Directory Pointer Table (PDPT)
    uint64_t *pdpt = (uint64_t*)((pml4[pml4_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) {
        void *pd_phys = pmm_alloc_page();
        if (!pd_phys) return false;
        uint64_t *pd_virt = (uint64_t*)((uint64_t)pd_phys + g_hhdm_offset);
        vmm_memset(pd_virt, 0, PAGE_SIZE);
        pdpt[pdpt_index] = (uint64_t)pd_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Level 3: Page Directory (PD)
    uint64_t *pd = (uint64_t*)((pdpt[pdpt_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pd[pd_index] & PTE_PRESENT)) {
        void *pt_phys = pmm_alloc_page();
        if (!pt_phys) return false;
        uint64_t* pt_virt = (uint64_t*)((uint64_t)pt_phys + g_hhdm_offset);
        vmm_memset(pt_virt, 0, PAGE_SIZE);
        pd[pd_index] = (uint64_t)pt_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    // Level 4: Page Table (PT)
    uint64_t *pt = (uint64_t*)((pd[pd_index] & PAGE_MASK) + g_hhdm_offset);
    pt[pt_index] = phys | flags;

    return true;
}

bool vmm_map_range(vmm_address_space_t *addr_space, uint64_t virt_start,
                   uint64_t phys_start, uint64_t size, uint64_t flags) {
    // Align addresses and size to page boundaries
    uint64_t virt_aligned = align_down(virt_start);
    uint64_t phys_aligned = align_down(phys_start);
    uint64_t size_aligned = align_up(size + (virt_start - virt_aligned));

    // Map each page in the range
    for (uint64_t offset = 0; offset < size_aligned; offset += PAGE_SIZE) {
        if (!vmm_map_page(addr_space, virt_aligned + offset, phys_aligned + offset, flags)) {
            return false;
        }
    }

    return true;
}

vmm_address_space_t *vmm_get_kernel_space(void) {
    return &kernel_space;
}

// --- CORRECTED vmm_init ---
void vmm_init(
    struct limine_memmap_response *memmap_resp,
    struct limine_framebuffer_response *fb_resp,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t hhdm_offset
) {
    // Set the global variable so other modules can use it.
    g_hhdm_offset = hhdm_offset;

    // 1. Create a new, blank address space for the kernel.
    void *pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        asm volatile ("cli; hlt");
    }
    kernel_space.top_level = (uint64_t*)((uint64_t)pml4_phys + g_hhdm_offset);
    vmm_memset(kernel_space.top_level, 0, PAGE_SIZE);

    // 2. Map all of physical memory to the higher half.
    for (uint64_t i = 0; i < memmap_resp->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap_resp->entries[i];
        if (!vmm_map_range(&kernel_space, entry->base + g_hhdm_offset, entry->base, entry->length, PTE_PRESENT | PTE_WRITABLE | PTE_NX)) {
            asm volatile ("cli; hlt");
        }
    }

    // 3. Map the kernel's code and data sections.
    if (!vmm_map_range(&kernel_space, kernel_virt_base, kernel_phys_base, 256 * 1024 * 1024, PTE_PRESENT | PTE_WRITABLE)) {
        asm volatile ("cli; hlt");
    }

    // 4. Map the framebuffer.
    for (uint64_t i = 0; i < fb_resp->framebuffer_count; i++) {
        struct limine_framebuffer *fb = fb_resp->framebuffers[i];
        uint64_t fb_phys_addr = (uint64_t)fb->address;
        uint64_t fb_size = fb->height * fb->pitch;
        if (!vmm_map_range(&kernel_space, fb_phys_addr + g_hhdm_offset, fb_phys_addr, fb_size, PTE_PRESENT | PTE_WRITABLE | PTE_NX)) {
            asm volatile ("cli; hlt");
        }
    }

    // 5. Switch to the new address space ONLY AFTER all mapping is complete.
    vmm_switch_address_space(&kernel_space);
}

uint64_t vmm_get_pml4_phys(vmm_address_space_t *addr_space) {
    return get_pml4_phys(addr_space);
}

void vmm_switch_address_space_phys(uint64_t pml4_phys) {
    asm volatile ("mov %0, %%cr3" : : "r" (pml4_phys) : "memory");
}

vmm_address_space_t* vmm_create_address_space(void) {
    // Check if we have space for another address space
    if (next_address_space_idx >= MAX_ADDRESS_SPACES) {
        return NULL;
    }

    vmm_address_space_t *space = &address_space_pool[next_address_space_idx++];
    
    // Allocate a new PML4 table
    void *pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        return NULL;
    }
    
    // Set up the address space structure
    space->top_level = (uint64_t*)((uint64_t)pml4_phys + g_hhdm_offset);
    vmm_memset(space->top_level, 0, PAGE_SIZE);

    // Every address space needs the kernel and HHDM mapped.
    // We can copy the kernel mappings from the initial kernel_space.
    // The top half of the PML4 (entries 256-511) are for the kernel.
    vmm_address_space_t *k_space = vmm_get_kernel_space();
    for (int i = 256; i < 512; i++) {
        space->top_level[i] = k_space->top_level[i];
    }

    return space;
}