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

// Advance to the next TAR header after `h`.
static tar_header_t *tar_next(tar_header_t *h) {
    size_t fsz = oct2ulong(h->size);
    uintptr_t na = (uintptr_t)h + 512;
    if (fsz > 0) na += ((fsz + 511) & ~((size_t)511));
    return (tar_header_t *)na;
}

// If `fn` begins with `prefix` (length pl), return the suffix pointer; else NULL.
static const char *match_prefix(const char *fn, const char *prefix, size_t pl) {
    for (size_t i = 0; i < pl; i++) {
        if (fn[i] != prefix[i]) return NULL;
    }
    return fn + pl;
}

// Copy the first '/'-delimited component of `rest` into `comp`; set *is_dir if
// a '/' follows. Returns the component length.
static size_t first_component(const char *rest, char *comp, size_t cap,
                              uint32_t *is_dir) {
    size_t cl = 0;
    while (rest[cl] && rest[cl] != '/' && cl < cap - 1) { comp[cl] = rest[cl]; cl++; }
    comp[cl] = '\0';
    if (is_dir) *is_dir = (rest[cl] == '/') ? 1u : 0u;
    return cl;
}

int initrd_readdir(const char *dirpath, uint32_t index,
                   char *name_out, size_t cap, uint32_t *is_dir_out) {
    if (!initrd_ptr || !name_out || cap == 0) return -1;

    // Normalize dirpath -> prefix: "" for the top level, "bin/" for "/bin".
    char prefix[128];
    size_t pl = 0;
    const char *p = dirpath ? dirpath : "";
    if (*p == '/') p++;                                  // strip leading '/'
    while (*p && pl < sizeof(prefix) - 2) prefix[pl++] = *p++;
    if (pl > 0 && prefix[pl - 1] != '/') prefix[pl++] = '/';  // ensure trailing '/'
    prefix[pl] = '\0';

    uint32_t distinct = 0;
    for (tar_header_t *h = (tar_header_t *)initrd_ptr;
         h->filename[0] != '\0'; h = tar_next(h)) {
        const char *rest = match_prefix(h->filename, prefix, pl);
        if (!rest || *rest == '\0') continue;           // not under prefix / dir itself

        char comp[100]; uint32_t is_dir = 0;
        size_t cl = first_component(rest, comp, sizeof(comp), &is_dir);
        if (cl == 0) continue;
        if (comp[0] == '.' && (cl == 1 || (cl == 2 && comp[1] == '.'))) continue;  // . / ..

        // Dedup: skip if this component already appeared in an earlier entry.
        int seen = 0;
        for (tar_header_t *g = (tar_header_t *)initrd_ptr; g != h; g = tar_next(g)) {
            const char *grest = match_prefix(g->filename, prefix, pl);
            if (!grest || *grest == '\0') continue;
            char gc[100]; size_t gcl = first_component(grest, gc, sizeof(gc), NULL);
            if (gcl != cl) continue;
            int eq = 1;
            for (size_t i = 0; i < cl; i++) if (gc[i] != comp[i]) { eq = 0; break; }
            if (eq) { seen = 1; break; }
        }
        if (seen) continue;

        if (distinct == index) {
            size_t o = 0;
            for (; comp[o] && o < cap - 1; o++) name_out[o] = comp[o];
            name_out[o] = '\0';
            if (is_dir_out) *is_dir_out = is_dir;
            return 0;
        }
        distinct++;
    }
    return -1;
}