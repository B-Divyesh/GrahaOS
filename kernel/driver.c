// kernel/driver.c
// Phase 8a: Driver registration framework implementation
#include "driver.h"
#include "../arch/x86_64/drivers/serial/serial.h"

// Static registry of driver descriptors
static driver_descriptor_t registry[MAX_REGISTERED_DRIVERS];
static int driver_count = 0;

// Simple string copy (kernel has no libc)
static void drv_strcpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int driver_register(const driver_descriptor_t *desc) {
    if (!desc || !desc->name) return -1;
    if (driver_count >= MAX_REGISTERED_DRIVERS) return -1;

    int id = driver_count;

    // Copy the descriptor into the registry
    registry[id].name = desc->name;
    registry[id].type = desc->type;
    registry[id].get_stats = desc->get_stats;
    registry[id].op_count = desc->op_count;
    if (registry[id].op_count > STATE_MAX_DRIVER_OPS) {
        registry[id].op_count = STATE_MAX_DRIVER_OPS;
    }

    for (int i = 0; i < registry[id].op_count; i++) {
        registry[id].ops[i] = desc->ops[i];
    }

    driver_count++;

    serial_write("[DRIVER] Registered: ");
    serial_write(desc->name);
    serial_write(" (type=");
    serial_write_dec(desc->type);
    serial_write(", ops=");
    serial_write_dec(desc->op_count);
    serial_write(")\n");

    return id;
}

int driver_get_count(void) {
    return driver_count;
}

int driver_snapshot_all(state_driver_info_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    int count = driver_count;
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        // Copy name
        drv_strcpy(out[i].name, registry[i].name, STATE_DRIVER_NAME_LEN);

        out[i].type = registry[i].type;
        out[i].initialized = 1;

        // Copy operation descriptors
        out[i].op_count = registry[i].op_count;
        for (int j = 0; j < (int)out[i].op_count && j < STATE_MAX_DRIVER_OPS; j++) {
            drv_strcpy(out[i].ops[j].name, registry[i].ops[j].name, STATE_OP_NAME_LEN);
            out[i].ops[j].param_count = registry[i].ops[j].param_count;
            out[i].ops[j].flags = registry[i].ops[j].flags;
        }

        // Call the driver's stats callback
        if (registry[i].get_stats) {
            out[i].stat_count = registry[i].get_stats(out[i].stats, STATE_MAX_DRIVER_STATS);
        } else {
            out[i].stat_count = 0;
        }
    }

    return count;
}
