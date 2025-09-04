// kernel/fs/grahafs.c - COMPLETE FILE
#include "grahafs.h"
#include "vfs.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/video/framebuffer.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/drivers/ahci/ahci.h"

static block_device_t* fs_device = NULL;
static grahafs_superblock_t superblock;
static uint8_t* free_space_bitmap = NULL;
static spinlock_t grahafs_lock = SPINLOCK_INITIALIZER("grahafs");
static bool fs_mounted = false;  // Add mount status flag

// Memory utility functions
static void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

static void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++));
    return ret;
}

static size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// Bitmap operations
static void bitmap_set(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_clear(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int bitmap_test(uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

// Block I/O helpers
// Fix block I/O helpers to validate parameters
static int read_fs_block(uint32_t block_num, void* buf) {
    if (!fs_device || !fs_mounted) {
        framebuffer_draw_rect(200, 300, 400, 100, COLOR_RED);
        framebuffer_draw_string("CRITICAL: No device mounted!", 210, 320, COLOR_WHITE, COLOR_RED);
        return -1;
    }
    
    if (block_num >= superblock.total_blocks) {
        framebuffer_draw_rect(200, 300, 400, 100, COLOR_RED);
        framebuffer_draw_string("CRITICAL: Invalid block number!", 210, 320, COLOR_WHITE, COLOR_RED);
        return -1;
    }
    
    return fs_device->read_blocks(fs_device->device_id, block_num, 1, buf);
}

static int write_fs_block(uint32_t block_num, void* buf) {
    if (!fs_device || !fs_mounted) {
        framebuffer_draw_rect(200, 300, 400, 100, COLOR_RED);
        framebuffer_draw_string("CRITICAL: No device mounted!", 210, 320, COLOR_WHITE, COLOR_RED);
        return -1;
    }
    
    if (block_num >= superblock.total_blocks) {
        framebuffer_draw_rect(200, 300, 400, 100, COLOR_RED);
        framebuffer_draw_string("CRITICAL: Invalid block number!", 210, 320, COLOR_WHITE, COLOR_RED);
        char msg[64];
        int pos = 0;
        char* text = "Block: ";
        while (*text) msg[pos++] = *text++;
        // Convert block_num to string
        uint32_t num = block_num;
        char digits[12];
        int digit_count = 0;
        do {
            digits[digit_count++] = '0' + (num % 10);
            num /= 10;
        } while (num > 0);
        while (digit_count > 0) {
            msg[pos++] = digits[--digit_count];
        }
        msg[pos] = '\0';
        framebuffer_draw_string(msg, 210, 340, COLOR_WHITE, COLOR_RED);
        return -1;
    }
    
    return fs_device->write_blocks(fs_device->device_id, block_num, 1, buf);
}


// Update superblock on disk
static int write_superblock(void) {
    return write_fs_block(0, &superblock);
}

// Allocate a free block
// Fix allocate_block to properly initialize on first allocation
static uint32_t allocate_block(void) {
    if (!fs_mounted) return 0;
    
    spinlock_acquire(&grahafs_lock);
    
    // Ensure we start from data blocks area
    uint32_t start_block = superblock.data_blocks_start_block;
    if (start_block == 0 || start_block >= superblock.total_blocks) {
        spinlock_release(&grahafs_lock);
        return 0;
    }
    
    for (uint32_t i = start_block; i < superblock.total_blocks; i++) {
        if (!bitmap_test(free_space_bitmap, i)) {
            bitmap_set(free_space_bitmap, i);
            superblock.free_blocks--;
            
            // Write updated bitmap block
            uint32_t bitmap_block_index = (i / (8 * GRAHAFS_BLOCK_SIZE));
            uint32_t bitmap_block = superblock.bitmap_start_block + bitmap_block_index;
            
            void* bitmap_buffer_phys = pmm_alloc_page();
            if (!bitmap_buffer_phys) {
                spinlock_release(&grahafs_lock);
                return 0;
            }
            void* bitmap_buffer = (void*)((uint64_t)bitmap_buffer_phys + g_hhdm_offset);
            
            // Copy the relevant part of bitmap to buffer
            memcpy(bitmap_buffer, free_space_bitmap + (bitmap_block_index * GRAHAFS_BLOCK_SIZE), GRAHAFS_BLOCK_SIZE);
            write_fs_block(bitmap_block, bitmap_buffer);
            pmm_free_page(bitmap_buffer_phys);
            
            // Update superblock on disk
            void* sb_buffer_phys = pmm_alloc_page();
            if (sb_buffer_phys) {
                void* sb_buffer = (void*)((uint64_t)sb_buffer_phys + g_hhdm_offset);
                memcpy(sb_buffer, &superblock, sizeof(grahafs_superblock_t));
                // Ensure rest of block is zeroed
                memset((uint8_t*)sb_buffer + sizeof(grahafs_superblock_t), 0, 
                       GRAHAFS_BLOCK_SIZE - sizeof(grahafs_superblock_t));
                write_fs_block(0, sb_buffer);
                pmm_free_page(sb_buffer_phys);
            }
            
            spinlock_release(&grahafs_lock);
            return i;
        }
    }
    
    spinlock_release(&grahafs_lock);
    return 0;
}

// Free a block
static void free_block(uint32_t block_num) {
    if (block_num < superblock.data_blocks_start_block || block_num >= superblock.total_blocks) return;
    
    spinlock_acquire(&grahafs_lock);
    
    bitmap_clear(free_space_bitmap, block_num);
    superblock.free_blocks++;
    
    // Write updated bitmap block
    uint32_t bitmap_block = superblock.bitmap_start_block + (block_num / (8 * GRAHAFS_BLOCK_SIZE));
    write_fs_block(bitmap_block, free_space_bitmap + ((bitmap_block - superblock.bitmap_start_block) * GRAHAFS_BLOCK_SIZE));
    
    // Update superblock
    write_superblock();
    
    spinlock_release(&grahafs_lock);
}

// Allocate a free inode
static uint32_t allocate_inode(void) {
    spinlock_acquire(&grahafs_lock);
    
    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) {
        spinlock_release(&grahafs_lock);
        return 0;
    }
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    // Start from inode 2 (0 is reserved, 1 is root)
    for (uint32_t i = 2; i < GRAHAFS_MAX_INODES; i++) {
        uint32_t block = superblock.inode_table_start_block + (i * sizeof(grahafs_inode_t) / GRAHAFS_BLOCK_SIZE);
        uint32_t offset = (i * sizeof(grahafs_inode_t)) % GRAHAFS_BLOCK_SIZE;
        
        if (read_fs_block(block, buffer) != 0) continue;
        
        grahafs_inode_t* inode = (grahafs_inode_t*)((uint8_t*)buffer + offset);
        if (inode->type == 0) { // Free inode
            superblock.free_inodes--;
            write_superblock();
            
            pmm_free_page(buffer_phys);
            spinlock_release(&grahafs_lock);
            return i;
        }
    }
    
    pmm_free_page(buffer_phys);
    spinlock_release(&grahafs_lock);
    return 0; // No free inodes
}

// Read an inode
static int read_inode(uint32_t inode_num, grahafs_inode_t* inode) {
    if (inode_num >= GRAHAFS_MAX_INODES) return -1;

    uint32_t bytes_offset = inode_num * sizeof(grahafs_inode_t);
    uint32_t block = superblock.inode_table_start_block + (bytes_offset / GRAHAFS_BLOCK_SIZE);
    uint32_t offset = bytes_offset % GRAHAFS_BLOCK_SIZE;

    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) return -1;
    
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(block, buffer) != 0) {
        pmm_free_page(buffer_phys);
        return -1;
    }

    memcpy(inode, (uint8_t*)buffer + offset, sizeof(grahafs_inode_t));
    pmm_free_page(buffer_phys);
    return 0;
}

