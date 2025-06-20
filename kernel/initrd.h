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