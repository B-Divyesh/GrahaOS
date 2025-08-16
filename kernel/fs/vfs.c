// kernel/fs/vfs.c
#include "vfs.h"
#include "../initrd.h"
#include <stddef.h>
#include "../sync/spinlock.h"

// The system-wide open file table
static open_file_t open_file_table[MAX_OPEN_FILES];
// NEW: The system-wide block device table
static block_device_t block_device_table[MAX_BLOCK_DEVICES];

spinlock_t vfs_lock = SPINLOCK_INITIALIZER("vfs");

// Simple memcpy and strlen for internal use
static void *vfs_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }
    return dest;
}

static size_t vfs_strlen(const char *str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

void vfs_init(void) {
    spinlock_init(&vfs_lock, "vfs");
    
    spinlock_acquire(&vfs_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        open_file_table[i].in_use = false;
    }
    // NEW: Initialize block device table
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        block_device_table[i].in_use = false;
    }
    spinlock_release(&vfs_lock);
}

// NEW: Implementation for registering a block device
void vfs_register_block_device(int dev_id, size_t block_size, 
                               int (*read_func)(int, uint64_t, uint16_t, void*),
                               int (*write_func)(int, uint64_t, uint16_t, void*)) {
    if (dev_id < 0 || dev_id >= MAX_BLOCK_DEVICES) {
        return;
    }
    spinlock_acquire(&vfs_lock);
    block_device_table[dev_id].in_use = true;
    block_device_table[dev_id].device_id = dev_id;
    block_device_table[dev_id].block_size = block_size;
    block_device_table[dev_id].read_blocks = read_func;
    block_device_table[dev_id].write_blocks = write_func;
    spinlock_release(&vfs_lock);
}

// NEW: Get a block device by ID
block_device_t* vfs_get_block_device(int dev_id) {
    if (dev_id < 0 || dev_id >= MAX_BLOCK_DEVICES) {
        return NULL;
    }
    spinlock_acquire(&vfs_lock);
    block_device_t* dev = NULL;
    if (block_device_table[dev_id].in_use) {
        dev = &block_device_table[dev_id];
    }
    spinlock_release(&vfs_lock);
    return dev;
}

int vfs_open(const char *pathname) {
    size_t file_size;
    void *file_data = initrd_lookup(pathname, &file_size);

    if (file_data == NULL) {
        return -1; // File not found
    }
    
    spinlock_acquire(&vfs_lock);
    // Find a free file descriptor
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!open_file_table[fd].in_use) {
            open_file_table[fd].in_use = true;
            open_file_table[fd].file_data = file_data;
            open_file_table[fd].size = file_size;
            open_file_table[fd].offset = 0;
            spinlock_release(&vfs_lock);
            return fd;
        }
    }
    spinlock_release(&vfs_lock);
    return -1; // No free file descriptors
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }

    open_file_t *file = &open_file_table[fd];
    if (file->offset >= file->size) {
        spinlock_release(&vfs_lock);
        return 0; // EOF
    }

    size_t bytes_to_read = count;
    if (file->offset + count > file->size) {
        bytes_to_read = file->size - file->offset;
    }

    vfs_memcpy(buffer, (uint8_t *)file->file_data + file->offset, bytes_to_read);
    file->offset += bytes_to_read;
    
    spinlock_release(&vfs_lock);
    return bytes_to_read;
}

int vfs_close(int fd) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }
    open_file_table[fd].in_use = false;
    spinlock_release(&vfs_lock);
    return 0;
}