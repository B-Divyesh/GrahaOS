// kernel/elf.c
#include "elf.h"
#include "../arch/x86_64/mm/pmm.h"
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

bool elf_load(void *elf_data, uint64_t *entry_point, uint64_t *new_cr3) {
    Elf64_Ehdr *header = (Elf64_Ehdr *)elf_data;

    if (!elf_validate_header(header)) return false;

    vmm_address_space_t *proc_space = vmm_create_address_space();
    if (!proc_space) return false;

    Elf64_Phdr *p_headers = (Elf64_Phdr *)((uintptr_t)elf_data + header->e_phoff);
    for (int i = 0; i < header->e_phnum; i++) {
        Elf64_Phdr *phdr = &p_headers[i];

        if (phdr->p_type == PT_LOAD) {
            uint64_t page_count = (phdr->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
            void *phys_addr = pmm_alloc_pages(page_count);
            if (!phys_addr) return false;

            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (phdr->p_flags & PF_W) flags |= PTE_WRITABLE;
            if (!(phdr->p_flags & PF_X)) flags |= PTE_NX;

            if (!vmm_map_range(proc_space, phdr->p_vaddr, (uint64_t)phys_addr, phdr->p_memsz, flags)) {
                return false;
            }

            void *dest_virt = (void*)((uintptr_t)phys_addr + g_hhdm_offset);
            void *src = (void*)((uintptr_t)elf_data + phdr->p_offset);
            
            elf_memset(dest_virt, 0, phdr->p_memsz);
            elf_memcpy(dest_virt, src, phdr->p_filesz);
        }
    }

    *entry_point = header->e_entry;
    *new_cr3 = vmm_get_pml4_phys(proc_space);

    return true;
}