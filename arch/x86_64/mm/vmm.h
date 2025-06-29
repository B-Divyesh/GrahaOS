#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../../kernel/limine.h" // Include for struct definitions

// --- Global Variable Declaration ---
// Any file that includes vmm.h can now access the HHDM offset.
extern uint64_t g_hhdm_offset;

// --- Page Table Entry Flags ---
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_CACHEDISABLE (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_LARGEPAGE  (1ULL << 7)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63) // No-Execute bit

// --- Virtual Memory Constants ---
#define PAGE_SIZE 4096
#define PAGE_MASK 0xFFFFFFFFFFFFF000ULL
#define MAX_ADDRESS_SPACES 32
// Virtual address space layout
#define KERNEL_VIRTUAL_BASE 0xFFFFFFFF80000000ULL

// Represents a top-level page map (PML4).
typedef struct {
    uint64_t *top_level;
} vmm_address_space_t;

extern vmm_address_space_t address_space_pool[MAX_ADDRESS_SPACES];

/**
 * @brief Initializes the Virtual Memory Manager.
 * @param memmap_resp The response from the central limine_memmap_request.
 * @param fb_resp The response from the central limine_framebuffer_request.
 * @param kernel_phys_base Physical base address of the kernel.
 * @param kernel_virt_base Virtual base address of the kernel.
 * @param hhdm_offset The virtual address offset of the higher-half direct map.
 */
void vmm_init(
    struct limine_memmap_response *memmap_resp,
    struct limine_framebuffer_response *fb_resp,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t hhdm_offset
);

/**
 * @brief Maps a virtual page to a physical page in a given address space.
 * @param addr_space The address space to modify.
 * @param virt The virtual address to map (must be page-aligned).
 * @param phys The physical address to map to (must be page-aligned).
 * @param flags The flags for the page table entry (e.g., PTE_WRITABLE).
 * @return True on success, false on failure (e.g., out of memory).
 */
bool vmm_map_page(vmm_address_space_t *addr_space, uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * @brief Maps a range of virtual pages to physical pages.
 * @param addr_space The address space to modify.
 * @param virt_start The starting virtual address (must be page-aligned).
 * @param phys_start The starting physical address (must be page-aligned).
 * @param size The size in bytes (will be rounded up to page boundaries).
 * @param flags The flags for the page table entries.
 * @return True on success, false on failure.
 */
bool vmm_map_range(vmm_address_space_t *addr_space, uint64_t virt_start,
                   uint64_t phys_start, uint64_t size, uint64_t flags);

/**
 * @brief Switches the current address space by loading a new PML4 into CR3.
 * @param addr_space The address space to switch to.
 */
void vmm_switch_address_space(vmm_address_space_t *addr_space);

/**
 * @brief Gets the current kernel address space.
 * @return Pointer to the kernel address space.
 */
vmm_address_space_t *vmm_get_kernel_space(void);

// --- NEW FUNCTIONS FOR PHASE 5A ---

/**
 * @brief Creates a new address space for a process.
 * @return Pointer to the new address space, or NULL on failure.
 */
vmm_address_space_t* vmm_create_address_space(void);

/**
 * @brief Gets the physical address of a PML4 table.
 * @param addr_space The address space.
 * @return Physical address of the PML4 table.
 */
uint64_t vmm_get_pml4_phys(vmm_address_space_t *addr_space);

/**
 * @brief Switches to an address space using its physical PML4 address.
 * @param pml4_phys Physical address of the PML4 table.
 */
void vmm_switch_address_space_phys(uint64_t pml4_phys);