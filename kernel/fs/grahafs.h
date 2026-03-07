// kernel/fs/grahafs.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GRAHAFS_MAGIC 0x47524148414F5321 // "GRAHAOS!"
#define GRAHAFS_VERSION 1
#define GRAHAFS_BLOCK_SIZE 4096
#define GRAHAFS_MAX_INODES 4096
#define GRAHAFS_MAX_FILENAME 28
#define GRAHAFS_MAX_PATH 256

#define GRAHAFS_INODE_TYPE_FILE 1
#define GRAHAFS_INODE_TYPE_DIRECTORY 2

// AI metadata constants
#define GRAHAFS_AI_META_MAGIC 0x47524148414D4554  // "GRAHAMET"

// Inode ai_flags bits (on-disk, which extended fields are present)
#define GRAHAFS_AI_HAS_TAGS      0x01
#define GRAHAFS_AI_HAS_SUMMARY   0x02
#define GRAHAFS_AI_HAS_EMBEDDING 0x04
#define GRAHAFS_AI_HAS_EXTENDED  0x08

// User-space metadata flags (which fields to set/get)
#define GRAHAFS_META_FLAG_TAGS       0x01
#define GRAHAFS_META_FLAG_SUMMARY    0x02
#define GRAHAFS_META_FLAG_EMBEDDING  0x04
#define GRAHAFS_META_FLAG_IMPORTANCE 0x08

typedef long ssize_t;

// On-disk superblock structure
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t bitmap_start_block;
    uint32_t inode_table_start_block;
    uint32_t data_blocks_start_block;
    uint32_t root_inode;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint8_t  padding[4056];  // Pad to 4096 bytes (40 bytes of fields + 4056 = 4096)
} __attribute__((packed)) grahafs_superblock_t;

// On-disk inode structure (256 bytes)
typedef struct {
    // --- Core fields (104 bytes) ---
    uint16_t type;
    uint16_t link_count;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t size;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint32_t direct_blocks[12];   // Pointers to 12 direct data blocks
    uint32_t indirect_block;      // Pointer to indirect block
    uint32_t double_indirect;     // Pointer to double indirect block
    // --- AI Metadata (152 bytes) ---
    uint32_t ai_flags;            // HAS_TAGS, HAS_SUMMARY, HAS_EMBEDDING, HAS_EXTENDED
    uint32_t ai_importance;       // 0-100 score
    uint32_t ai_metadata_block;   // Block number for extended metadata (0 = none)
    uint32_t ai_access_count;     // Access frequency counter
    uint64_t ai_last_modified;    // Timestamp of last AI metadata change
    char     ai_tags[96];         // Inline tags (NUL-terminated, comma-separated)
    uint8_t  ai_reserved[32];     // Reserved for future use
} __attribute__((packed)) grahafs_inode_t;

// On-disk directory entry structure
typedef struct {
    uint32_t inode_num;
    char name[GRAHAFS_MAX_FILENAME];
} __attribute__((packed)) grahafs_dirent_t;

// Extended AI metadata block (4096 bytes, allocated on demand)
typedef struct {
    uint64_t magic;              // GRAHAFS_AI_META_MAGIC
    uint32_t version;            // Format version (1)
    uint32_t embedding_dim;      // Actual dimensions used (0-128)
    char     tags[512];          // Full tags string
    char     summary[1024];      // Human-readable summary
    uint64_t embedding[128];     // 128-dimension vector (1024 bytes)
    uint8_t  reserved[1520];     // Pad to 4096 bytes
} __attribute__((packed)) grahafs_ai_metadata_block_t;

// User-space metadata struct (for set/get syscalls)
typedef struct {
    uint32_t flags;              // Which fields are valid/being set
    uint32_t importance;         // 0-100
    uint32_t access_count;       // Read-only on get
    uint32_t _pad0;
    uint64_t last_modified;      // Read-only on get
    char     tags[512];          // Tags string
    char     summary[1024];      // Summary string
    uint64_t embedding[128];     // Vector (1024 bytes)
    uint32_t embedding_dim;      // Dimensions used
    uint32_t _pad1;
} __attribute__((packed)) grahafs_ai_metadata_t;

// Search result entry
typedef struct {
    char     path[256];
    uint32_t inode_num;
    uint32_t importance;
    char     tags[96];
} __attribute__((packed)) grahafs_search_result_t;

// Search results container
typedef struct {
    uint32_t count;
    uint32_t _pad;
    grahafs_search_result_t results[16];
} __attribute__((packed)) grahafs_search_results_t;

// Forward declaration for VFS integration
struct vfs_node;
struct block_device;

/**
 * @brief Initializes the GrahaFS filesystem driver.
 */
void grahafs_init(void);

/**
 * @brief Mounts a GrahaFS filesystem from a block device.
 * @param device The block device to mount.
 * @return A pointer to the root VFS node on success, NULL on failure.
 */
struct vfs_node* grahafs_mount(struct block_device* device);

/**
 * @brief Unmounts a GrahaFS filesystem.
 * @param root The root node of the filesystem to unmount.
 * @return 0 on success, -1 on failure.
 */
int grahafs_unmount(struct vfs_node* root);

// Filesystem operations for VFS integration
ssize_t grahafs_read(struct vfs_node* node, uint64_t offset, size_t size, void* buffer);
ssize_t grahafs_write(struct vfs_node* node, uint64_t offset, size_t size, void* buffer);
struct vfs_node* grahafs_open(struct vfs_node* node, const char* name);
void grahafs_close(struct vfs_node* node);
struct vfs_node* grahafs_readdir(struct vfs_node* node, uint32_t index);
struct vfs_node* grahafs_finddir(struct vfs_node* node, const char* name);

// Phase 8a: Get GrahaFS statistics for system state reporting
void grahafs_get_stats(uint32_t *mounted, uint32_t *total_blocks,
                       uint32_t *free_blocks, uint32_t *free_inodes);

// Phase 8c: AI metadata operations
int grahafs_set_ai_metadata(uint32_t inode_num, const grahafs_ai_metadata_t *meta);
int grahafs_get_ai_metadata(uint32_t inode_num, grahafs_ai_metadata_t *meta);
int grahafs_search_by_tag(const char *tag, grahafs_search_results_t *results, int max_results);

// Phase 11a: SimHash feature extraction
uint64_t grahafs_compute_simhash(uint32_t inode_num);
int grahafs_find_similar(uint32_t ref_inode, int threshold,
                         grahafs_search_results_t *results, int max_results);

// Phase 11b: Background indexer task (auto-indexes files without SimHash)
void grahafs_indexer_task(void);
