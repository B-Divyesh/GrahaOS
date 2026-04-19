// kernel/state.c
// Phase 8a: System state collection and snapshot module
#include "state.h"
#include "cap/can.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/smp.h"
#include "fs/vfs.h"
#include "fs/grahafs.h"

// Forward declaration - memset not available in kernel
static void *state_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

void state_collect_memory(state_memory_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    uint64_t total = pmm_get_total_memory();
    uint64_t free = pmm_get_free_memory();

    out->total_physical = total;
    out->free_physical = free;
    out->used_physical = total - free;
    out->page_size = PAGE_SIZE;
    out->total_pages = total / PAGE_SIZE;
    out->used_pages = (total - free) / PAGE_SIZE;
}

void state_collect_processes(state_process_list_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    out->count = sched_snapshot_processes(out->procs, STATE_MAX_TASKS);
}

void state_collect_filesystem(state_filesystem_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    uint32_t of = 0, bd = 0, mf = 0;
    vfs_get_stats(&of, &bd, &mf);
    out->open_files = of;
    out->max_open_files = 64;   // MAX_OPEN_FILES from vfs.c
    out->block_devices = bd;
    out->max_block_devices = 8; // MAX_BLOCK_DEVICES from vfs.c
    out->mounted_fs = mf;
    out->max_filesystems = 4;   // MAX_FILESYSTEMS from vfs.c

    uint32_t gm = 0, gtb = 0, gfb = 0, gfi = 0;
    grahafs_get_stats(&gm, &gtb, &gfb, &gfi);
    out->grahafs_mounted = gm;
    out->grahafs_total_blocks = gtb;
    out->grahafs_free_blocks = gfb;
    out->grahafs_free_inodes = gfi;
    out->grahafs_max_inodes = 4096;  // GRAHAFS_MAX_INODES
    out->grahafs_block_size = 4096;  // GRAHAFS_BLOCK_SIZE
}

extern volatile uint64_t g_timer_ticks;

void state_collect_system(state_system_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    out->cpu_count = g_cpu_count;
    out->bsp_lapic_id = g_bsp_lapic_id;
    out->schedule_count = schedule_count;
    out->context_switches = context_switches;
    // Phase 12: expose LAPIC 100 Hz tick count (divide by 100 for seconds).
    out->uptime_ticks = g_timer_ticks;

    uint32_t entries = g_cpu_count;
    if (entries > STATE_MAX_CPUS) entries = STATE_MAX_CPUS;
    out->cpu_entries = entries;

    for (uint32_t i = 0; i < entries; i++) {
        out->cpus[i].lapic_id = g_cpu_info[i].lapic_id;
        out->cpus[i].active = g_cpu_info[i].active ? 1 : 0;
    }
}

void state_collect_drivers(state_driver_list_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    out->count = cap_snapshot_drivers(out->drivers, STATE_MAX_DRIVERS);
}

void state_collect_capabilities(state_cap_list_t *out) {
    if (!out) return;
    state_memset(out, 0, sizeof(*out));

    out->count = cap_query_all(out->caps, STATE_MAX_CAPS);
}

int state_collect_all(state_snapshot_t *out) {
    if (!out) return -1;
    state_memset(out, 0, sizeof(*out));

    out->version = 2;  // Phase 8b v2

    state_collect_memory(&out->memory);
    state_collect_processes(&out->processes);
    state_collect_filesystem(&out->filesystem);
    state_collect_system(&out->system);
    state_collect_drivers(&out->drivers);
    state_collect_capabilities(&out->capabilities);

    return 0;
}

// Returns the size of data for a given category
int state_get_size(uint32_t category) {
    switch (category) {
        case STATE_CAT_MEMORY:     return sizeof(state_memory_t);
        case STATE_CAT_PROCESSES:  return sizeof(state_process_list_t);
        case STATE_CAT_FILESYSTEM: return sizeof(state_filesystem_t);
        case STATE_CAT_SYSTEM:     return sizeof(state_system_t);
        case STATE_CAT_DRIVERS:        return sizeof(state_driver_list_t);
        case STATE_CAT_CAPABILITIES:   return sizeof(state_cap_list_t);
        case STATE_CAT_ALL:            return sizeof(state_snapshot_t);
        default:                   return -1;
    }
}
