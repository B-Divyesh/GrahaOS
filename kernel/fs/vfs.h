// kernel/fs/vfs.h
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../sync/spinlock.h"

#define MAX_OPEN_FILES 64

typedef long ssize_t;
// Represents an open file in the system
typedef struct {
    bool in_use;             // Is this file descriptor slot in use?
    void *file_data;         // Pointer to the file's data in the initrd
    size_t size;             // Total size of the file
    size_t offset;           // Current read offset
} open_file_t;

/**
 * @brief Initializes the Virtual File System.
 */
void vfs_init(void);

/**
 * @brief Opens a file and returns a file descriptor.
 * @param pathname The null-terminated path of the file to open.
 * @return A non-negative file descriptor on success, -1 on failure.
 */
int vfs_open(const char *pathname);

/**
 * @brief Reads from an open file.
 * @param fd The file descriptor to read from.
 * @param buffer A pointer to the user-space buffer to store data.
 * @param count The maximum number of bytes to read.
 * @return The number of bytes read, 0 on EOF, or -1 on error.
 */
ssize_t vfs_read(int fd, void *buffer, size_t count);

/**
 * @brief Closes an open file descriptor.
 * @param fd The file descriptor to close.
 * @return 0 on success, -1 on failure.
 */
int vfs_close(int fd);