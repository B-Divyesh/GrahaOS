// kernel/fs/vfs.c
#include "vfs.h"
#include "../initrd.h"
#include <stddef.h> // For NULL

// The system-wide open file table
static open_file_t open_file_table[MAX_OPEN_FILES];

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
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        open_file_table[i].in_use = false;
    }
}

int vfs_open(const char *pathname) {
    size_t file_size;
    void *file_data = initrd_lookup(pathname, &file_size);

    if (file_data == NULL) {
        return -1; // File not found
    }

    // Find a free file descriptor
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!open_file_table[fd].in_use) {
            open_file_table[fd].in_use = true;
            open_file_table[fd].file_data = file_data;
            open_file_table[fd].size = file_size;
            open_file_table[fd].offset = 0;
            return fd;
        }
    }

    return -1; // No free file descriptors
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    // Validate file descriptor
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        return -1;
    }

    open_file_t *file = &open_file_table[fd];

    // Check for end-of-file
    if (file->offset >= file->size) {
        return 0; // EOF
    }

    // Determine how many bytes to read
    size_t bytes_to_read = count;
    if (file->offset + count > file->size) {
        bytes_to_read = file->size - file->offset;
    }

    // Copy data from initrd to user buffer
    // NOTE: This assumes the user `buffer` pointer is a valid, writable virtual address.
    vfs_memcpy(buffer, (uint8_t *)file->file_data + file->offset, bytes_to_read);

    // Update the offset
    file->offset += bytes_to_read;

    return bytes_to_read;
}

int vfs_close(int fd) {
    // Validate file descriptor
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        return -1;
    }

    // Mark the descriptor as free
    open_file_table[fd].in_use = false;
    return 0;
}