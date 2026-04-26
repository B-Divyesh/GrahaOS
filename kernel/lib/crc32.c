// kernel/lib/crc32.c
//
// Phase 19 — CRC32 helper. See header for contract.

#include "crc32.h"

#include <stdbool.h>

static uint32_t g_crc32_table[256];
static bool     g_crc32_table_init = false;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc32_table[i] = c;
    }
    g_crc32_table_init = true;
}

uint32_t crc32_update(uint32_t running, const void *data, size_t len) {
    if (!g_crc32_table_init) crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = running;
    for (size_t i = 0; i < len; ++i) {
        c = g_crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

uint32_t crc32_buf(const void *data, size_t len) {
    uint32_t c = crc32_init();
    c = crc32_update(c, data, len);
    return crc32_final(c);
}
