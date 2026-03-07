// kernel/fs/grahafs.c - COMPLETE FILE
#include "grahafs.h"
#include "simhash.h"
#include "cluster.h"
#include "vfs.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/video/framebuffer.h"
#include "../sync/spinlock.h"
#include "../../arch/x86_64/drivers/ahci/ahci.h"
#include "../../arch/x86_64/drivers/serial/serial.h"
#include "../capability.h"

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

static const char* grahafs_strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
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
    if (!fs_mounted) return -1;
    
    void* sb_buffer_phys = pmm_alloc_page();
    if (!sb_buffer_phys) return -1;
    
    void* sb_buffer = (void*)((uint64_t)sb_buffer_phys + g_hhdm_offset);
    
    // Copy superblock to buffer
    memcpy(sb_buffer, &superblock, sizeof(grahafs_superblock_t));
    
    // Ensure rest of block is zeroed
    memset((uint8_t*)sb_buffer + sizeof(grahafs_superblock_t), 0, 
           GRAHAFS_BLOCK_SIZE - sizeof(grahafs_superblock_t));
    
    int result = write_fs_block(0, sb_buffer);
    pmm_free_page(sb_buffer_phys);
    
    // Force flush after superblock write
    if (result == 0 && fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }
    
    return result;
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

// Write an inode - FIX the bounds check
static int write_inode(uint32_t inode_num, grahafs_inode_t* inode) {
    if (!inode || !fs_mounted) return -1;
    if (inode_num >= GRAHAFS_MAX_INODES) {
        framebuffer_draw_string("ERROR: Invalid inode number in write_inode", 10, 800, COLOR_RED, 0x00101828);
        return -1;
    }

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
    
    // Flush after inode write
    if (result == 0 && fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }
    
    return result;
}


