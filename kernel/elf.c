// kernel/elf.c
#include "elf.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/drivers/serial/serial.h"
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
    serial_write("[ELF] elf_load called\n");

    if (!file_data || !entry_point || !cr3) {
        serial_write("[ELF] ERROR: Invalid parameters!\n");
        framebuffer_draw_string("ELF: Invalid parameters", 10, 400, COLOR_RED, 0x00101828);
        return false;
    }

    serial_write("[ELF] Parameters OK, validating header...\n");
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_data;

    // Validate ELF magic
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        serial_write("[ELF] ERROR: Invalid magic!\n");
        framebuffer_draw_string("ELF: Invalid magic", 10, 420, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's 64-bit
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        serial_write("[ELF] ERROR: Not 64-bit!\n");
        framebuffer_draw_string("ELF: Not 64-bit", 10, 440, COLOR_RED, 0x00101828);
        return false;
    }

    // Check if it's executable
    if (ehdr->e_type != ET_EXEC) {
        serial_write("[ELF] ERROR: Not executable!\n");
        framebuffer_draw_string("ELF: Not executable", 10, 460, COLOR_RED, 0x00101828);
        return false;
    }

    serial_write("[ELF] Header validated, drawing framebuffer msg...\n");
    framebuffer_draw_string("ELF: Header validated", 10, 420, COLOR_GREEN, 0x00101828);
    serial_write("[ELF] Framebuffer msg drawn\n");

    // Create new address space
    serial_write("[ELF] Creating address space...\n");
    vmm_address_space_t *addr_space = vmm_create_address_space();
    if (!addr_space) {
        serial_write("[ELF] ERROR: Failed to create address space!\n");
        framebuffer_draw_string("ELF: Failed to create address space", 10, 480, COLOR_RED, 0x00101828);
        return false;
    }
    serial_write("[ELF] Address space created\n");

    // Map user stack FIRST (critical fix)
    const uint64_t user_stack_top = 0x00007FFFFFFFE000;
    const uint64_t user_stack_size = 0x200000; // 2MB stack

    // Allocate physical pages for stack
    serial_write("[ELF] Mapping user stack (");
    serial_write_dec(user_stack_size / PAGE_SIZE);
    serial_write(" pages)...\n");

    size_t stack_pages = user_stack_size / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        void *stack_page_phys = pmm_alloc_page();
        if (!stack_page_phys) {
            serial_write("[ELF] ERROR: Failed to allocate stack page!\n");
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
            serial_write("[ELF] ERROR: Failed to map stack page!\n");
            framebuffer_draw_string("ELF: Failed to map stack page", 10, 520, COLOR_RED, 0x00101828);
            return false;
        }
    }

    serial_write("[ELF] Stack mapped successfully\n");
    framebuffer_draw_string("ELF: Stack mapped", 10, 440, COLOR_GREEN, 0x00101828);

    // Get program headers
    serial_write("[ELF] Getting program headers...\n");
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    serial_write("[ELF] Program header count: ");
    serial_write_dec(ehdr->e_phnum);
    serial_write("\n");

    // Load each program segment
    for (int i = 0; i < ehdr->e_phnum; i++) {
        serial_write("[ELF] Processing segment ");
        serial_write_dec(i);
        serial_write(" type=");
        serial_write_hex(phdr[i].p_type);
        serial_write("\n");

        if (phdr[i].p_type != PT_LOAD) {
            serial_write("[ELF] Skipping non-LOAD segment\n");
            continue;
        }

        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t offset = phdr[i].p_offset;

        serial_write("[ELF] Loading segment: vaddr=");
        serial_write_hex(vaddr);
        serial_write(" memsz=");
        serial_write_hex(memsz);
        serial_write("\n");

        // Calculate pages needed
        uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t num_pages = (end_page - start_page) / PAGE_SIZE;

        serial_write("[ELF] Segment needs ");
        serial_write_dec(num_pages);
        serial_write(" pages\n");

        serial_write("[ELF] Drawing framebuffer msg...\n");
        framebuffer_draw_string("ELF: Loading segment", 10, 460 + (i * 20), COLOR_YELLOW, 0x00101828);
        serial_write("[ELF] Framebuffer msg drawn\n");

        // Map and load pages
        for (size_t j = 0; j < num_pages; j++) {
            serial_write("[ELF] Allocating page ");
            serial_write_dec(j);
            serial_write("/");
            serial_write_dec(num_pages);
            serial_write("...\n");

            void *page_phys = pmm_alloc_page();
            if (!page_phys) {
                serial_write("[ELF] ERROR: Out of memory!\n");
                framebuffer_draw_string("ELF: Out of memory", 10, 540, COLOR_RED, 0x00101828);
                return false;
            }

            serial_write("[ELF] Page allocated at ");
            serial_write_hex((uint64_t)page_phys);
            serial_write("\n");

            // Get virtual address for copying
            void *page_virt = (void *)((uint64_t)page_phys + g_hhdm_offset);
            serial_write("[ELF] Page virt addr: ");
            serial_write_hex((uint64_t)page_virt);
            serial_write("\n");

            // Clear the page
            serial_write("[ELF] Clearing page...\n");
            for (int k = 0; k < PAGE_SIZE; k++) {
                ((uint8_t *)page_virt)[k] = 0;
            }
            serial_write("[ELF] Page cleared\n");

            // Copy segment data
            uint64_t page_vaddr = start_page + (j * PAGE_SIZE);
            serial_write("[ELF] page_vaddr=");
            serial_write_hex(page_vaddr);
            serial_write("\n");

            if (page_vaddr < vaddr + filesz) {
                serial_write("[ELF] Copying segment data to page...\n");
                uint64_t copy_start = (page_vaddr < vaddr) ? vaddr - page_vaddr : 0;
                uint64_t copy_size = PAGE_SIZE - copy_start;

                if (page_vaddr + copy_size > vaddr + filesz) {
                    copy_size = (vaddr + filesz) - page_vaddr;
                }

                if (copy_size > 0) {
                    serial_write("[ELF] Copying ");
                    serial_write_dec(copy_size);
                    serial_write(" bytes...\n");

                    uint64_t file_offset = offset + (page_vaddr - vaddr);
                    if (page_vaddr < vaddr) {
                        file_offset = offset;
                    }

                    serial_write("[ELF] file_offset=");
                    serial_write_hex(file_offset);
                    serial_write(" copy_start=");
                    serial_write_hex(copy_start);
                    serial_write("\n");

                    uint8_t *src = (uint8_t *)file_data + file_offset;
                    uint8_t *dst = (uint8_t *)page_virt + copy_start;

                    serial_write("[ELF] src=");
                    serial_write_hex((uint64_t)src);
                    serial_write(" dst=");
                    serial_write_hex((uint64_t)dst);
                    serial_write("\n");

                    serial_write("[ELF] Starting memcpy loop...\n");
                    for (size_t k = 0; k < copy_size; k++) {
                        dst[k] = src[k];
                    }
                    serial_write("[ELF] Copy complete\n");
                }
            }

            // Determine page flags
            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;
            if (!(phdr[i].p_flags & PF_X)) flags |= PTE_NX;

            // Map the page
            serial_write("[ELF] Mapping page at vaddr=");
            serial_write_hex(page_vaddr);
            serial_write("...\n");

            if (!vmm_map_page(addr_space, page_vaddr, (uint64_t)page_phys, flags)) {
                serial_write("[ELF] ERROR: Failed to map page!\n");
                framebuffer_draw_string("ELF: Failed to map page", 10, 560, COLOR_RED, 0x00101828);
                return false;
            }
            serial_write("[ELF] Page mapped successfully\n");
        }
    }

    serial_write("[ELF] All pages loaded, drawing framebuffer msg...\n");
    framebuffer_draw_string("ELF: All segments loaded", 10, 540, COLOR_GREEN, 0x00101828);
    serial_write("[ELF] Framebuffer msg drawn\n");

    serial_write("[ELF] Setting output parameters...\n");
    *entry_point = ehdr->e_entry;
    *cr3 = vmm_get_pml4_phys(addr_space);
    serial_write("[ELF] entry_point=");
    serial_write_hex(*entry_point);
    serial_write(" cr3=");
    serial_write_hex(*cr3);
    serial_write("\n");

    serial_write("[ELF] Building entry point message...\n");
    char msg[64] = "ELF: Entry point: 0x";
    for (int i = 0; i < 16; i++) {
        char hex = "0123456789ABCDEF"[(*entry_point >> (60 - i * 4)) & 0xF];
        msg[20 + i] = hex;
    }
    msg[36] = '\0';

    serial_write("[ELF] Drawing entry point message...\n");
    framebuffer_draw_string(msg, 10, 560, COLOR_CYAN, 0x00101828);
    serial_write("[ELF] Entry point message drawn\n");

    serial_write("[ELF] elf_load returning true\n");
    return true;
}