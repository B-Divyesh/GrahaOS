// kernel/fs/simhash.c - SimHash & BFD implementation
// Phase 11a: Feature extraction for semantic auto-organizing filesystem
//
// All integer math, no FPU. O(N) per file, O(1) storage.

#include "simhash.h"
#include <stdint.h>

// FNV-1a constants for 64-bit
#define FNV1A_OFFSET_BASIS 0xCBF29CE484222325ULL
#define FNV1A_PRIME        0x00000100000001B3ULL

uint64_t fnv1a_hash64(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

// Software popcount via parallel bit counting (no SSE/POPCNT dependency)
static inline int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

uint64_t simhash_text(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;

    if (len < SIMHASH_SHINGLE_SIZE) {
        // File too small for shingles — just hash the whole thing
        return fnv1a_hash64(data, len);
    }

    // Accumulator: 64 counters, one per bit position.
    // Positive = more 1s, negative = more 0s.
    // Use int32_t to avoid overflow for files up to ~2GB.
    int32_t v[64];
    for (int i = 0; i < 64; i++) v[i] = 0;

    // Slide a window of SIMHASH_SHINGLE_SIZE chars across the text
    size_t num_shingles = len - SIMHASH_SHINGLE_SIZE + 1;
    for (size_t s = 0; s < num_shingles; s++) {
        uint64_t h = fnv1a_hash64(&bytes[s], SIMHASH_SHINGLE_SIZE);

        // For each bit in the hash, increment or decrement the counter
        for (int bit = 0; bit < 64; bit++) {
            if (h & (1ULL << bit))
                v[bit]++;
            else
                v[bit]--;
        }
    }

    // Build final SimHash: bit is 1 if counter is positive, 0 otherwise
    uint64_t simhash = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (v[bit] > 0)
            simhash |= (1ULL << bit);
    }

    return simhash;
}

uint64_t simhash_bfd(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;

    // Build byte frequency histogram (256 entries)
    uint32_t freq[256];
    for (int i = 0; i < 256; i++) freq[i] = 0;
    for (size_t i = 0; i < len; i++) freq[bytes[i]]++;

    // SimHash from frequency distribution
    // Each non-zero frequency becomes a feature: hash(byte_value, frequency)
    int32_t v[64];
    for (int i = 0; i < 64; i++) v[i] = 0;

    for (int b = 0; b < 256; b++) {
        if (freq[b] == 0) continue;

        // Create a feature from (byte_value, frequency)
        uint8_t feature[6];
        feature[0] = (uint8_t)b;
        feature[1] = (uint8_t)(b >> 4);  // some bit mixing
        feature[2] = (uint8_t)(freq[b] & 0xFF);
        feature[3] = (uint8_t)((freq[b] >> 8) & 0xFF);
        feature[4] = (uint8_t)((freq[b] >> 16) & 0xFF);
        feature[5] = (uint8_t)((freq[b] >> 24) & 0xFF);

        uint64_t h = fnv1a_hash64(feature, 6);

        // Weight by frequency (capped to prevent overflow)
        int32_t weight = (int32_t)(freq[b] > 1000 ? 1000 : freq[b]);

        for (int bit = 0; bit < 64; bit++) {
            if (h & (1ULL << bit))
                v[bit] += weight;
            else
                v[bit] -= weight;
        }
    }

    uint64_t simhash = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (v[bit] > 0)
            simhash |= (1ULL << bit);
    }

    return simhash;
}

uint64_t simhash_auto(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;

    if (len == 0) return 0;

    // Heuristic: count non-printable bytes (outside 0x09-0x7E)
    // Sample first 1024 bytes to keep it fast
    size_t sample = len > 1024 ? 1024 : len;
    size_t non_printable = 0;
    for (size_t i = 0; i < sample; i++) {
        uint8_t b = bytes[i];
        if (b < 0x09 || (b > 0x0D && b < 0x20) || b > 0x7E)
            non_printable++;
    }

    // If >20% non-printable, treat as binary
    if (non_printable * 5 > sample) {
        return simhash_bfd(data, len);
    } else {
        return simhash_text(data, len);
    }
}

int simhash_hamming_distance(uint64_t a, uint64_t b) {
    return popcount64(a ^ b);
}
