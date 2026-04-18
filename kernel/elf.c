// kernel/elf.c
#include "elf.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/drivers/serial/serial.h"
#include <stddef.h> // For NULL
#include <stdint.h>
#include "log.h"

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
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] elf_load called");

    if (!file_data || !entry_point || !cr3) {
        klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Invalid parameters!");
        framebuffer_draw_string("ELF: Invalid parameters", 10, 400, COLOR_RED, 0x00101828);
        return false;
    }

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Parameters OK, validating header...");
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_data;

    // Validate ELF magic
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Invalid magic!");
        framebuffer_draw_string("ELF: Invalid magic", 10, 420, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's 64-bit
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Not 64-bit!");
        framebuffer_draw_string("ELF: Not 64-bit", 10, 440, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's executable
    if (ehdr->e_type != ET_EXEC) {
        klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Not executable!");
        framebuffer_draw_string("ELF: Not executable", 10, 460, COLOR_RED, 0x00101828);
        return false;
    }

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Header validated, drawing framebuffer msg...");
    framebuffer_draw_string("ELF: Header validated", 10, 420, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Framebuffer msg drawn");

    // Create new address space
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Creating address space...");
    vmm_address_space_t *addr_space = vmm_create_address_space();
    if (!addr_space) {
        klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Failed to create address space!");
        framebuffer_draw_string("ELF: Failed to create address space", 10, 480, COLOR_RED, 0x00101828);
        return false;
    }
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Address space created");

    // NOTE: User stack is allocated by sched_create_user_process(), not here.
    // elf_load only loads code/data segments into the address space.

    // Get program headers
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Getting program headers...");
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Program header count: %lu", (unsigned long)(ehdr->e_phnum));

    // Load each program segment
    for (int i = 0; i < ehdr->e_phnum; i++) {
        klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Processing segment %lu type=0x%lx", (unsigned long)(i), (unsigned long)(phdr[i].p_type));

        if (phdr[i].p_type != PT_LOAD) {
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Skipping non-LOAD segment");
            continue;
        }

        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t offset = phdr[i].p_offset;

        klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Loading segment: vaddr=0x%lx memsz=0x%lx", (unsigned long)(vaddr), (unsigned long)(memsz));

        // Calculate pages needed
        uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t num_pages = (end_page - start_page) / PAGE_SIZE;

        klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Segment needs %lu pages", (unsigned long)(num_pages));

        klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Drawing framebuffer msg...");
        framebuffer_draw_string("ELF: Loading segment", 10, 460 + (i * 20), COLOR_YELLOW, 0x00101828);
        klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Framebuffer msg drawn");

        // Map and load pages
        for (size_t j = 0; j < num_pages; j++) {
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Allocating page %lu/%lu...", (unsigned long)(j), (unsigned long)(num_pages));

            void *page_phys = pmm_alloc_page();
            if (!page_phys) {
                klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Out of memory!");
                framebuffer_draw_string("ELF: Out of memory", 10, 540, COLOR_RED, 0x00101828);
                return false;
            }

            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Page allocated at 0x%lx", (unsigned long)((uint64_t)page_phys));

            // Get virtual address for copying
            void *page_virt = (void *)((uint64_t)page_phys + g_hhdm_offset);
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Page virt addr: 0x%lx", (unsigned long)((uint64_t)page_virt));

            // Clear the page
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Clearing page...");
            for (int k = 0; k < PAGE_SIZE; k++) {
                ((uint8_t *)page_virt)[k] = 0;
            }
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Page cleared");

            // Copy segment data
            uint64_t page_vaddr = start_page + (j * PAGE_SIZE);
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] page_vaddr=0x%lx", (unsigned long)(page_vaddr));

            if (page_vaddr < vaddr + filesz) {
                klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Copying segment data to page...");
                uint64_t copy_start = (page_vaddr < vaddr) ? vaddr - page_vaddr : 0;
                uint64_t copy_size = PAGE_SIZE - copy_start;

                if (page_vaddr + copy_size > vaddr + filesz) {
                    copy_size = (vaddr + filesz) - page_vaddr;
                }

                if (copy_size > 0) {
                    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Copying %lu bytes...", (unsigned long)(copy_size));

                    uint64_t file_offset = offset + (page_vaddr - vaddr);
                    if (page_vaddr < vaddr) {
                        file_offset = offset;
                    }

                    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] file_offset=0x%lx copy_start=0x%lx", (unsigned long)(file_offset), (unsigned long)(copy_start));

                    uint8_t *src = (uint8_t *)file_data + file_offset;
                    uint8_t *dst = (uint8_t *)page_virt + copy_start;

                    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] src=0x%lx dst=0x%lx", (unsigned long)((uint64_t)src), (unsigned long)((uint64_t)dst));

                    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Starting memcpy loop...");
                    for (size_t k = 0; k < copy_size; k++) {
                        dst[k] = src[k];
                    }
                    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Copy complete");
                }
            }

            // Determine page flags
            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;
            if (!(phdr[i].p_flags & PF_X)) flags |= PTE_NX;

            // Map the page
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Mapping page at vaddr=0x%lx...", (unsigned long)(page_vaddr));

            if (!vmm_map_page(addr_space, page_vaddr, (uint64_t)page_phys, flags)) {
                klog(KLOG_ERROR, SUBSYS_CORE, "[ELF] ERROR: Failed to map page!");
                framebuffer_draw_string("ELF: Failed to map page", 10, 560, COLOR_RED, 0x00101828);
                return false;
            }
            klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Page mapped successfully");
        }
    }

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] All pages loaded, drawing framebuffer msg...");
    framebuffer_draw_string("ELF: All segments loaded", 10, 540, COLOR_GREEN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Framebuffer msg drawn");

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Setting output parameters...");
    *entry_point = ehdr->e_entry;
    *cr3 = vmm_get_pml4_phys(addr_space);
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] entry_point=0x%lx cr3=0x%lx", (unsigned long)(*entry_point), (unsigned long)(*cr3));

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Building entry point message...");
    char msg[64] = "ELF: Entry point: 0x";
    for (int i = 0; i < 16; i++) {
        char hex = "0123456789ABCDEF"[(*entry_point >> (60 - i * 4)) & 0xF];
        msg[20 + i] = hex;
    }
    msg[36] = '\0';

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Drawing entry point message...");
    framebuffer_draw_string(msg, 10, 560, COLOR_CYAN, 0x00101828);
    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] Entry point message drawn");

    klog(KLOG_INFO, SUBSYS_CORE, "[ELF] elf_load returning true");
    return true;
}