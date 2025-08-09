// arch/x86_64/cpu/smp.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../../kernel/limine.h"

// Maximum number of CPUs we support
#define MAX_CPUS 256

// Kernel stack size (assuming this is defined elsewhere)
#ifndef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE (16 * 1024) // 16KB default
#endif

// Include GDT definitions
#include "gdt.h"

// Per-CPU data structure
typedef struct {
    uint32_t lapic_id;
    uint32_t cpu_id;        // Linear CPU ID (0, 1, 2, ...)
    bool active;
    uint64_t kernel_stack;
    // Add more per-CPU data as needed
} cpu_info_t;

// Per-CPU local data (stored in GS segment)
// This structure now includes the GDT and TSS for each CPU
typedef struct {
    uint32_t cpu_id;        // Linear CPU ID (must be first field!)
    uint32_t lapic_id;
    // Per-CPU GDT and TSS
    struct gdt_entry gdt[7];  // 7 entries as per your GDT_ENTRIES
    struct tss tss;
    // Add more per-CPU data here in the future
} __attribute__((packed)) cpu_local_t;

// Global CPU information array
extern cpu_info_t g_cpu_info[MAX_CPUS];
extern uint32_t g_cpu_count;
extern uint32_t g_bsp_lapic_id;

// Global per-CPU local data array
extern cpu_local_t g_cpu_locals[MAX_CPUS];

// The physical address of the kernel's PML4, for the AP trampoline
extern uint64_t g_kernel_pml4;

// External variables from other modules
extern uint64_t g_hhdm_offset;

// Function declarations remain the same...
#if LIMINE_API_REVISION >= 1
void smp_init(volatile struct limine_mp_request *mp_request);
void ap_main(struct limine_mp_info *info);
#else
void smp_init(volatile struct limine_smp_request *smp_request);
void ap_main(struct limine_smp_info *info);
#endif

uint32_t smp_get_current_cpu(void);
uint32_t smp_get_current_cpu_id(void);
cpu_info_t* smp_get_cpu_info(uint32_t cpu_id);