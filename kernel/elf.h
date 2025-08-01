// kernel/elf.h
// ELF64 format definitions for GrahaOS
// Based on the ELF specification for x86_64
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../arch/x86_64/mm/vmm.h"

// ELF identification indices
#define EI_MAG0 0 // File identification byte 0 index
#define EI_MAG1 1 // File identification byte 1 index
#define EI_MAG2 2 // File identification byte 2 index
#define EI_MAG3 3 // File identification byte 3 index
#define EI_CLASS 4 // File class byte index
#define EI_DATA 5 // Data encoding byte index
#define EI_VERSION 6 // File version byte index
#define EI_NIDENT 16 // Size of e_ident array

// ELF magic number
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

// ELF classes
#define ELFCLASS32 1 // 32-bit objects
#define ELFCLASS64 2 // 64-bit objects

// ELF data encodings
#define ELFDATA2LSB 1 // Little-endian
#define ELFDATA2MSB 2 // Big-endian

// ELF file types
#define ET_NONE 0 // No file type
#define ET_REL 1 // Relocatable file
#define ET_EXEC 2 // Executable file
#define ET_DYN 3 // Dynamic linking file
#define ET_CORE 4 // Core file

// ELF machine types
#define EM_X86_64 62 // AMD x86-64 architecture

// Program header types
#define PT_NULL 0 // Unused entry
#define PT_LOAD 1 // Loadable segment
#define PT_DYNAMIC 2 // Dynamic linking information
#define PT_INTERP 3 // Interpreter information
#define PT_NOTE 4 // Auxiliary information
#define PT_SHLIB 5 // Reserved
#define PT_PHDR 6 // Program header table itself

// Program header flags
#define PF_X 0x1 // Execute permission
#define PF_W 0x2 // Write permission
#define PF_R 0x4 // Read permission

// ELF64 header structure
typedef struct {
    unsigned char e_ident[EI_NIDENT]; // ELF identification
    uint16_t e_type; // Object file type
    uint16_t e_machine; // Architecture
    uint32_t e_version; // Object file version
    uint64_t e_entry; // Entry point virtual address
    uint64_t e_phoff; // Program header table file offset
    uint64_t e_shoff; // Section header table file offset
    uint32_t e_flags; // Processor-specific flags
    uint16_t e_ehsize; // ELF header size in bytes
    uint16_t e_phentsize; // Program header table entry size
    uint16_t e_phnum; // Program header table entry count
    uint16_t e_shentsize; // Section header table entry size
    uint16_t e_shnum; // Section header table entry count
    uint16_t e_shstrndx; // Section header string table index
} __attribute__((packed)) Elf64_Ehdr;

// ELF64 program header structure
typedef struct {
    uint32_t p_type; // Segment type
    uint32_t p_flags; // Segment flags
    uint64_t p_offset; // Segment file offset
    uint64_t p_vaddr; // Segment virtual address
    uint64_t p_paddr; // Segment physical address
    uint64_t p_filesz; // Segment size in file
    uint64_t p_memsz; // Segment size in memory
    uint64_t p_align; // Segment alignment
} __attribute__((packed)) Elf64_Phdr;

/**
 * Validate an ELF64 header
 * @param header Pointer to the ELF header
 * @return 1 if valid, 0 if invalid
 */
int elf_validate_header(const Elf64_Ehdr *header);

/**
 * @brief Loads an ELF executable into a new address space.
 * @param elf_data Pointer to the ELF file data in memory.
 * @param entry_point Pointer to store the executable's entry point address.
 * @param new_cr3 Pointer to store the physical address of the new process's PML4.
 * @return True on success, false on failure.
 */
bool elf_load(void *elf_data, uint64_t *entry_point, uint64_t *new_cr3);