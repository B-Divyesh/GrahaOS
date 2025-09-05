// kernel/elf.c
#include "elf.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../drivers/video/framebuffer.h"
#include <stddef.h> // For NULL
#include <stdint.h>

static void *elf_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) pdest[i] = psrc[i];
    return dest;
}

static void *elf_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

/**
 * Validate an ELF64 header for x86_64 executables
 * @param header Pointer to the ELF header to validate
 * @return 1 if the header is valid, 0 otherwise
 */
int elf_validate_header(const Elf64_Ehdr *header) {
    // Check ELF magic number
    if (header->e_ident[EI_MAG0] != ELFMAG0 ||
        header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 ||
        header->e_ident[EI_MAG3] != ELFMAG3) {
        return 0; // Invalid magic number
    }

    // Check for 64-bit ELF
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        return 0; // Not a 64-bit ELF
    }

    // Check for little-endian encoding
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        return 0; // Not little-endian
    }

    // Check for executable file type
    if (header->e_type != ET_EXEC) {
        return 0; // Not an executable
    }

    // Check for x86_64 architecture
    if (header->e_machine != EM_X86_64) {
        return 0; // Not x86_64
    }

    // Check for valid entry point (should be non-zero for executables)
    if (header->e_entry == 0) {
        return 0; // Invalid entry point
    }

    return 1; // Header is valid
}

bool elf_load(void *file_data, uint64_t *entry_point, uint64_t *cr3) {
    if (!file_data || !entry_point || !cr3) {
        framebuffer_draw_string("ELF: Invalid parameters", 10, 400, COLOR_RED, 0x00101828);
        return false;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_data;

    // Validate ELF magic
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        framebuffer_draw_string("ELF: Invalid magic", 10, 420, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's 64-bit
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        framebuffer_draw_string("ELF: Not 64-bit", 10, 440, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's executable
    if (ehdr->e_type != ET_EXEC) {
        framebuffer_draw_string("ELF: Not executable", 10, 460, COLOR_RED, 0x00101828);
        return false;
    }

    framebuffer_draw_string("ELF: Header validated", 10, 420, COLOR_GREEN, 0x00101828);

    // Create new address space
    vmm_address_space_t *addr_space = vmm_create_address_space();
    if (!addr_space) {
        framebuffer_draw_string("ELF: Failed to create address space", 10, 480, COLOR_RED, 0x00101828);
        return false;
    }

    // Map user stack FIRST (critical fix)
    const uint64_t user_stack_top = 0x00007FFFFFFFE000;
    const uint64_t user_stack_size = 0x200000; // 2MB stack
    
    // Allocate physical pages for stack
    size_t stack_pages = user_stack_size / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        void *stack_page_phys = pmm_alloc_page();
        if (!stack_page_phys) {
            framebuffer_draw_string("ELF: Failed to allocate stack", 10, 500, COLOR_RED, 0x00101828);
            return false;
        }
        
        // Clear the page
        void *stack_page_virt = (void *)((uint64_t)stack_page_phys + g_hhdm_offset);
        for (int j = 0; j < PAGE_SIZE; j++) {
            ((uint8_t *)stack_page_virt)[j] = 0;
        }
        
        // Map the stack page
        uint64_t stack_vaddr = user_stack_top - user_stack_size + (i * PAGE_SIZE);
        if (!vmm_map_page(addr_space, stack_vaddr, (uint64_t)stack_page_phys,
                         PTE_PRESENT | PTE_WRITABLE | PTE_USER)) {
            framebuffer_draw_string("ELF: Failed to map stack page", 10, 520, COLOR_RED, 0x00101828);
            return false;
        }
    }
    
    framebuffer_draw_string("ELF: Stack mapped", 10, 440, COLOR_GREEN, 0x00101828);

    // Get program headers
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);

    // Load each program segment
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t offset = phdr[i].p_offset;

        // Calculate pages needed
        uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t num_pages = (end_page - start_page) / PAGE_SIZE;

        framebuffer_draw_string("ELF: Loading segment", 10, 460 + (i * 20), COLOR_YELLOW, 0x00101828);

        // Map and load pages
        for (size_t j = 0; j < num_pages; j++) {
            void *page_phys = pmm_alloc_page();
            if (!page_phys) {
                framebuffer_draw_string("ELF: Out of memory", 10, 540, COLOR_RED, 0x00101828);
                return false;
            }

            // Get virtual address for copying
            void *page_virt = (void *)((uint64_t)page_phys + g_hhdm_offset);

            // Clear the page
            for (int k = 0; k < PAGE_SIZE; k++) {
                ((uint8_t *)page_virt)[k] = 0;
            }

            // Copy segment data
            uint64_t page_vaddr = start_page + (j * PAGE_SIZE);
            if (page_vaddr < vaddr + filesz) {
                uint64_t copy_start = (page_vaddr < vaddr) ? vaddr - page_vaddr : 0;
                uint64_t copy_size = PAGE_SIZE - copy_start;
                
                if (page_vaddr + copy_size > vaddr + filesz) {
                    copy_size = (vaddr + filesz) - page_vaddr;
                }

                if (copy_size > 0) {
                    uint64_t file_offset = offset + (page_vaddr - vaddr);
                    if (page_vaddr < vaddr) {
                        file_offset = offset;
                    }
                    
                    uint8_t *src = (uint8_t *)file_data + file_offset;
                    uint8_t *dst = (uint8_t *)page_virt + copy_start;
                    for (size_t k = 0; k < copy_size; k++) {
                        dst[k] = src[k];
                    }
                }
            }

            // Determine page flags
            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;
            if (!(phdr[i].p_flags & PF_X)) flags |= PTE_NX;

            // Map the page
            if (!vmm_map_page(addr_space, page_vaddr, (uint64_t)page_phys, flags)) {
                framebuffer_draw_string("ELF: Failed to map page", 10, 560, COLOR_RED, 0x00101828);
                return false;
            }
        }
    }

    framebuffer_draw_string("ELF: All segments loaded", 10, 540, COLOR_GREEN, 0x00101828);

    *entry_point = ehdr->e_entry;
    *cr3 = vmm_get_pml4_phys(addr_space);

    char msg[64] = "ELF: Entry point: 0x";
    for (int i = 0; i < 16; i++) {
        char hex = "0123456789ABCDEF"[(*entry_point >> (60 - i * 4)) & 0xF];
        msg[20 + i] = hex;
    }
    msg[36] = '\0';
    framebuffer_draw_string(msg, 10, 560, COLOR_CYAN, 0x00101828);

    return true;
}