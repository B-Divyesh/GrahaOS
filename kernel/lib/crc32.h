// kernel/lib/crc32.h
//
// Phase 19 — CRC32 helper for GrahaFS v2 on-disk integrity checks.
//
// Standard CRC32 with reflected polynomial 0xEDB88320 (aka CRC32-IEEE802.3).
// Test vector: crc32_buf("123456789", 9) == 0xCBF43926. Verified in the
// table init path at first use (runtime one-liner).
//
// Design:
//   * Table-based (1 KB table) for speed. Populated on first call.
//   * Iterative API (init/update/final) allows CRC over non-contiguous data
//     so a journal commit can fold begin + data[] + metadata[] into one
//     checksum without a staging buffer.
//   * Endianness-neutral: treats input as a byte stream.
#pragma once

#include <stddef.h>
#include <stdint.h>

// One-shot CRC32 over a contiguous buffer. For short blocks (inode,
// superblock, version record). Zero bytes hash to 0xFFFFFFFF xored with
// the final 0xFFFFFFFF = 0.
uint32_t crc32_buf(const void *data, size_t len);

// Iterative API for large or fragmented inputs (journal txn checksums).
//   uint32_t c = crc32_init();
//   c = crc32_update(c, begin_block, 4096);
//   for (int i = 0; i < N; ++i) c = crc32_update(c, data_blocks[i], 4096);
//   uint32_t final = crc32_final(c);
static inline uint32_t crc32_init(void) { return 0xFFFFFFFFu; }
uint32_t crc32_update(uint32_t running, const void *data, size_t len);
static inline uint32_t crc32_final(uint32_t running) { return running ^ 0xFFFFFFFFu; }
