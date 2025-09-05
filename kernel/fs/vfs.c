// kernel/fs/vfs.c - COMPLETE FILE
#include "vfs.h"
#include "../initrd.h"
#include <stddef.h>
#include "../sync/spinlock.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/video/framebuffer.h"
#include "../../arch/x86_64/drivers/ahci/ahci.h"

static open_file_t open_file_table[MAX_OPEN_FILES];
static block_device_t block_device_table[MAX_BLOCK_DEVICES];
static vfs_filesystem_t filesystem_table[MAX_FILESYSTEMS];
static vfs_node_t* vfs_root = NULL;
spinlock_t vfs_lock = SPINLOCK_INITIALIZER("vfs");

// Memory utilities
static void *vfs_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) pdest[i] = psrc[i];
    return dest;
}

static void *vfs_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static size_t vfs_strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static int vfs_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static char* vfs_strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++));
    return ret;
}

static char* vfs_strdup(const char *src) {
    size_t len = vfs_strlen(src) + 1;
    char *dst = (char*)((uint64_t)pmm_alloc_page() + g_hhdm_offset);
    if (dst) vfs_memcpy(dst, src, len);
    return dst;
}

// Parse path component by component
static char* get_next_path_component(char** path) {
    if (!*path || **path == '\0') return NULL;
    
    // Skip leading slashes
    while (**path == '/') (*path)++;
    
    if (**path == '\0') return NULL;
    
    char* start = *path;
    
    // Find next slash or end
    while (**path && **path != '/') (*path)++;
    
    // Temporarily terminate the component
    if (**path == '/') {
        **path = '\0';
        (*path)++;
    }
    
    return start;
}

// Path traversal with proper error handling
vfs_node_t* vfs_path_to_node(const char* path) {
    if (!path || !vfs_root) return NULL;
    
    // Handle root directory
    if (vfs_strcmp(path, "/") == 0) {
        vfs_root->refcount++;
        return vfs_root;
    }
    
    // Make a copy of the path for parsing
    char* path_copy = vfs_strdup(path);
    if (!path_copy) return NULL;
    
    char* p = path_copy;
    vfs_node_t* current = vfs_root;
    current->refcount++;
    
    char* component;
    while ((component = get_next_path_component(&p)) != NULL) {
        // Skip empty components (from //)
        if (vfs_strlen(component) == 0) continue;
        
        // Handle special directories
        if (vfs_strcmp(component, ".") == 0) {
            continue; // Stay in current directory
        }
        
        if (vfs_strcmp(component, "..") == 0) {
            // Go up to parent (if we have one)
            if (current->parent) {
                vfs_node_t* parent = current->parent;
                parent->refcount++;
                vfs_destroy_node(current);
                current = parent;
            }
            continue;
        }
        
        // Look for the component in current directory
        if (current->type != VFS_DIRECTORY || !current->finddir) {
            vfs_destroy_node(current);
            pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
            return NULL;
        }
        
        vfs_node_t* next = current->finddir(current, component);
        if (!next) {
            vfs_destroy_node(current);
            pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
            return NULL;
        }
        
        next->parent = current;
        vfs_destroy_node(current);
        current = next;
    }
    
    pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
    return current;
}

void vfs_init(void) {
    spinlock_init(&vfs_lock, "vfs");
    
    spinlock_acquire(&vfs_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        open_file_table[i].in_use = false;
    }
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        block_device_table[i].in_use = false;
    }
    for (int i = 0; i < MAX_FILESYSTEMS; i++) {
        filesystem_table[i].mounted = false;
    }
    vfs_root = NULL;
    spinlock_release(&vfs_lock);
}

void vfs_register_block_device(int dev_id, size_t block_size, 
                               int (*read_func)(int, uint64_t, uint16_t, void*),
                               int (*write_func)(int, uint64_t, uint16_t, void*)) {
    if (dev_id < 0 || dev_id >= MAX_BLOCK_DEVICES) return;
    
    spinlock_acquire(&vfs_lock);
    block_device_table[dev_id].in_use = true;
    block_device_table[dev_id].device_id = dev_id;
    block_device_table[dev_id].block_size = block_size;
    block_device_table[dev_id].read_blocks = read_func;
    block_device_table[dev_id].write_blocks = write_func;
    spinlock_release(&vfs_lock);
}

