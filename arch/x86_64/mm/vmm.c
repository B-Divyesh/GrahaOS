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
    // First, try to reuse a freed slot (top_level == NULL)
    vmm_address_space_t *space = NULL;
    for (int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (address_space_pool[i].top_level == NULL) {
            space = &address_space_pool[i];
            break;
        }
    }

    if (!space) {
        // No free slots available
        if (next_address_space_idx >= MAX_ADDRESS_SPACES) {
            return NULL;
        }
        space = &address_space_pool[next_address_space_idx++];
    }

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

// --- PHASE 7C HELPER FUNCTIONS ---

uint64_t vmm_get_physical_address(uint64_t cr3, uint64_t virt) {
    // Calculate indices for each page table level
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;
    uint64_t offset     = virt & 0xFFF;

    // Walk the page tables
    uint64_t *pml4 = (uint64_t*)(cr3 + g_hhdm_offset);
    if (!(pml4[pml4_index] & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)((pml4[pml4_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) return 0;

    uint64_t *pd = (uint64_t*)((pdpt[pdpt_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pd[pd_index] & PTE_PRESENT)) return 0;

    uint64_t *pt = (uint64_t*)((pd[pd_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pt[pt_index] & PTE_PRESENT)) return 0;

    // Return physical address with offset
    return (pt[pt_index] & PAGE_MASK) | offset;
}

bool vmm_map_page_by_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    // Calculate indices for each page table level
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    // Level 1: Page Map Level 4 (PML4)
    uint64_t *pml4 = (uint64_t*)(cr3 + g_hhdm_offset);
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

    // CRITICAL FIX: Invalidate TLB entry for this page
    // When mapping pages for another process, the TLB must be flushed
    // so the CPU picks up the new mapping when the process runs
    asm volatile("invlpg (%0)" : : "r" (virt) : "memory");

    return true;
}

void vmm_destroy_address_space(vmm_address_space_t *space) {
    if (!space || !space->top_level) return;

    uint64_t *pml4 = space->top_level;

    // Only walk the user half (PML4 entries 0-255).
    // Entries 256-511 are shared kernel mappings and must NOT be freed.
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;

        uint64_t *pdpt = (uint64_t*)((pml4[i] & PAGE_MASK) + g_hhdm_offset);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            if (pdpt[j] & PTE_LARGEPAGE) continue; // 1GB huge page - skip

            uint64_t *pd = (uint64_t*)((pdpt[j] & PAGE_MASK) + g_hhdm_offset);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_LARGEPAGE) continue; // 2MB huge page - skip

                uint64_t *pt = (uint64_t*)((pd[k] & PAGE_MASK) + g_hhdm_offset);

                // Free all mapped physical pages in this page table
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    pmm_free_page((void*)(pt[l] & PAGE_MASK));
                }

                // Free the PT page itself
                pmm_free_page((void*)(pd[k] & PAGE_MASK));
            }

            // Free the PD page itself
            pmm_free_page((void*)(pdpt[j] & PAGE_MASK));
        }

        // Free the PDPT page itself
        pmm_free_page((void*)(pml4[i] & PAGE_MASK));
    }

    // Free the PML4 page itself
    uint64_t pml4_phys = (uint64_t)pml4 - g_hhdm_offset;
    pmm_free_page((void*)pml4_phys);

    // Clear the pool entry so it can be reused
    space->top_level = NULL;
}

void vmm_destroy_address_space_by_cr3(uint64_t cr3) {
    // Find the address space in the pool
    for (int i = 0; i < MAX_ADDRESS_SPACES; i++) {
        if (address_space_pool[i].top_level &&
            vmm_get_pml4_phys(&address_space_pool[i]) == cr3) {
            vmm_destroy_address_space(&address_space_pool[i]);
            return;
        }
    }
}

