// kernel/initrd.h
// Initial RAM disk (initrd) support for GrahaOS
// Provides functionality to parse TAR archives loaded by Limine

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "limine.h"

/**
 * Initialize the initrd subsystem
 * @param module_request Pointer to the Limine module request structure
 */
void initrd_init(volatile struct limine_module_request *module_request);

/**
 * Look up a file in the initrd
 * @param filename Name of the file to look up (e.g., "bin/grahai")
 * @param size Pointer to store the file size
 * @return Pointer to file data, or NULL if not found
 */
void *initrd_lookup(const char *filename, size_t *size);

/**
 * Enumerate the immediate children of a directory in the initrd TAR.
 * The initrd is a flat TAR ("bin/gsh", "bin/tests/x.tap", "etc/gcp.json", ...);
 * this returns the distinct first-level names under `dirpath`.
 *
 * @param dirpath  Directory path, with or without a leading '/'. "/" or ""
 *                 enumerates the top level (e.g. "bin", "etc"); "/bin"
 *                 enumerates the binaries directly under bin/.
 * @param index    Zero-based index of the distinct child to return.
 * @param name_out Buffer for the child's basename (NUL-terminated).
 * @param cap      Capacity of name_out.
 * @param is_dir_out  Set to 1 if the child is itself a directory (has further
 *                 path components in the TAR), else 0. May be NULL.
 * @return 0 on success, -1 if `index` is past the last distinct child.
 */
int initrd_readdir(const char *dirpath, uint32_t index,
                   char *name_out, size_t cap, uint32_t *is_dir_out);