// Fix add_dirent to validate parameters
static int add_dirent(grahafs_inode_t* dir_inode, uint32_t dir_inode_num, const char* name, uint32_t inode_num) {
    if (!dir_inode || !name || !fs_mounted) return -1;
    if (dir_inode->type != GRAHAFS_INODE_TYPE_DIRECTORY) return -1;
    if (dir_inode_num >= GRAHAFS_MAX_INODES || inode_num >= GRAHAFS_MAX_INODES) return -1;
    
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

// Phase 10c: Truncate file inode to 0 bytes (called from vfs_truncate)
int grahafs_truncate_inode(uint32_t inode_num) {
    spinlock_acquire(&grahafs_lock);

    grahafs_inode_t inode;
    if (read_inode(inode_num, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    if (inode.type != GRAHAFS_INODE_TYPE_FILE) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    inode.size = 0;
    write_inode(inode_num, &inode);

    if (fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }

    spinlock_release(&grahafs_lock);
    return 0;
}

// Create file or directory - ADD VALIDATION
int grahafs_create(vfs_node_t* parent, const char* name, uint32_t type) {
    // Validate inputs
    if (!parent || !name || !fs_mounted) {
        framebuffer_draw_string("ERROR: Invalid params to grahafs_create", 10, 820, COLOR_RED, 0x00101828);
        return -1;
    }
    
    if (strlen(name) == 0 || strlen(name) >= GRAHAFS_MAX_FILENAME) {
        framebuffer_draw_string("ERROR: Invalid filename length", 10, 840, COLOR_RED, 0x00101828);
        return -1;
    }
    
    if (type != VFS_FILE && type != VFS_DIRECTORY) {
        framebuffer_draw_string("ERROR: Invalid file type", 10, 860, COLOR_RED, 0x00101828);
        return -1;
    }
    
    spinlock_acquire(&grahafs_lock);
    
    // Validate parent is a valid inode
    if (parent->inode >= GRAHAFS_MAX_INODES) {
        framebuffer_draw_string("ERROR: Parent has invalid inode", 10, 880, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
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
    
    // Check if file already exists
    if (parent_inode.direct_blocks[0] != 0) {
        void* buffer_phys = pmm_alloc_page();
        if (buffer_phys) {
            void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
            if (read_fs_block(parent_inode.direct_blocks[0], buffer) == 0) {
                grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
                int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
                
                for (int i = 0; i < max_entries; i++) {
                    if (entries[i].inode_num != 0 && strcmp(entries[i].name, name) == 0) {
                        // File already exists
                        pmm_free_page(buffer_phys);
                        spinlock_release(&grahafs_lock);
                        return -1;
                    }
                }
            }
            pmm_free_page(buffer_phys);
        }
    }
    
    // Allocate new inode
    uint32_t new_inode_num = allocate_inode();
    if (new_inode_num == 0 || new_inode_num >= GRAHAFS_MAX_INODES) {
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
    new_inode.mode = (type == VFS_DIRECTORY) ? 0755 : 0644;
    
    // Get current time (simplified - just use a counter)
    static uint64_t timestamp = 1000000;
    timestamp++;
    new_inode.creation_time = timestamp;
    new_inode.modification_time = timestamp;
    new_inode.access_time = timestamp;
    
    // If it's a directory, add . and .. entries
    if (type == VFS_DIRECTORY) {
        uint32_t dir_block = allocate_block();
        if (dir_block == 0) {
            // Return the inode to free pool
            memset(&new_inode, 0, sizeof(grahafs_inode_t));
            write_inode(new_inode_num, &new_inode);
            superblock.free_inodes++;
            write_superblock();
            spinlock_release(&grahafs_lock);
            return -1;
        }
        
        new_inode.direct_blocks[0] = dir_block;
        
        void* buffer_phys = pmm_alloc_page();
        if (!buffer_phys) {
            free_block(dir_block);
            memset(&new_inode, 0, sizeof(grahafs_inode_t));
            write_inode(new_inode_num, &new_inode);
            superblock.free_inodes++;
            write_superblock();
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
        memset(&new_inode, 0, sizeof(grahafs_inode_t));
        write_inode(new_inode_num, &new_inode);
        superblock.free_inodes++;
        write_superblock();
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    // Add entry to parent directory
    if (add_dirent(&parent_inode, parent->inode, name, new_inode_num) != 0) {
        // Cleanup on failure
        if (new_inode.direct_blocks[0]) free_block(new_inode.direct_blocks[0]);
        memset(&new_inode, 0, sizeof(grahafs_inode_t));
        write_inode(new_inode_num, &new_inode);
        superblock.free_inodes++;
        write_superblock();
        spinlock_release(&grahafs_lock);
        return -1;
    }
    
    // Force flush to disk
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



// Driver framework stats callback
static int grahafs_get_driver_stats(state_driver_stat_t *stats, int max) {
    if (!stats || max < 4) return 0;
    const char *k0 = "mounted";
    for (int i = 0; k0[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[0].key[i] = k0[i];
    stats[0].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[0].value = fs_mounted ? 1 : 0;
    const char *k1 = "total_blocks";
    for (int i = 0; k1[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[1].key[i] = k1[i];
    stats[1].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[1].value = fs_mounted ? (uint64_t)superblock.total_blocks : 0;
    const char *k2 = "free_blocks";
    for (int i = 0; k2[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[2].key[i] = k2[i];
    stats[2].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[2].value = fs_mounted ? (uint64_t)superblock.free_blocks : 0;
    const char *k3 = "free_inodes";
    for (int i = 0; k3[i] && i < STATE_STAT_KEY_LEN - 1; i++) stats[3].key[i] = k3[i];
    stats[3].key[STATE_STAT_KEY_LEN - 1] = '\0';
    stats[3].value = fs_mounted ? (uint64_t)superblock.free_inodes : 0;
    return 4;
}

void grahafs_init(void) {
    spinlock_init(&grahafs_lock, "grahafs");
    framebuffer_draw_string("GrahaFS: Driver initialized.", 10, 650, COLOR_GREEN, 0x00101828);

    // Register with Capability Activation Network
    const char *gfs_deps[] = {"disk"};
    cap_op_t gfs_ops[2];
    cap_op_set(&gfs_ops[0], "mount", 1, 1);
    cap_op_set(&gfs_ops[1], "sync",  0, 1);
    cap_register("filesystem", CAP_SERVICE, CAP_SUBTYPE_FS, -1, gfs_deps, 1,
                 NULL, NULL, gfs_ops, 2, grahafs_get_driver_stats);
}

// Forward declaration for cluster rebuild (defined below)
static void grahafs_cluster_rebuild_locked(void);

// Mount function with robust diagnostics
vfs_node_t* grahafs_mount(block_device_t* device) {
    serial_write("[GrahaFS] mount called\n");

    if (!device) {
        serial_write("[GrahaFS] ERROR: NULL device!\n");
        return NULL;
    }

    serial_write("[GrahaFS] device_id=");
    serial_write_dec(device->device_id);
    serial_write(" block_size=");
    serial_write_dec(device->block_size);
    serial_write(" read_blocks=");
    serial_write_hex((uint64_t)device->read_blocks);
    serial_write(" write_blocks=");
    serial_write_hex((uint64_t)device->write_blocks);
    serial_write("\n");

    spinlock_acquire(&grahafs_lock);

    // Clear any previous mount
    fs_mounted = false;
    fs_device = device;

    // Allocate buffer for superblock
    void* sb_buffer_phys = pmm_alloc_page();
    if (!sb_buffer_phys) {
        serial_write("[GrahaFS] ERROR: Failed to allocate page for superblock buffer!\n");
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* sb_buffer = (void*)((uint64_t)sb_buffer_phys + g_hhdm_offset);
    serial_write("[GrahaFS] sb_buffer phys=");
    serial_write_hex((uint64_t)sb_buffer_phys);
    serial_write(" virt=");
    serial_write_hex((uint64_t)sb_buffer);
    serial_write("\n");

    // Clear the buffer first to detect if read actually writes data
    memset(sb_buffer, 0xAA, 4096);

    // Read superblock from disk
    serial_write("[GrahaFS] Reading block 0...\n");
    int read_result = device->read_blocks(device->device_id, 0, 1, sb_buffer);
    serial_write("[GrahaFS] read_blocks returned: ");
    serial_write_dec(read_result);
    serial_write("\n");

    if (read_result != 0) {
        serial_write("[GrahaFS] ERROR: Failed to read superblock from disk!\n");
        framebuffer_draw_string("GrahaFS: Failed to read superblock.", 10, 750, COLOR_RED, 0x00101828);
        pmm_free_page(sb_buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }

    // Hex dump first 64 bytes for debugging
    serial_write("[GrahaFS] First 64 bytes of block 0:\n");
    uint8_t *raw = (uint8_t*)sb_buffer;
    for (int row = 0; row < 4; row++) {
        serial_write("  ");
        for (int col = 0; col < 16; col++) {
            serial_write_hex(raw[row * 16 + col]);
            serial_write(" ");
        }
        serial_write("\n");
    }

    // Copy to our global superblock structure
    memcpy(&superblock, sb_buffer, sizeof(grahafs_superblock_t));
    pmm_free_page(sb_buffer_phys);

    serial_write("[GrahaFS] Superblock magic read: 0x");
    serial_write_hex(superblock.magic);
    serial_write("\n");
    serial_write("[GrahaFS] Expected magic:        0x");
    serial_write_hex(GRAHAFS_MAGIC);
    serial_write("\n");
    serial_write("[GrahaFS] sizeof(superblock_t) = ");
    serial_write_dec(sizeof(grahafs_superblock_t));
    serial_write("\n");

    // Verify magic
    if (superblock.magic != GRAHAFS_MAGIC) {
        serial_write("[GrahaFS] ERROR: Magic mismatch!\n");
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

    serial_write("[GrahaFS] Magic verified OK!\n");

    // Verify filesystem version
    if (superblock.version == 0) {
        serial_write("[GrahaFS] ERROR: Old format (version 0), run 'make reformat' to update\n");
        framebuffer_draw_string("GrahaFS: Old disk format! Run make reformat", 10, 750, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    if (superblock.version > GRAHAFS_VERSION) {
        serial_write("[GrahaFS] ERROR: Unsupported version ");
        serial_write_dec(superblock.version);
        serial_write(", expected ");
        serial_write_dec(GRAHAFS_VERSION);
        serial_write("\n");
        framebuffer_draw_string("GrahaFS: Unsupported disk version!", 10, 750, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    serial_write("[GrahaFS] Version: ");
    serial_write_dec(superblock.version);
    serial_write("\n");

    // Log superblock fields
    serial_write("[GrahaFS] total_blocks=");
    serial_write_dec(superblock.total_blocks);
    serial_write(" bitmap_start=");
    serial_write_dec(superblock.bitmap_start_block);
    serial_write(" inode_start=");
    serial_write_dec(superblock.inode_table_start_block);
    serial_write(" data_start=");
    serial_write_dec(superblock.data_blocks_start_block);
    serial_write(" root_inode=");
    serial_write_dec(superblock.root_inode);
    serial_write(" free_blocks=");
    serial_write_dec(superblock.free_blocks);
    serial_write(" free_inodes=");
    serial_write_dec(superblock.free_inodes);
    serial_write("\n");

    // Validate superblock fields
    if (superblock.total_blocks == 0 || superblock.total_blocks > 65536) {
        serial_write("[GrahaFS] ERROR: Invalid block count!\n");
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

    serial_write("[GrahaFS] Bitmap loaded, filesystem mounted!\n");
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

    // Phase 11b: Rebuild in-memory cluster table from on-disk inodes
    grahafs_cluster_rebuild_locked();

    serial_write("[GrahaFS] Root VFS node created, mount complete\n");
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

// Phase 8a: Get GrahaFS statistics for system state reporting
void grahafs_get_stats(uint32_t *mounted, uint32_t *total_blocks,
                       uint32_t *free_blocks, uint32_t *free_inodes) {
    spinlock_acquire(&grahafs_lock);

    if (mounted) *mounted = fs_mounted ? 1 : 0;
    if (fs_mounted) {
        if (total_blocks) *total_blocks = superblock.total_blocks;
        if (free_blocks) *free_blocks = superblock.free_blocks;
        if (free_inodes) *free_inodes = superblock.free_inodes;
    } else {
        if (total_blocks) *total_blocks = 0;
        if (free_blocks) *free_blocks = 0;
        if (free_inodes) *free_inodes = 0;
    }

    spinlock_release(&grahafs_lock);
}

// Phase 8c: AI Metadata Operations

int grahafs_set_ai_metadata(uint32_t inode_num, const grahafs_ai_metadata_t *meta) {
    if (!meta || !fs_mounted) return -1;
    if (inode_num >= GRAHAFS_MAX_INODES) return -1;

    spinlock_acquire(&grahafs_lock);

    grahafs_inode_t inode;
    if (read_inode(inode_num, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    // Must be an allocated inode
    if (inode.type == 0) {
        spinlock_release(&grahafs_lock);
        return -2;
    }

    // Set importance (always stored inline)
    if (meta->flags & GRAHAFS_META_FLAG_IMPORTANCE) {
        inode.ai_importance = meta->importance > 100 ? 100 : meta->importance;
    }

    // Set tags (inline up to 95 chars, extended for overflow)
    if (meta->flags & GRAHAFS_META_FLAG_TAGS) {
        size_t tag_len = strlen(meta->tags);
        size_t inline_len = tag_len > 95 ? 95 : tag_len;
        memcpy(inode.ai_tags, meta->tags, inline_len);
        inode.ai_tags[inline_len] = '\0';
        inode.ai_flags |= GRAHAFS_AI_HAS_TAGS;

        if (tag_len > 95) {
            inode.ai_flags |= GRAHAFS_AI_HAS_EXTENDED;
        }
    }

    // Determine if extended block is needed
    bool need_extended = (meta->flags & GRAHAFS_META_FLAG_SUMMARY) ||
                         (meta->flags & GRAHAFS_META_FLAG_EMBEDDING) ||
                         ((meta->flags & GRAHAFS_META_FLAG_TAGS) && strlen(meta->tags) > 95);

    if (need_extended) {
        // Allocate extended block if not yet allocated
        if (inode.ai_metadata_block == 0) {
            uint32_t ext_block = allocate_block();
            if (ext_block == 0) {
                spinlock_release(&grahafs_lock);
                return -3;
            }
            inode.ai_metadata_block = ext_block;
            inode.ai_flags |= GRAHAFS_AI_HAS_EXTENDED;
        }

        void* ext_phys = pmm_alloc_page();
        if (!ext_phys) {
            spinlock_release(&grahafs_lock);
            return -3;
        }
        void* ext_buf = (void*)((uint64_t)ext_phys + g_hhdm_offset);

        // Read existing extended block
        grahafs_ai_metadata_block_t *ext_meta;
        if (read_fs_block(inode.ai_metadata_block, ext_buf) != 0) {
            memset(ext_buf, 0, GRAHAFS_BLOCK_SIZE);
        }
        ext_meta = (grahafs_ai_metadata_block_t *)ext_buf;

        // Initialize magic if this is a fresh block
        if (ext_meta->magic != GRAHAFS_AI_META_MAGIC) {
            memset(ext_buf, 0, GRAHAFS_BLOCK_SIZE);
            ext_meta->magic = GRAHAFS_AI_META_MAGIC;
            ext_meta->version = 1;
        }

        // Copy full tags to extended block
        if (meta->flags & GRAHAFS_META_FLAG_TAGS) {
            size_t tag_len = strlen(meta->tags);
            size_t copy_len = tag_len > 511 ? 511 : tag_len;
            memcpy(ext_meta->tags, meta->tags, copy_len);
            ext_meta->tags[copy_len] = '\0';
        }

        // Copy summary
        if (meta->flags & GRAHAFS_META_FLAG_SUMMARY) {
            size_t sum_len = strlen(meta->summary);
            size_t copy_len = sum_len > 1023 ? 1023 : sum_len;
            memcpy(ext_meta->summary, meta->summary, copy_len);
            ext_meta->summary[copy_len] = '\0';
            inode.ai_flags |= GRAHAFS_AI_HAS_SUMMARY;
        }

        // Copy embedding
        if (meta->flags & GRAHAFS_META_FLAG_EMBEDDING) {
            uint32_t dim = meta->embedding_dim > 128 ? 128 : meta->embedding_dim;
            memcpy(ext_meta->embedding, meta->embedding, dim * sizeof(uint64_t));
            ext_meta->embedding_dim = dim;
            inode.ai_flags |= GRAHAFS_AI_HAS_EMBEDDING;
        }

        write_fs_block(inode.ai_metadata_block, ext_buf);
        pmm_free_page(ext_phys);
    }

    // Update AI metadata timestamp (incrementing counter, no RTC available)
    static uint64_t ai_timestamp = 1;
    inode.ai_last_modified = ai_timestamp++;

    write_inode(inode_num, &inode);

    if (fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }

    spinlock_release(&grahafs_lock);
    return 0;
}

int grahafs_get_ai_metadata(uint32_t inode_num, grahafs_ai_metadata_t *meta) {
    if (!meta || !fs_mounted) return -1;
    if (inode_num >= GRAHAFS_MAX_INODES) return -1;

    spinlock_acquire(&grahafs_lock);

    grahafs_inode_t inode;
    if (read_inode(inode_num, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    if (inode.type == 0) {
        spinlock_release(&grahafs_lock);
        return -2;
    }

    // Zero output struct
    memset(meta, 0, sizeof(grahafs_ai_metadata_t));

    // Fill from inline fields
    meta->flags = inode.ai_flags;
    meta->importance = inode.ai_importance;
    meta->access_count = inode.ai_access_count;
    meta->last_modified = inode.ai_last_modified;

    // Copy inline tags
    size_t tag_len = strlen(inode.ai_tags);
    if (tag_len > 0) {
        size_t copy_len = tag_len > 511 ? 511 : tag_len;
        memcpy(meta->tags, inode.ai_tags, copy_len);
        meta->tags[copy_len] = '\0';
    }

    // Read extended block if present
    if ((inode.ai_flags & GRAHAFS_AI_HAS_EXTENDED) && inode.ai_metadata_block != 0) {
        void* ext_phys = pmm_alloc_page();
        if (ext_phys) {
            void* ext_buf = (void*)((uint64_t)ext_phys + g_hhdm_offset);

            if (read_fs_block(inode.ai_metadata_block, ext_buf) == 0) {
                grahafs_ai_metadata_block_t *ext_meta = (grahafs_ai_metadata_block_t *)ext_buf;

                if (ext_meta->magic == GRAHAFS_AI_META_MAGIC) {
                    // Full tags from extended block (overwrite inline copy)
                    if (ext_meta->tags[0] != '\0') {
                        memcpy(meta->tags, ext_meta->tags, 511);
                        meta->tags[511] = '\0';
                    }

                    // Summary
                    if (inode.ai_flags & GRAHAFS_AI_HAS_SUMMARY) {
                        memcpy(meta->summary, ext_meta->summary, 1023);
                        meta->summary[1023] = '\0';
                    }

                    // Embedding
                    if (inode.ai_flags & GRAHAFS_AI_HAS_EMBEDDING) {
                        memcpy(meta->embedding, ext_meta->embedding, 128 * sizeof(uint64_t));
                        meta->embedding_dim = ext_meta->embedding_dim;
                    }
                }
            }

            pmm_free_page(ext_phys);
        }
    }

    // Increment access counter
    inode.ai_access_count++;
    write_inode(inode_num, &inode);

    spinlock_release(&grahafs_lock);
    return 0;
}

int grahafs_search_by_tag(const char *tag, grahafs_search_results_t *results, int max_results) {
    if (!tag || !results || !fs_mounted) return -1;
    if (max_results <= 0 || max_results > 16) max_results = 16;

    spinlock_acquire(&grahafs_lock);

    memset(results, 0, sizeof(grahafs_search_results_t));

    // Read root inode to get directory block
    grahafs_inode_t root_inode;
    if (read_inode(superblock.root_inode, &root_inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    if (root_inode.direct_blocks[0] == 0) {
        spinlock_release(&grahafs_lock);
        return 0;
    }

    void* dir_phys = pmm_alloc_page();
    if (!dir_phys) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    void* dir_buf = (void*)((uint64_t)dir_phys + g_hhdm_offset);

    if (read_fs_block(root_inode.direct_blocks[0], dir_buf) != 0) {
        pmm_free_page(dir_phys);
        spinlock_release(&grahafs_lock);
        return -1;
    }

    grahafs_dirent_t* entries = (grahafs_dirent_t*)dir_buf;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    uint32_t count = 0;

    for (int i = 0; i < max_entries && count < (uint32_t)max_results; i++) {
        if (entries[i].inode_num == 0 || entries[i].name[0] == '\0') continue;

        // Skip . and ..
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;

        grahafs_inode_t file_inode;
        if (read_inode(entries[i].inode_num, &file_inode) != 0) continue;

        // Check if this inode has tags
        if (!(file_inode.ai_flags & GRAHAFS_AI_HAS_TAGS)) continue;

        if (grahafs_strstr(file_inode.ai_tags, tag)) {
            // Build path "/<name>"
            results->results[count].path[0] = '/';
            size_t name_len = strlen(entries[i].name);
            if (name_len > 254) name_len = 254;
            memcpy(results->results[count].path + 1, entries[i].name, name_len);
            results->results[count].path[1 + name_len] = '\0';

            results->results[count].inode_num = entries[i].inode_num;
            results->results[count].importance = file_inode.ai_importance;

            size_t tag_copy = strlen(file_inode.ai_tags);
            if (tag_copy > 95) tag_copy = 95;
            memcpy(results->results[count].tags, file_inode.ai_tags, tag_copy);
            results->results[count].tags[tag_copy] = '\0';

            count++;
        }
    }

    results->count = count;

    pmm_free_page(dir_phys);
    spinlock_release(&grahafs_lock);
    return (int)count;
}

// Phase 11a: Compute SimHash for a file and store in ai_embedding[0]
// Returns the 64-bit SimHash on success, 0 on failure
uint64_t grahafs_compute_simhash(uint32_t inode_num) {
    if (!fs_mounted) return 0;
    if (inode_num >= GRAHAFS_MAX_INODES) return 0;

    spinlock_acquire(&grahafs_lock);

    grahafs_inode_t inode;
    if (read_inode(inode_num, &inode) != 0) {
        spinlock_release(&grahafs_lock);
        return 0;
    }

    if (inode.type != GRAHAFS_INODE_TYPE_FILE || inode.size == 0) {
        spinlock_release(&grahafs_lock);
        return 0;
    }

    // Read file data into a temporary buffer (max 48KB = 12 direct blocks)
    size_t file_size = inode.size;
    size_t max_read = 12 * GRAHAFS_BLOCK_SIZE; // 48KB limit
    if (file_size > max_read) file_size = max_read;

    // Allocate pages for file data (up to 12 pages)
    size_t pages_needed = (file_size + GRAHAFS_BLOCK_SIZE - 1) / GRAHAFS_BLOCK_SIZE;
    void *data_phys = pmm_alloc_page();
    if (!data_phys) {
        spinlock_release(&grahafs_lock);
        return 0;
    }
    void *data_buf = (void *)((uint64_t)data_phys + g_hhdm_offset);

    // Read block by block, compute SimHash incrementally
    // For simplicity, read into a single page buffer one block at a time
    // and use a cumulative approach via simhash_auto on each block's data

    // Actually, for best results we need contiguous data for shingle hashing.
    // Since files are typically small, allocate enough pages.
    // But we only have pmm_alloc_page (one page at a time).
    // Solution: read one block at a time, concatenate into data_buf.
    // For files > 4KB, we need multiple pages.

    // Allocate a contiguous-enough buffer using multiple page allocations
    // For up to 48KB we need 12 pages
    void *page_phys[12];
    void *page_virt[12];
    size_t actual_pages = pages_needed > 12 ? 12 : pages_needed;

    // We already allocated one page, use it as block 0
    page_phys[0] = data_phys;
    page_virt[0] = data_buf;
    for (size_t p = 1; p < actual_pages; p++) {
        page_phys[p] = pmm_alloc_page();
        if (!page_phys[p]) {
            // Free previously allocated pages
            for (size_t q = 0; q < p; q++) pmm_free_page(page_phys[q]);
            spinlock_release(&grahafs_lock);
            return 0;
        }
        page_virt[p] = (void *)((uint64_t)page_phys[p] + g_hhdm_offset);
    }

    // Read file data block by block
    size_t bytes_read = 0;
    for (size_t b = 0; b < actual_pages && b < 12; b++) {
        if (inode.direct_blocks[b] == 0) break;
        if (read_fs_block(inode.direct_blocks[b], page_virt[b]) != 0) break;
        size_t chunk = GRAHAFS_BLOCK_SIZE;
        if (bytes_read + chunk > file_size) chunk = file_size - bytes_read;
        bytes_read += chunk;
    }

    // For SimHash, we need contiguous data. Copy into first pages sequentially.
    // Each page already has the block data in order, but they're not contiguous.
    // For files <= 4KB (one page), data is already contiguous.
    // For larger files, we compute per-block and combine.

    uint64_t hash;
    if (actual_pages == 1) {
        // Single page — data is contiguous
        hash = simhash_auto(page_virt[0], bytes_read);
    } else {
        // Multi-page: compute SimHash using accumulator approach
        // Process each page's data through the same accumulator
        int32_t v[64];
        for (int i = 0; i < 64; i++) v[i] = 0;

        for (size_t p = 0; p < actual_pages; p++) {
            size_t page_bytes = GRAHAFS_BLOCK_SIZE;
            size_t total_so_far = p * GRAHAFS_BLOCK_SIZE;
            if (total_so_far + page_bytes > bytes_read)
                page_bytes = bytes_read - total_so_far;
            if (page_bytes == 0) break;

            // Determine if text or binary from first page
            const uint8_t *bytes = (const uint8_t *)page_virt[p];

            // Generate shingle hashes within this page
            if (page_bytes >= SIMHASH_SHINGLE_SIZE) {
                size_t num_shingles = page_bytes - SIMHASH_SHINGLE_SIZE + 1;
                for (size_t s = 0; s < num_shingles; s++) {
                    uint64_t h = fnv1a_hash64(&bytes[s], SIMHASH_SHINGLE_SIZE);
                    for (int bit = 0; bit < 64; bit++) {
                        if (h & (1ULL << bit)) v[bit]++;
                        else v[bit]--;
                    }
                }
            }
        }

        hash = 0;
        for (int bit = 0; bit < 64; bit++) {
            if (v[bit] > 0) hash |= (1ULL << bit);
        }
    }

    // Free data pages
    for (size_t p = 0; p < actual_pages; p++) {
        pmm_free_page(page_phys[p]);
    }

    // Store SimHash in ai_embedding[0] via extended metadata block
    // Allocate extended block if not yet allocated
    if (inode.ai_metadata_block == 0) {
        uint32_t ext_block = allocate_block();
        if (ext_block == 0) {
            spinlock_release(&grahafs_lock);
            return hash; // Return hash but couldn't store it
        }
        inode.ai_metadata_block = ext_block;
        inode.ai_flags |= GRAHAFS_AI_HAS_EXTENDED;
    }

    // Read/init extended block
    void *ext_phys = pmm_alloc_page();
    if (!ext_phys) {
        spinlock_release(&grahafs_lock);
        return hash;
    }
    void *ext_buf = (void *)((uint64_t)ext_phys + g_hhdm_offset);

    grahafs_ai_metadata_block_t *ext_meta;
    if (read_fs_block(inode.ai_metadata_block, ext_buf) != 0) {
        memset(ext_buf, 0, GRAHAFS_BLOCK_SIZE);
    }
    ext_meta = (grahafs_ai_metadata_block_t *)ext_buf;

    if (ext_meta->magic != GRAHAFS_AI_META_MAGIC) {
        memset(ext_buf, 0, GRAHAFS_BLOCK_SIZE);
        ext_meta->magic = GRAHAFS_AI_META_MAGIC;
        ext_meta->version = 1;
    }

    // Store SimHash in embedding[0]
    ext_meta->embedding[0] = hash;
    if (ext_meta->embedding_dim < 1) ext_meta->embedding_dim = 1;
    inode.ai_flags |= GRAHAFS_AI_HAS_EMBEDDING;

    // Write back
    write_fs_block(inode.ai_metadata_block, ext_buf);
    pmm_free_page(ext_phys);

    // Phase 11b: Auto-cluster — look up filename from root dir, then assign
    {
        grahafs_inode_t root_in;
        char found_name[28];
        found_name[0] = '\0';
        if (read_inode(superblock.root_inode, &root_in) == 0 &&
            root_in.direct_blocks[0] != 0) {
            void *dir_phys = pmm_alloc_page();
            if (dir_phys) {
                void *dir_buf = (void *)((uint64_t)dir_phys + g_hhdm_offset);
                if (read_fs_block(root_in.direct_blocks[0], dir_buf) == 0) {
                    grahafs_dirent_t *entries = (grahafs_dirent_t *)dir_buf;
                    int max_ent = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
                    for (int i = 0; i < max_ent; i++) {
                        if (entries[i].inode_num == inode_num) {
                            strcpy(found_name, entries[i].name);
                            break;
                        }
                    }
                }
                pmm_free_page(dir_phys);
            }
        }
        if (found_name[0] != '\0') {
            uint32_t cid = cluster_assign(inode_num, hash, found_name);
            if (cid != 0) {
                // Store cluster_id in ai_reserved[0..3]
                *(uint32_t *)inode.ai_reserved = cid;
            }
        }
    }

    write_inode(inode_num, &inode);

    if (fs_device && fs_device->device_id >= 0) {
        ahci_flush_cache(fs_device->device_id);
    }

    spinlock_release(&grahafs_lock);
    return hash;
}

// Phase 11a: Find files similar to a given file (by SimHash Hamming distance)
// Returns number of matches found
int grahafs_find_similar(uint32_t ref_inode, int threshold,
                         grahafs_search_results_t *results, int max_results) {
    if (!results || !fs_mounted) return -1;
    if (threshold <= 0) threshold = SIMHASH_SIMILAR_THRESHOLD;
    if (max_results <= 0 || max_results > 16) max_results = 16;

    spinlock_acquire(&grahafs_lock);

    // Get the reference file's SimHash from its embedding
    grahafs_inode_t ref_in;
    if (read_inode(ref_inode, &ref_in) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    uint64_t ref_hash = 0;
    if (ref_in.ai_metadata_block != 0) {
        void *ext_phys = pmm_alloc_page();
        if (ext_phys) {
            void *ext_buf = (void *)((uint64_t)ext_phys + g_hhdm_offset);
            if (read_fs_block(ref_in.ai_metadata_block, ext_buf) == 0) {
                grahafs_ai_metadata_block_t *ext = (grahafs_ai_metadata_block_t *)ext_buf;
                if (ext->magic == GRAHAFS_AI_META_MAGIC && ext->embedding_dim >= 1)
                    ref_hash = ext->embedding[0];
            }
            pmm_free_page(ext_phys);
        }
    }

    if (ref_hash == 0) {
        spinlock_release(&grahafs_lock);
        return -2; // No SimHash computed for reference file
    }

    memset(results, 0, sizeof(grahafs_search_results_t));

    // Scan all files in root directory
    grahafs_inode_t root_inode;
    if (read_inode(superblock.root_inode, &root_inode) != 0) {
        spinlock_release(&grahafs_lock);
        return -1;
    }

    if (root_inode.direct_blocks[0] == 0) {
        spinlock_release(&grahafs_lock);
        return 0;
    }

    void *dir_phys = pmm_alloc_page();
    if (!dir_phys) {
        spinlock_release(&grahafs_lock);
        return -1;
    }
    void *dir_buf = (void *)((uint64_t)dir_phys + g_hhdm_offset);

    if (read_fs_block(root_inode.direct_blocks[0], dir_buf) != 0) {
        pmm_free_page(dir_phys);
        spinlock_release(&grahafs_lock);
        return -1;
    }

    void *ext_page_phys = pmm_alloc_page();
    if (!ext_page_phys) {
        pmm_free_page(dir_phys);
        spinlock_release(&grahafs_lock);
        return -1;
    }
    void *ext_page_buf = (void *)((uint64_t)ext_page_phys + g_hhdm_offset);

    grahafs_dirent_t *entries = (grahafs_dirent_t *)dir_buf;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    uint32_t count = 0;

    for (int i = 0; i < max_entries && count < (uint32_t)max_results; i++) {
        if (entries[i].inode_num == 0 || entries[i].name[0] == '\0') continue;
        if (entries[i].inode_num == ref_inode) continue; // Skip self

        // Skip . and ..
        if (entries[i].name[0] == '.' &&
            (entries[i].name[1] == '\0' ||
             (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;

        grahafs_inode_t file_inode;
        if (read_inode(entries[i].inode_num, &file_inode) != 0) continue;
        if (file_inode.type != GRAHAFS_INODE_TYPE_FILE) continue;

        // Get this file's SimHash
        if (file_inode.ai_metadata_block == 0) continue;
        if (!(file_inode.ai_flags & GRAHAFS_AI_HAS_EMBEDDING)) continue;

        if (read_fs_block(file_inode.ai_metadata_block, ext_page_buf) != 0) continue;
        grahafs_ai_metadata_block_t *ext = (grahafs_ai_metadata_block_t *)ext_page_buf;
        if (ext->magic != GRAHAFS_AI_META_MAGIC || ext->embedding_dim < 1) continue;

        uint64_t file_hash = ext->embedding[0];
        int dist = simhash_hamming_distance(ref_hash, file_hash);

        if (dist <= threshold) {
            // Match found
            results->results[count].path[0] = '/';
            size_t name_len = strlen(entries[i].name);
            if (name_len > 254) name_len = 254;
            memcpy(results->results[count].path + 1, entries[i].name, name_len);
            results->results[count].path[1 + name_len] = '\0';

            results->results[count].inode_num = entries[i].inode_num;
            results->results[count].importance = (uint32_t)dist; // Store distance in importance field

            size_t tag_copy = strlen(file_inode.ai_tags);
            if (tag_copy > 95) tag_copy = 95;
            memcpy(results->results[count].tags, file_inode.ai_tags, tag_copy);
            results->results[count].tags[tag_copy] = '\0';

            count++;
        }
    }

    results->count = count;

    pmm_free_page(ext_page_phys);
    pmm_free_page(dir_phys);
    spinlock_release(&grahafs_lock);
    return (int)count;
}

// Phase 11b: Rebuild cluster table from on-disk inodes at mount time.
// Must be called while grahafs_lock IS held (from grahafs_mount).
static void grahafs_cluster_rebuild_locked(void) {
    // cluster_init acquires its own lock (cluster_lock), which is fine
    cluster_init();

    grahafs_inode_t root_in;
    if (read_inode(superblock.root_inode, &root_in) != 0) return;
    if (root_in.direct_blocks[0] == 0) return;

    void *dir_phys = pmm_alloc_page();
    if (!dir_phys) return;
    void *dir_buf = (void *)((uint64_t)dir_phys + g_hhdm_offset);

    if (read_fs_block(root_in.direct_blocks[0], dir_buf) != 0) {
        pmm_free_page(dir_phys);
        return;
    }

    void *ext_phys = pmm_alloc_page();
    if (!ext_phys) {
        pmm_free_page(dir_phys);
        return;
    }
    void *ext_buf = (void *)((uint64_t)ext_phys + g_hhdm_offset);

    grahafs_dirent_t *entries = (grahafs_dirent_t *)dir_buf;
    int max_ent = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);

    for (int i = 0; i < max_ent; i++) {
        if (entries[i].inode_num == 0) continue;
        if (entries[i].name[0] == '.' && (entries[i].name[1] == '\0' ||
            (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
            continue;

        grahafs_inode_t file_inode;
        if (read_inode(entries[i].inode_num, &file_inode) != 0) continue;
        if (file_inode.type != GRAHAFS_INODE_TYPE_FILE) continue;
        if (!(file_inode.ai_flags & GRAHAFS_AI_HAS_EMBEDDING)) continue;
        if (file_inode.ai_metadata_block == 0) continue;

        // Extract cluster_id from ai_reserved[0..3]
        uint32_t cluster_id = *(uint32_t *)file_inode.ai_reserved;
        if (cluster_id == 0) continue;

        // Read extended metadata for SimHash
        if (read_fs_block(file_inode.ai_metadata_block, ext_buf) != 0) continue;
        grahafs_ai_metadata_block_t *ext = (grahafs_ai_metadata_block_t *)ext_buf;
        if (ext->magic != GRAHAFS_AI_META_MAGIC || ext->embedding_dim < 1) continue;

        uint64_t simhash = ext->embedding[0];
        cluster_rebuild_add(entries[i].inode_num, cluster_id, simhash, entries[i].name);
    }

    cluster_rebuild_finalize();

    pmm_free_page(ext_phys);
    pmm_free_page(dir_phys);
}

// Phase 11b: Background indexer task
// Periodically scans root directory for files without SimHash and auto-indexes them.
// Only processes files directly in root (not in subdirectories like /illu/, /bin/, /etc/).
extern volatile uint64_t g_timer_ticks;

void grahafs_indexer_task(void) {
    // Initial delay: 5 seconds (500 ticks at 100Hz)
    uint64_t start = g_timer_ticks;
    while (g_timer_ticks - start < 500) {
        asm volatile("hlt");
    }

    serial_write("[Indexer] Background indexer started\n");

    while (1) {
        // Sleep 3 seconds (300 ticks)
        uint64_t sleep_start = g_timer_ticks;
        while (g_timer_ticks - sleep_start < 300) {
            asm volatile("hlt");
        }

        if (!fs_mounted) continue;

        // Scan root dir for first file without HAS_EMBEDDING
        spinlock_acquire(&grahafs_lock);

        grahafs_inode_t root_in;
        if (read_inode(superblock.root_inode, &root_in) != 0) {
            spinlock_release(&grahafs_lock);
            continue;
        }
        if (root_in.direct_blocks[0] == 0) {
            spinlock_release(&grahafs_lock);
            continue;
        }

        void *dir_phys = pmm_alloc_page();
        if (!dir_phys) {
            spinlock_release(&grahafs_lock);
            continue;
        }
        void *dir_buf = (void *)((uint64_t)dir_phys + g_hhdm_offset);

        if (read_fs_block(root_in.direct_blocks[0], dir_buf) != 0) {
            pmm_free_page(dir_phys);
            spinlock_release(&grahafs_lock);
            continue;
        }

        grahafs_dirent_t *entries = (grahafs_dirent_t *)dir_buf;
        int max_ent = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
        uint32_t target_inode = 0;

        for (int i = 0; i < max_ent; i++) {
            if (entries[i].inode_num == 0) continue;
            if (entries[i].name[0] == '.' && (entries[i].name[1] == '\0' ||
                (entries[i].name[1] == '.' && entries[i].name[2] == '\0')))
                continue;

            grahafs_inode_t fnode;
            if (read_inode(entries[i].inode_num, &fnode) != 0) continue;
            // Only index regular files (not directories)
            if (fnode.type != GRAHAFS_INODE_TYPE_FILE) continue;
            if (fnode.size == 0) continue;
            if (fnode.ai_flags & GRAHAFS_AI_HAS_EMBEDDING) continue;

            target_inode = entries[i].inode_num;
            break;
        }

        pmm_free_page(dir_phys);
        spinlock_release(&grahafs_lock);

        if (target_inode != 0) {
            // Compute SimHash (this also auto-clusters via the hook)
            grahafs_compute_simhash(target_inode);
        }
    }
}