void vmm_unmap_page_by_cr3(uint64_t cr3, uint64_t virt) {
    // Calculate indices for each page table level
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    // Walk the page tables
    uint64_t *pml4 = (uint64_t*)(cr3 + g_hhdm_offset);
    if (!(pml4[pml4_index] & PTE_PRESENT)) return;

    uint64_t *pdpt = (uint64_t*)((pml4[pml4_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) return;

    uint64_t *pd = (uint64_t*)((pdpt[pdpt_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pd[pd_index] & PTE_PRESENT)) return;

    uint64_t *pt = (uint64_t*)((pd[pd_index] & PAGE_MASK) + g_hhdm_offset);

    // Clear the page table entry and invalidate TLB
    pt[pt_index] = 0;
    asm volatile("invlpg (%0)" : : "r" (virt) : "memory");
}

// ---------------------------------------------------------------------------
// Phase 17: VA reservation, page protect, page-fault hook.
// ---------------------------------------------------------------------------

// Return pointer to the PTE for 'virt' in the given CR3, or NULL if any of
// the intermediate tables is absent. Does not allocate.
static uint64_t *vmm_walk_pte(uint64_t cr3, uint64_t virt) {
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t*)(cr3 + g_hhdm_offset);
    if (!(pml4[pml4_index] & PTE_PRESENT)) return NULL;
    uint64_t *pdpt = (uint64_t*)((pml4[pml4_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) return NULL;
    uint64_t *pd = (uint64_t*)((pdpt[pdpt_index] & PAGE_MASK) + g_hhdm_offset);
    if (!(pd[pd_index] & PTE_PRESENT)) return NULL;
    uint64_t *pt = (uint64_t*)((pd[pd_index] & PAGE_MASK) + g_hhdm_offset);
    return &pt[pt_index];
}

// Scan bottom-up from a high-enough offset to avoid the user binary's
// text/data/brk (typically < 0x1000_0000) and well below the user stack
// (at 0x7FFF_xxxx_xxxx). VMOs get a dedicated slab of VA in the mid-user
// region.
#define VMM_USER_SEARCH_BOTTOM  0x0000100000000000ULL  // 16 TiB
#define VMM_USER_SEARCH_TOP     0x0000500000000000ULL  // 80 TiB

uint64_t vmm_reserve_va_by_cr3(uint64_t cr3, uint64_t len) {
    if (len == 0 || (len & (PAGE_SIZE - 1)) != 0) return 0;
    uint64_t npages = len / PAGE_SIZE;

    // Bottom-up scan. In practice user pages at this region are empty and
    // the first try succeeds. Max 64K iterations of 4 KiB each = 256 MiB
    // before we give up, which matches VMO_MAX_SIZE.
    uint64_t cur = VMM_USER_SEARCH_BOTTOM;
    uint64_t stop = VMM_USER_SEARCH_TOP - len;
    while (cur <= stop) {
        bool clear = true;
        for (uint64_t p = 0; p < npages; p++) {
            uint64_t va = cur + p * PAGE_SIZE;
            uint64_t *pte = vmm_walk_pte(cr3, va);
            if (pte && (*pte & PTE_PRESENT)) {
                clear = false;
                // Jump past the occupied page so we don't rescan.
                cur = va + PAGE_SIZE;
                break;
            }
        }
        if (clear) return cur;
    }
    return 0;
}

bool vmm_protect_page_by_cr3(uint64_t cr3, uint64_t virt, uint64_t new_flags) {
    uint64_t *pte = vmm_walk_pte(cr3, virt);
    if (!pte || !(*pte & PTE_PRESENT)) return false;
    uint64_t phys = *pte & PAGE_MASK;
    *pte = phys | (new_flags & ~PAGE_MASK) | PTE_PRESENT;
    asm volatile("invlpg (%0)" : : "r" (virt) : "memory");
    return true;
}

// Installed page-fault handler (Phase 17). NULL until vmo_init runs.
static vmm_pf_handler_t g_pf_handler = NULL;

void vmm_install_pf_handler(vmm_pf_handler_t fn) {
    // Single aligned 8-byte write is atomic on x86_64; no lock needed.
    g_pf_handler = fn;
}

int vmm_dispatch_pf(uint64_t fault_va, uint64_t error_code) {
    vmm_pf_handler_t fn = g_pf_handler;
    if (!fn) return -1;
    return fn(fault_va, error_code);
}