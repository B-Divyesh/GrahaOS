// kernel/fs/simhash.h - SimHash & Byte Frequency Distribution for GrahaFS
// Phase 11a: Feature extraction for semantic auto-organizing filesystem
#pragma once

#include <stdint.h>
#include <stddef.h>

// SimHash configuration
#define SIMHASH_SHINGLE_SIZE 3   // 3-character sliding window for text
#define SIMHASH_BITS         64  // 64-bit hash fingerprint

// Hamming distance threshold for "similar" files
#define SIMHASH_SIMILAR_THRESHOLD 10  // default: <=10 differing bits = similar

// Compute 64-bit SimHash from text data using character k-gram shingles.
// Operates entirely in integer math. O(N) time, O(1) extra space.
uint64_t simhash_text(const void *data, size_t len);

// Compute 64-bit SimHash from binary data using byte frequency distribution.
// Builds a 256-entry histogram, then hashes each (byte_value, frequency) pair.
// O(N) time, 256 bytes stack space.
uint64_t simhash_bfd(const void *data, size_t len);

// Auto-detect text vs binary and compute appropriate SimHash.
// Heuristic: if >20% of bytes are non-printable (outside 0x09-0x7E), treat as binary.
uint64_t simhash_auto(const void *data, size_t len);

// Compute Hamming distance between two SimHash values.
// Uses XOR + popcount. O(1) constant time on x86_64.
int simhash_hamming_distance(uint64_t a, uint64_t b);

// 64-bit FNV-1a hash (used internally for shingle hashing)
uint64_t fnv1a_hash64(const void *data, size_t len);
