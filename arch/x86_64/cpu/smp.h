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

// Include GDT definitions (needed by percpu.h).
#include "gdt.h"

// Phase 14: per-CPU data structure is now defined in kernel/percpu.h as
// percpu_t, with the Phase 7a cpu_local_t layout preserved as its prefix
// (cpu_id at offset 0, tss.rsp0 at gs:68, syscall_scratch at gs:168).
// A typedef alias `cpu_local_t` is kept so existing callers compile.
#include "../../../kernel/percpu.h"

// Per-CPU system information (separate from percpu_t; used by the SMP
// bring-up code, not hot paths).
typedef struct {
    uint32_t lapic_id;
    uint32_t cpu_id;        // Linear CPU ID (0, 1, 2, ...)
    bool active;
    uint64_t kernel_stack;
    // Add more per-CPU data as needed
} cpu_info_t;

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

extern volatile uint32_t aps_started;

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