// Write an inode
static int write_inode(uint32_t inode_num, grahafs_inode_t* inode) {
    if (inode_num >= GRAHAFS_MAX_INODES) return -1;

    uint32_t bytes_offset = inode_num * sizeof(grahafs_inode_t);
    uint32_t block = superblock.inode_table_start_block + (bytes_offset / GRAHAFS_BLOCK_SIZE);
    uint32_t offset = bytes_offset % GRAHAFS_BLOCK_SIZE;

    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) return -1;
    
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(block, buffer) != 0) {
        pmm_free_page(buffer_phys);
        return -1;
    }

    memcpy((uint8_t*)buffer + offset, inode, sizeof(grahafs_inode_t));
    
    int result = write_fs_block(block, buffer);
    pmm_free_page(buffer_phys);
    return result;
}

// Add directory entry
static int add_dirent(grahafs_inode_t* dir_inode, uint32_t dir_inode_num, const char* name, uint32_t inode_num) {
    if (dir_inode->type != GRAHAFS_INODE_TYPE_DIRECTORY) return -1;
    
    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) return -1;
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    // For simplicity, only use first direct block
    if (dir_inode->direct_blocks[0] == 0) {
        // Allocate first block for directory
        dir_inode->direct_blocks[0] = allocate_block();
        if (dir_inode->direct_blocks[0] == 0) {
            pmm_free_page(buffer_phys);
            return -1;
        }
        memset(buffer, 0, GRAHAFS_BLOCK_SIZE);
    } else {
        if (read_fs_block(dir_inode->direct_blocks[0], buffer) != 0) {
            pmm_free_page(buffer_phys);
            return -1;
        }
    }
    
    grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    
    // Find free slot
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_num == 0) {
            entries[i].inode_num = inode_num;
            strcpy(entries[i].name, name);
            
            // Write back directory block
            if (write_fs_block(dir_inode->direct_blocks[0], buffer) != 0) {
                pmm_free_page(buffer_phys);
                return -1;
            }
            
            // Update directory size
            dir_inode->size = (i + 1) * sizeof(grahafs_dirent_t);
            write_inode(dir_inode_num, dir_inode);
            
            pmm_free_page(buffer_phys);
            return 0;
        }
    }
    
    pmm_free_page(buffer_phys);
    return -1; // Directory full
}

