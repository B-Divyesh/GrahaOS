// kernel/initrd.c
// Initial RAM disk (initrd) implementation
// Supports POSIX TAR format archives

#include "initrd.h"
#include <stddef.h>
#include <stdint.h>

// POSIX TAR header structure (512 bytes)
typedef struct {
    char filename[100];     // File name
    char mode[8];          // File mode (octal)
    char uid[8];           // Owner user ID (octal)
    char gid[8];           // Owner group ID (octal)
    char size[12];         // File size (octal)
    char mtime[12];        // Last modification time (octal)
    char checksum[8];      // Header checksum (octal)
    char typeflag;         // File type flag
    char linkname[100];    // Link name
    char magic[6];         // Magic number ("ustar")
    char version[2];       // Version
    char uname[32];        // Owner user name
    char gname[32];        // Owner group name
    char devmajor[8];      // Device major number
    char devminor[8];      // Device minor number
    char prefix[155];      // Path prefix
    char pad[12];          // Padding to 512 bytes
} __attribute__((packed)) tar_header_t;

// Global pointer to the initrd data
static void *initrd_ptr = NULL;

/**
 * Simple string comparison function
 */
static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

/**
 * Convert octal string to unsigned long
 */
static unsigned long oct2ulong(const char *oct) {
    unsigned long val = 0;
    // FIX: Correctly dereference the pointer in the loop condition
    while (*oct >= '0' && *oct <= '7') {
        val = (val * 8) + (*oct++ - '0');
    }
    return val;
}

/**
 * Initialize the initrd subsystem
 */
void initrd_init(volatile struct limine_module_request *module_request) {
    if (module_request->response && module_request->response->module_count > 0) {
        // Use the first module as our initrd
        initrd_ptr = (void*)module_request->response->modules[0]->address;
    }
}

/**
 * Look up a file in the initrd TAR archive
 */
void *initrd_lookup(const char *filename, size_t *size) {
    if (!initrd_ptr) {
        return NULL;
    }

    tar_header_t *current_header = (tar_header_t *)initrd_ptr;

    // Iterate through TAR entries
    while (current_header->filename[0] != '\0') {
        // Check if this is the file we're looking for
        if (strcmp(current_header->filename, filename) == 0) {
            *size = oct2ulong(current_header->size);
            // File data starts immediately after the 512-byte header
            return (void *)((uintptr_t)current_header + 512);
        }

        // Calculate the next header position
        // TAR files are aligned to 512-byte boundaries
        size_t file_size = oct2ulong(current_header->size);
        uintptr_t next_header_addr = (uintptr_t)current_header + 512;
        
        // Round up file size to next 512-byte boundary
        if (file_size > 0) {
            next_header_addr += ((file_size + 511) & ~511);
        }
        
        current_header = (tar_header_t *)next_header_addr;
    }

    // File not found
    return NULL;
}