block_device_t* vfs_get_block_device(int dev_id) {
    if (dev_id < 0 || dev_id >= MAX_BLOCK_DEVICES) return NULL;
    
    spinlock_acquire(&vfs_lock);
    block_device_t* dev = NULL;
    if (block_device_table[dev_id].in_use) {
        dev = &block_device_table[dev_id];
    }
    spinlock_release(&vfs_lock);
    return dev;
}

vfs_node_t* vfs_create_node(const char* name, uint32_t type) {
    void* node_mem = pmm_alloc_page();
    if (!node_mem) return NULL;
    
    vfs_node_t* node = (vfs_node_t*)((uint64_t)node_mem + g_hhdm_offset);
    vfs_memset(node, 0, sizeof(vfs_node_t));
    
    if (name) {
        size_t len = vfs_strlen(name);
        if (len >= VFS_MAX_NAME) len = VFS_MAX_NAME - 1;
        vfs_memcpy(node->name, name, len);
        node->name[len] = '\0';
    }
    
    node->type = type;
    node->refcount = 1;
    
    return node;
}

void vfs_destroy_node(vfs_node_t* node) {
    if (!node) return;
    
    if (--node->refcount == 0) {
        if (node->close) {
            node->close(node);
        }
        
        uint64_t phys = (uint64_t)node - g_hhdm_offset;
        pmm_free_page((void*)phys);
    }
}

vfs_node_t* vfs_get_root(void) {
    return vfs_root;
}

void vfs_set_root(vfs_node_t* root) {
    vfs_root = root;
}

int vfs_open(const char *pathname) {
    spinlock_acquire(&vfs_lock);
    
    vfs_node_t* node = vfs_path_to_node(pathname);
    if (!node) {
        // Try initrd as fallback
        size_t file_size;
        void *file_data = initrd_lookup(pathname, &file_size);
        
        if (file_data == NULL) {
            spinlock_release(&vfs_lock);
            return -1;
        }
        
        for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
            if (!open_file_table[fd].in_use) {
                open_file_table[fd].in_use = true;
                open_file_table[fd].file_data = file_data;
                open_file_table[fd].size = file_size;
                open_file_table[fd].offset = 0;
                open_file_table[fd].node = NULL;
                spinlock_release(&vfs_lock);
                return fd;
            }
        }
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    // Find free file descriptor
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!open_file_table[fd].in_use) {
            open_file_table[fd].in_use = true;
            open_file_table[fd].node = node;
            open_file_table[fd].offset = 0;
            open_file_table[fd].size = node->size;
            open_file_table[fd].file_data = NULL;
            spinlock_release(&vfs_lock);
            return fd;
        }
    }
    
    vfs_destroy_node(node);
    spinlock_release(&vfs_lock);
    return -1;
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }

    open_file_t *file = &open_file_table[fd];
    
    if (file->node && file->node->read) {
        ssize_t result = file->node->read(file->node, file->offset, count, buffer);
        if (result > 0) {
            file->offset += result;
        }
        spinlock_release(&vfs_lock);
        return result;
    }
    
    // Initrd fallback
    if (file->file_data) {
        if (file->offset >= file->size) {
            spinlock_release(&vfs_lock);
            return 0;
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
    
    spinlock_release(&vfs_lock);
    return -1;
}

ssize_t vfs_write(int fd, void *buffer, size_t count) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }

    open_file_t *file = &open_file_table[fd];
    
    if (file->node && file->node->write) {
        ssize_t result = file->node->write(file->node, file->offset, count, buffer);
        if (result > 0) {
            file->offset += result;
            // Update file size if we extended it
            if (file->offset > file->node->size) {
                file->node->size = file->offset;
            }
        }
        spinlock_release(&vfs_lock);
        return result;
    }
    
    spinlock_release(&vfs_lock);
    return -1;
}