// VFS operations
ssize_t grahafs_read(vfs_node_t* node, uint64_t offset, size_t size, void* buffer) {
    if (!node || !buffer) return -1;
    
    spinlock_acquire(&grahafs_lock);
    
    grahafs_inode_t inode;
    if (read_inode(node->inode, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    if (inode.type != GRAHAFS_INODE_TYPE_FILE) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    if (offset >= inode.size) {
        spinlock_release(&grahafs_lock);
        return 0;
    }
    
    size_t bytes_to_read = size;
    if (offset + size > inode.size) {
        bytes_to_read = inode.size - offset;
    }
    
    size_t bytes_read = 0;
    uint32_t block_index = offset / GRAHAFS_BLOCK_SIZE;
    uint32_t block_offset = offset % GRAHAFS_BLOCK_SIZE;
    
    void* temp_buffer_phys = pmm_alloc_page();
    if (!temp_buffer_phys) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    void* temp_buffer = (void*)((uint64_t)temp_buffer_phys + g_hhdm_offset);
    
    while (bytes_read < bytes_to_read && block_index < 12) {
        if (inode.direct_blocks[block_index] == 0) break;
        
        if (read_fs_block(inode.direct_blocks[block_index], temp_buffer) != 0) {
            pmm_free_page(temp_buffer_phys);
            spinlock_release(&grahafs_lock);
            return -1;
        }
        
        size_t copy_size = GRAHAFS_BLOCK_SIZE - block_offset;
        if (copy_size > bytes_to_read - bytes_read) {
            copy_size = bytes_to_read - bytes_read;
        }
        
        memcpy((uint8_t*)buffer + bytes_read, (uint8_t*)temp_buffer + block_offset, copy_size);
        
        bytes_read += copy_size;
        block_index++;
        block_offset = 0;
    }
    
    pmm_free_page(temp_buffer_phys);
    spinlock_release(&grahafs_lock);
    return bytes_read;
}

ssize_t grahafs_write(vfs_node_t* node, uint64_t offset, size_t size, void* buffer) {
    if (!node || !buffer) return -1;
    
    spinlock_acquire(&grahafs_lock);
    
    grahafs_inode_t inode;
    if (read_inode(node->inode, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    if (inode.type != GRAHAFS_INODE_TYPE_FILE) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    size_t bytes_written = 0;
    uint32_t block_index = offset / GRAHAFS_BLOCK_SIZE;
    uint32_t block_offset = offset % GRAHAFS_BLOCK_SIZE;
    
    void* temp_buffer_phys = pmm_alloc_page();
    if (!temp_buffer_phys) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    void* temp_buffer = (void*)((uint64_t)temp_buffer_phys + g_hhdm_offset);
    
    while (bytes_written < size && block_index < 12) {
        // Allocate block if needed
        if (inode.direct_blocks[block_index] == 0) {
            inode.direct_blocks[block_index] = allocate_block();
            if (inode.direct_blocks[block_index] == 0) {
                pmm_free_page(temp_buffer_phys);
                spinlock_release(&grahafs_lock);
                return bytes_written; // Out of space
            }
            memset(temp_buffer, 0, GRAHAFS_BLOCK_SIZE);
        } else {
            // Read existing block for partial writes
            if (read_fs_block(inode.direct_blocks[block_index], temp_buffer) != 0) {
                pmm_free_page(temp_buffer_phys);
                spinlock_release(&grahafs_lock);
                return -1;
            }
        }
        
        size_t copy_size = GRAHAFS_BLOCK_SIZE - block_offset;
        if (copy_size > size - bytes_written) {
            copy_size = size - bytes_written;
        }
        
        memcpy((uint8_t*)temp_buffer + block_offset, (uint8_t*)buffer + bytes_written, copy_size);
        
        // Write block back
        if (write_fs_block(inode.direct_blocks[block_index], temp_buffer) != 0) {
            pmm_free_page(temp_buffer_phys);
            spinlock_release(&grahafs_lock);
            return -1;
        }
        
        bytes_written += copy_size;
        block_index++;
        block_offset = 0;
    }
    
    // Update inode size if extended
    if (offset + bytes_written > inode.size) {
        inode.size = offset + bytes_written;
        write_inode(node->inode, &inode);
    }

    // After successful write/create, flush to ensure persistence
    if (fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }
    
    pmm_free_page(temp_buffer_phys);
    spinlock_release(&grahafs_lock);
    return bytes_written;
}

// Create file or directory
int grahafs_create(vfs_node_t* parent, const char* name, uint32_t type) {
    if (!parent || !name || strlen(name) >= GRAHAFS_MAX_FILENAME) return -1;
    
    spinlock_acquire(&grahafs_lock);
    
    // Read parent inode
    grahafs_inode_t parent_inode;
    if (read_inode(parent->inode, &parent_inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    if (parent_inode.type != GRAHAFS_INODE_TYPE_DIRECTORY) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    // Allocate new inode
    uint32_t new_inode_num = allocate_inode();
    if (new_inode_num == 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    // Initialize new inode
    grahafs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(grahafs_inode_t));
    new_inode.type = (type == VFS_DIRECTORY) ? GRAHAFS_INODE_TYPE_DIRECTORY : GRAHAFS_INODE_TYPE_FILE;
    new_inode.size = 0;
    new_inode.link_count = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.mode = 0755;
    
    // Get current time (simplified - just use a counter)
    static uint64_t timestamp = 0;
    timestamp++;
    new_inode.creation_time = timestamp;
    new_inode.modification_time = timestamp;
    new_inode.access_time = timestamp;
    
    // If it's a directory, add . and .. entries
    if (type == VFS_DIRECTORY) {
        uint32_t dir_block = allocate_block();
        if (dir_block == 0) {
            spinlock_release(&grahafs_lock);
            return -1;
        }
        
        new_inode.direct_blocks[0] = dir_block;
        
        void* buffer_phys = pmm_alloc_page();
        if (!buffer_phys) {
            free_block(dir_block);
            spinlock_release(&grahafs_lock);
            return -1;
        }
        void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
        
        memset(buffer, 0, GRAHAFS_BLOCK_SIZE);
        grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
        
        // Add . entry
        entries[0].inode_num = new_inode_num;
        strcpy(entries[0].name, ".");
        
        // Add .. entry
        entries[1].inode_num = parent->inode;
        strcpy(entries[1].name, "..");
        
        write_fs_block(dir_block, buffer);
        pmm_free_page(buffer_phys);
        
        new_inode.size = 2 * sizeof(grahafs_dirent_t);
        new_inode.link_count = 2;
    }
    
    // Write new inode
    if (write_inode(new_inode_num, &new_inode) != 0) {
        if (new_inode.direct_blocks[0]) free_block(new_inode.direct_blocks[0]);
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    // Add entry to parent directory
    if (add_dirent(&parent_inode, parent->inode, name, new_inode_num) != 0) {
        // Cleanup on failure
        if (new_inode.direct_blocks[0]) free_block(new_inode.direct_blocks[0]);
        memset(&new_inode, 0, sizeof(grahafs_inode_t));
        write_inode(new_inode_num, &new_inode);
        spinlock_release(&grahafs_lock);
        return -1;
    }

    // After successful write/create, flush to ensure persistence
    if (fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }
    
    spinlock_release(&grahafs_lock);
    return 0;
}

vfs_node_t* grahafs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !name) return NULL;
    
    spinlock_acquire(&grahafs_lock);
    
    grahafs_inode_t inode;
    if (read_inode(node->inode, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    if (inode.type != GRAHAFS_INODE_TYPE_DIRECTORY) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    if (inode.direct_blocks[0] == 0) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(inode.direct_blocks[0], buffer) != 0) {
        pmm_free_page(buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_num == 0) continue;
        if (entries[i].name[0] == '\0') continue;
        
        if (strcmp(entries[i].name, name) == 0) {
            grahafs_inode_t found_inode;
            if (read_inode(entries[i].inode_num, &found_inode) != 0) {
                pmm_free_page(buffer_phys);
                spinlock_release(&grahafs_lock);
                return NULL;
            }
            
            uint32_t type = (found_inode.type == GRAHAFS_INODE_TYPE_DIRECTORY) ? 
                           VFS_DIRECTORY : VFS_FILE;
            vfs_node_t* result = vfs_create_node(entries[i].name, type);
            if (result) {
                result->inode = entries[i].inode_num;
                result->size = found_inode.size;
                result->read = grahafs_read;
                result->write = grahafs_write;
                result->finddir = grahafs_finddir;
                result->readdir = grahafs_readdir;
                result->create = grahafs_create;
                result->fs = (vfs_filesystem_t*)fs_device;
            }
            
            pmm_free_page(buffer_phys);
            spinlock_release(&grahafs_lock);
            return result;
        }
    }
    
    pmm_free_page(buffer_phys);
    spinlock_release(&grahafs_lock);
    return NULL;
}

vfs_node_t* grahafs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node) return NULL;
    
    spinlock_acquire(&grahafs_lock);
    
    grahafs_inode_t inode;
    if (read_inode(node->inode, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    if (inode.type != GRAHAFS_INODE_TYPE_DIRECTORY) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    if (inode.direct_blocks[0] == 0) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(inode.direct_blocks[0], buffer) != 0) {
        pmm_free_page(buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    
    uint32_t current_index = 0;
    for (int i = 0; i < max_entries; i++) {
        if (entries[i].inode_num == 0) continue;
        if (entries[i].name[0] == '\0') continue;
        
        if (current_index == index) {
            grahafs_inode_t found_inode;
            if (read_inode(entries[i].inode_num, &found_inode) != 0) {
                pmm_free_page(buffer_phys);
                spinlock_release(&grahafs_lock);
                return NULL;
            }
            
            uint32_t type = (found_inode.type == GRAHAFS_INODE_TYPE_DIRECTORY) ? 
                           VFS_DIRECTORY : VFS_FILE;
            vfs_node_t* result = vfs_create_node(entries[i].name, type);
            if (result) {
                result->inode = entries[i].inode_num;
                result->size = found_inode.size;
                result->read = grahafs_read;
                result->write = grahafs_write;
                result->finddir = grahafs_finddir;
                result->readdir = grahafs_readdir;
                result->create = grahafs_create;
                result->fs = (vfs_filesystem_t*)fs_device;
            }
            
            pmm_free_page(buffer_phys);
            spinlock_release(&grahafs_lock);
            return result;
        }
        current_index++;
    }
    
    pmm_free_page(buffer_phys);
    spinlock_release(&grahafs_lock);
    return NULL;
}



void grahafs_init(void) {
    spinlock_init(&grahafs_lock, "grahafs");
    framebuffer_draw_string("GrahaFS: Driver initialized.", 10, 650, COLOR_GREEN, 0x00101828);
}

// Fix mount function to properly preserve superblock
vfs_node_t* grahafs_mount(block_device_t* device) {
    if (!device) return NULL;
    
    spinlock_acquire(&grahafs_lock);
    
    // Clear any previous mount
    fs_mounted = false;
    fs_device = device;

    // Allocate buffer for superblock
    void* sb_buffer_phys = pmm_alloc_page();
    if (!sb_buffer_phys) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* sb_buffer = (void*)((uint64_t)sb_buffer_phys + g_hhdm_offset);

    // Read superblock from disk
    if (device->read_blocks(device->device_id, 0, 1, sb_buffer) != 0) {
        framebuffer_draw_string("GrahaFS: Failed to read superblock.", 10, 750, COLOR_RED, 0x00101828);
        pmm_free_page(sb_buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    // Copy to our global superblock structure
    memcpy(&superblock, sb_buffer, sizeof(grahafs_superblock_t));
    pmm_free_page(sb_buffer_phys);

    // Verify magic
    if (superblock.magic != GRAHAFS_MAGIC) {
        framebuffer_draw_string("GrahaFS: Invalid magic number!", 10, 750, COLOR_RED, 0x00101828);
        
        // Display the actual magic we got
        char msg[80] = "Expected: 0x47524148414F5321, Got: 0x";
        int pos = 38;
        uint64_t magic = superblock.magic;
        for (int i = 0; i < 16; i++) {
            char hex = "0123456789ABCDEF"[(magic >> (60 - i * 4)) & 0xF];
            msg[pos++] = hex;
        }
        msg[pos] = '\0';
        framebuffer_draw_string(msg, 10, 770, COLOR_RED, 0x00101828);
        
        spinlock_release(&grahafs_lock);
        return NULL;
    }

    // Validate superblock fields
    if (superblock.total_blocks == 0 || superblock.total_blocks > 65536) {
        framebuffer_draw_string("GrahaFS: Invalid block count!", 10, 750, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }

    // Load bitmap
    uint32_t bitmap_blocks = (superblock.total_blocks + 8 * GRAHAFS_BLOCK_SIZE - 1) / (8 * GRAHAFS_BLOCK_SIZE);
    size_t bitmap_size = bitmap_blocks * GRAHAFS_BLOCK_SIZE;
    size_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    void* bitmap_phys = pmm_alloc_pages(bitmap_pages);
    if (!bitmap_phys) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    free_space_bitmap = (uint8_t*)((uint64_t)bitmap_phys + g_hhdm_offset);
    
    // Read bitmap from disk
    for (uint32_t i = 0; i < bitmap_blocks; i++) {
        if (device->read_blocks(device->device_id, superblock.bitmap_start_block + i, 1, 
                               free_space_bitmap + (i * GRAHAFS_BLOCK_SIZE)) != 0) {
            pmm_free_pages(bitmap_phys, bitmap_pages);
            spinlock_release(&grahafs_lock);
            return NULL;
        }
    }

    // Mark as mounted
    fs_mounted = true;
    
    framebuffer_draw_string("GrahaFS: Filesystem mounted successfully!", 10, 750, COLOR_GREEN, 0x00101828);

    // Create root VFS node
    vfs_node_t* root = vfs_create_node("/", VFS_DIRECTORY);
    if (!root) {
        fs_mounted = false;
        pmm_free_pages(bitmap_phys, bitmap_pages);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    // Read root inode
    grahafs_inode_t root_inode;
    
    uint32_t root_inode_num = superblock.root_inode;
    uint32_t bytes_offset = root_inode_num * sizeof(grahafs_inode_t);
    uint32_t block = superblock.inode_table_start_block + (bytes_offset / GRAHAFS_BLOCK_SIZE);
    uint32_t offset = bytes_offset % GRAHAFS_BLOCK_SIZE;
    
    void* inode_buffer_phys = pmm_alloc_page();
    if (inode_buffer_phys) {
        void* inode_buffer = (void*)((uint64_t)inode_buffer_phys + g_hhdm_offset);
        
        if (device->read_blocks(device->device_id, block, 1, inode_buffer) == 0) {
            memcpy(&root_inode, (uint8_t*)inode_buffer + offset, sizeof(grahafs_inode_t));
            
            root->inode = root_inode_num;
            root->size = root_inode.size;
            root->read = grahafs_read;
            root->write = grahafs_write;
            root->finddir = grahafs_finddir;
            root->readdir = grahafs_readdir;
            root->create = grahafs_create;
            root->fs = (vfs_filesystem_t*)device;
        }
        
        pmm_free_page(inode_buffer_phys);
    }
    
    vfs_set_root(root);
    
    spinlock_release(&grahafs_lock);
    return root;
}

int grahafs_unmount(vfs_node_t* root) {
    if (!root) return -1;
    
    spinlock_acquire(&grahafs_lock);
    
    vfs_set_root(NULL);
    vfs_destroy_node(root);
    
    // Free bitmap memory
    if (free_space_bitmap) {
        uint32_t bitmap_blocks = (superblock.total_blocks + 8 * GRAHAFS_BLOCK_SIZE - 1) / (8 * GRAHAFS_BLOCK_SIZE);
        size_t bitmap_size = bitmap_blocks * GRAHAFS_BLOCK_SIZE;
        size_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
        pmm_free_pages((void*)((uint64_t)free_space_bitmap - g_hhdm_offset), bitmap_pages);
        free_space_bitmap = NULL;
    }
    
    fs_device = NULL;
    
    spinlock_release(&grahafs_lock);
    return 0;
}