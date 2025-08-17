// kernel/fs/grahafs.h
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GRAHAFS_MAGIC 0x47524148414F5321 // "GRAHAOS!"
#define GRAHAFS_BLOCK_SIZE 4096
#define GRAHAFS_MAX_INODES 4096
#define GRAHAFS_MAX_FILENAME 28
#define GRAHAFS_MAX_PATH 256

#define GRAHAFS_INODE_TYPE_FILE 1
#define GRAHAFS_INODE_TYPE_DIRECTORY 2

typedef long ssize_t;

// On-disk superblock structure
typedef struct {
    uint64_t magic;
    uint32_t total_blocks;
    uint32_t bitmap_start_block;
    uint32_t inode_table_start_block;
    uint32_t data_blocks_start_block;
    uint32_t root_inode;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint8_t  padding[4036];  // Pad to 4096 bytes
} __attribute__((packed)) grahafs_superblock_t;

// On-disk inode structure
typedef struct {
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
    uint8_t  padding[24];         // Pad to 128 bytes (FIXED: was 32, should be 24)
} __attribute__((packed)) grahafs_inode_t;

// On-disk directory entry structure
typedef struct {
    uint32_t inode_num;
    char name[GRAHAFS_MAX_FILENAME];
} __attribute__((packed)) grahafs_dirent_t;

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