// kernel/ipc/manifest.c — Phase 17.
#include "manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fs/simhash.h"    // fnv1a_hash64()
#include "log.h"

#define MANIFEST_SLOTS 7

static uint64_t g_hashes[MANIFEST_SLOTS];
static bool     g_manifest_ready = false;

uint64_t manifest_compute_hash(const char *type_name) {
    if (!type_name) return 0;
    return fnv1a_hash64(type_name, strlen(type_name));
}

void manifest_init(void) {
    g_hashes[0] = manifest_compute_hash(MANIFEST_NAME_NOTIFY_V1);
    g_hashes[1] = manifest_compute_hash(MANIFEST_NAME_PIPE_BYTES_V1);
    g_hashes[2] = manifest_compute_hash(MANIFEST_NAME_FAULT_V1);
    g_hashes[3] = manifest_compute_hash(MANIFEST_NAME_SHUTDOWN_V1);
    g_hashes[4] = manifest_compute_hash(MANIFEST_NAME_AUDIT_V1);
    g_hashes[5] = manifest_compute_hash(MANIFEST_NAME_TEST_V1);
    g_hashes[6] = manifest_compute_hash(MANIFEST_NAME_IO_COMPLETION_V1);
    g_manifest_ready = true;
    // Detect collisions: any two equal entries means the hash space is
    // compromised at build time. 64-bit FNV-1a has astronomically low
    // collision probability for 6 inputs, but check anyway.
    for (int i = 0; i < MANIFEST_SLOTS; i++) {
        for (int j = i + 1; j < MANIFEST_SLOTS; j++) {
            if (g_hashes[i] == g_hashes[j]) {
                klog(KLOG_ERROR, SUBSYS_CAP, "manifest: hash collision at slots %d and %d", i, j);
            }
        }
    }
    klog(KLOG_INFO, SUBSYS_CAP, "manifest: %d type hashes registered", MANIFEST_SLOTS);
}

bool manifest_type_known(uint64_t type_hash) {
    if (!g_manifest_ready) return false;
    for (int i = 0; i < MANIFEST_SLOTS; i++) {
        if (g_hashes[i] == type_hash) return true;
    }
    return false;
}

uint64_t manifest_hash_notify_v1(void)     { return g_manifest_ready ? g_hashes[0] : 0; }
uint64_t manifest_hash_pipe_bytes_v1(void) { return g_manifest_ready ? g_hashes[1] : 0; }
uint64_t manifest_hash_fault_v1(void)      { return g_manifest_ready ? g_hashes[2] : 0; }
uint64_t manifest_hash_shutdown_v1(void)   { return g_manifest_ready ? g_hashes[3] : 0; }
uint64_t manifest_hash_audit_v1(void)      { return g_manifest_ready ? g_hashes[4] : 0; }
uint64_t manifest_hash_test_v1(void)       { return g_manifest_ready ? g_hashes[5] : 0; }
