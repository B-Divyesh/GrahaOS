// kernel/fs/vfs.h
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../sync/spinlock.h"

#define MAX_OPEN_FILES 64
#define MAX_BLOCK_DEVICES 8 // NEW: Maximum number of block devices

typedef long ssize_t;

// Represents an open file in the system
typedef struct {
    bool in_use;
    void *file_data;
    size_t size;
    size_t offset;
} open_file_t;

// NEW: Represents a block device in the system
typedef struct {
    bool in_use;
    int device_id;
    size_t block_size;
    // Function pointers for read/write operations
    int (*read_blocks)(int dev_id, uint64_t lba, uint16_t count, void* buf);
    int (*write_blocks)(int dev_id, uint64_t lba, uint16_t count, void* buf);
} block_device_t;

void vfs_init(void);
int vfs_open(const char *pathname);
ssize_t vfs_read(int fd, void *buffer, size_t count);
int vfs_close(int fd);

// NEW: Function to register a block device
void vfs_register_block_device(int dev_id, size_t block_size, 
                               int (*read_func)(int, uint64_t, uint16_t, void*),
                               int (*write_func)(int, uint64_t, uint16_t, void*));

// NEW: Get a block device by ID
block_device_t* vfs_get_block_device(int dev_id);