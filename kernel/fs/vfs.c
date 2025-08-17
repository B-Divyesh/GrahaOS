// kernel/fs/vfs.c
#include "vfs.h"
#include "../initrd.h"
#include <stddef.h>
#include "../sync/spinlock.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"

// The system-wide open file table
static open_file_t open_file_table[MAX_OPEN_FILES];
// The system-wide block device table
static block_device_t block_device_table[MAX_BLOCK_DEVICES];
// NEW: Filesystem table
static vfs_filesystem_t filesystem_table[MAX_FILESYSTEMS];
// NEW: Root filesystem node
static vfs_node_t* vfs_root = NULL;

spinlock_t vfs_lock = SPINLOCK_INITIALIZER("vfs");

// Simple memory functions for internal use
static void *vfs_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }
    return dest;
}

static void *vfs_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

static size_t vfs_strlen(const char *str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

static int vfs_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static char* vfs_strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++));
    return ret;
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

// NEW: Node management functions
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
        // Close the node if it has a close handler
        if (node->close) {
            node->close(node);
        }
        
        // Free the memory
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

// Modified open function to work with nodes
int vfs_open(const char *pathname) {
    // First try to open from filesystem if mounted
    if (vfs_root && vfs_root->finddir) {
        vfs_node_t* node = vfs_root->finddir(vfs_root, pathname);
        if (node) {
            spinlock_acquire(&vfs_lock);
            for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
                if (!open_file_table[fd].in_use) {
                    open_file_table[fd].in_use = true;
                    open_file_table[fd].node = node;
                    open_file_table[fd].size = node->size;
                    open_file_table[fd].offset = 0;
                    open_file_table[fd].file_data = NULL;
                    spinlock_release(&vfs_lock);
                    return fd;
                }
            }
            spinlock_release(&vfs_lock);
            return -1;
        }
    }
    
    // Fall back to initrd
    size_t file_size;
    void *file_data = initrd_lookup(pathname, &file_size);

    if (file_data == NULL) {
        return -1;
    }
    
    spinlock_acquire(&vfs_lock);
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

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }

    open_file_t *file = &open_file_table[fd];
    
    // If it's a node-based file
    if (file->node && file->node->read) {
        ssize_t result = file->node->read(file->node, file->offset, count, buffer);
        if (result > 0) {
            file->offset += result;
        }
        spinlock_release(&vfs_lock);
        return result;
    }
    
    // Otherwise, it's an initrd file
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

ssize_t vfs_write(int fd, void *buffer, size_t count) {
    spinlock_acquire(&vfs_lock);
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_file_table[fd].in_use) {
        spinlock_release(&vfs_lock);
        return -1;
    }

    open_file_t *file = &open_file_table[fd];
    
    // If it's a node-based file with write support
    if (file->node && file->node->write) {
        ssize_t result = file->node->write(file->node, file->offset, count, buffer);
        if (result > 0) {
            file->offset += result;
        }
        spinlock_release(&vfs_lock);
        return result;
    }
    
    // Initrd files are read-only
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
    
    // If it's a node-based file
    if (file->node) {
        vfs_destroy_node(file->node);
    }
    
    open_file_table[fd].in_use = false;
    spinlock_release(&vfs_lock);
    return 0;
}