int vfs_close(int fd) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    open_file_t *file = &open_file_table[fd];
    
    if (file->node) {
        vfs_destroy_node(file->node);
    }
    
    open_file_table[fd].in_use = false;
    open_file_table[fd].node = NULL;
    open_file_table[fd].file_data = NULL;
    
    spinlock_release(&vfs_lock);
    return 0;
}

// Create a new file
int vfs_create(const char* path, uint32_t mode) {
    if (!path || !vfs_root) return -1;
    
    spinlock_acquire(&vfs_lock);
    
    // Make a copy of the path for manipulation
    char* path_copy = vfs_strdup(path);
    if (!path_copy) {
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    // Find the last slash to separate directory and filename
    char* last_slash = NULL;
    char* p = path_copy;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }
    
    char* filename;
    vfs_node_t* parent;
    
    if (last_slash) {
        // Has a directory component
        *last_slash = '\0';
        filename = last_slash + 1;
        
        // If path_copy is now empty, it means root directory
        if (path_copy[0] == '\0') {
            parent = vfs_root;
            parent->refcount++;
        } else {
            parent = vfs_path_to_node(path_copy);
        }
    } else {
        // No directory component, create in root
        filename = path_copy;
        parent = vfs_root;
        parent->refcount++;
    }
    
    if (!parent || parent->type != VFS_DIRECTORY) {
        if (parent) vfs_destroy_node(parent);
        pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    // Check if file already exists
    if (parent->finddir) {
        vfs_node_t* existing = parent->finddir(parent, filename);
        if (existing) {
            vfs_destroy_node(existing);
            vfs_destroy_node(parent);
            pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
            spinlock_release(&vfs_lock);
            return -1; // File already exists
        }
    }
    
    // Create the file
    int result = -1;
    if (parent->create) {
        result = parent->create(parent, filename, VFS_FILE);
    }
    
    vfs_destroy_node(parent);
    pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
    spinlock_release(&vfs_lock);
    
    return result;
}


// Create a directory
int vfs_mkdir(const char* path, uint32_t mode) {
    if (!path || !vfs_root) return -1;
    
    spinlock_acquire(&vfs_lock);
    
    // Make a copy of the path for manipulation
    char* path_copy = vfs_strdup(path);
    if (!path_copy) {
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    // Find the last slash to separate directory and dirname
    char* last_slash = NULL;
    char* p = path_copy;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }
    
    char* dirname;
    vfs_node_t* parent;
    
    if (last_slash) {
        // Has a directory component
        *last_slash = '\0';
        dirname = last_slash + 1;
        
        // If path_copy is now empty, it means root directory
        if (path_copy[0] == '\0') {
            parent = vfs_root;
            parent->refcount++;
        } else {
            parent = vfs_path_to_node(path_copy);
        }
    } else {
        // No directory component, create in root
        dirname = path_copy;
        parent = vfs_root;
        parent->refcount++;
    }
    
    if (!parent || parent->type != VFS_DIRECTORY) {
        if (parent) vfs_destroy_node(parent);
        pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
        spinlock_release(&vfs_lock);
        return -1;
    }
    
    // Check if directory already exists
    if (parent->finddir) {
        vfs_node_t* existing = parent->finddir(parent, dirname);
        if (existing) {
            vfs_destroy_node(existing);
            vfs_destroy_node(parent);
            pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
            spinlock_release(&vfs_lock);
            return -1; // Directory already exists
        }
    }
    
    // Create the directory
    int result = -1;
    if (parent->create) {
        result = parent->create(parent, dirname, VFS_DIRECTORY);
    }
    
    vfs_destroy_node(parent);
    pmm_free_page((void*)((uint64_t)path_copy - g_hhdm_offset));
    spinlock_release(&vfs_lock);
    
    return result;
}

void vfs_sync(void) {
    // Flush all block devices
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        if (block_device_table[i].in_use) {
            // Call AHCI flush for this device
            ahci_flush_cache(i);
        }
    }
}