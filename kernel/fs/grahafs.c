// kernel/fs/grahafs.c
#include "grahafs.h"
#include "vfs.h"
#include "../../arch/x86_64/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../drivers/video/framebuffer.h"
#include "../sync/spinlock.h"

// For now, we'll have one mounted instance of GrahaFS
static block_device_t* fs_device = NULL;
static grahafs_superblock_t superblock;
static uint8_t* free_space_bitmap = NULL;
static spinlock_t grahafs_lock = SPINLOCK_INITIALIZER("grahafs");

// Memory utility functions
static void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
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

// Debug helper to print a number
static void debug_print_num(const char* prefix, uint32_t num, int x, int y, uint32_t color) {
    char msg[80];
    int pos = 0;
    while (*prefix) {
        msg[pos++] = *prefix++;
    }
    
    if (num == 0) {
        msg[pos++] = '0';
    } else {
        char digits[10];
        int digit_count = 0;
        uint32_t n = num;
        while (n > 0) {
            digits[digit_count++] = '0' + (n % 10);
            n /= 10;
        }
        while (digit_count > 0) {
            msg[pos++] = digits[--digit_count];
        }
    }
    msg[pos] = '\0';
    framebuffer_draw_string(msg, x, y, color, 0x00101828);
}

// Helper to read a block from the device
static int read_fs_block(uint32_t block_num, void* buf) {
    if (!fs_device) {
        framebuffer_draw_string("read_fs_block: No device!", 400, 400, COLOR_RED, 0x00101828);
        return -1;
    }
    
    // Debug: Show what block we're reading
    debug_print_num("Reading block: ", block_num, 400, 420, COLOR_YELLOW);
    
    int result = fs_device->read_blocks(fs_device->device_id, block_num, 1, buf);
    
    if (result != 0) {
        framebuffer_draw_string("read_fs_block: Read failed!", 400, 440, COLOR_RED, 0x00101828);
    } else {
        framebuffer_draw_string("read_fs_block: Success", 400, 440, COLOR_GREEN, 0x00101828);
    }
    
    return result;
}

// Helper to write a block to the device
static int write_fs_block(uint32_t block_num, void* buf) {
    if (!fs_device) return -1;
    return fs_device->write_blocks(fs_device->device_id, block_num, 1, buf);
}

// Helper to read an inode from the inode table
static int read_inode(uint32_t inode_num, grahafs_inode_t* inode) {
    if (inode_num >= GRAHAFS_MAX_INODES) {
        debug_print_num("read_inode: Invalid inode: ", inode_num, 400, 460, COLOR_RED);
        return -1;
    }

    // Calculate which block contains this inode
    uint32_t bytes_offset = inode_num * sizeof(grahafs_inode_t);
    uint32_t block = superblock.inode_table_start_block + (bytes_offset / GRAHAFS_BLOCK_SIZE);
    uint32_t offset = bytes_offset % GRAHAFS_BLOCK_SIZE;

    debug_print_num("read_inode: inode_num=", inode_num, 400, 480, COLOR_CYAN);
    debug_print_num("  inode table starts at block: ", superblock.inode_table_start_block, 400, 500, COLOR_CYAN);
    debug_print_num("  reading from block: ", block, 400, 520, COLOR_CYAN);
    debug_print_num("  at offset: ", offset, 400, 540, COLOR_CYAN);

    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) {
        framebuffer_draw_string("read_inode: Alloc failed!", 400, 560, COLOR_RED, 0x00101828);
        return -1;
    }
    
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(block, buffer) != 0) {
        framebuffer_draw_string("read_inode: Block read failed!", 400, 580, COLOR_RED, 0x00101828);
        pmm_free_page(buffer_phys);
        return -1;
    }

    memcpy(inode, (uint8_t*)buffer + offset, sizeof(grahafs_inode_t));
    
    // Debug: Print inode contents
    debug_print_num("  inode type: ", inode->type, 400, 600, COLOR_YELLOW);
    debug_print_num("  inode size: ", (uint32_t)inode->size, 400, 620, COLOR_YELLOW);
    debug_print_num("  first direct block: ", inode->direct_blocks[0], 400, 640, COLOR_YELLOW);
    
    pmm_free_page(buffer_phys);
    return 0;
}

// Helper to write an inode to the inode table
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

// VFS operations implementation
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
        return 0; // EOF
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
    
    // For now, implement simple in-place write (not CoW yet)
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
        // If block doesn't exist, allocate it (simplified - would need bitmap management)
        if (inode.direct_blocks[block_index] == 0) {
            // For now, just fail if we need to allocate new blocks
            break;
        }
        
        // Read existing block
        if (read_fs_block(inode.direct_blocks[block_index], temp_buffer) != 0) {
            pmm_free_page(temp_buffer_phys);
            spinlock_release(&grahafs_lock);
            return -1;
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
    
    // Update inode size if we extended the file
    if (offset + bytes_written > inode.size) {
        inode.size = offset + bytes_written;
        write_inode(node->inode, &inode);
    }
    
    pmm_free_page(temp_buffer_phys);
    spinlock_release(&grahafs_lock);
    return bytes_written;
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
    
    // Read directory entries from the first direct block
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
        // Check if entry is empty by looking at the name (inode 0 means empty)
        if (entries[i].inode_num == 0) continue;  // Skip truly empty entries
        if (entries[i].name[0] == '\0') continue;  // Skip entries with no name
        
        if (strcmp(entries[i].name, name) == 0) {
            // Found the entry, create a VFS node for it
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
    if (!node) {
        framebuffer_draw_string("readdir: NULL node!", 400, 660, COLOR_RED, 0x00101828);
        return NULL;
    }
    
    spinlock_acquire(&grahafs_lock);
    
    debug_print_num("readdir: Reading inode ", node->inode, 400, 680, COLOR_CYAN);
    
    grahafs_inode_t inode;
    if (read_inode(node->inode, &inode) != 0) {
        framebuffer_draw_string("readdir: Failed to read inode!", 700, 600, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    if (inode.type != GRAHAFS_INODE_TYPE_DIRECTORY) {
        framebuffer_draw_string("readdir: Not a directory!", 700, 620, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    // For simplicity, only support first block of directory entries
    if (inode.direct_blocks[0] == 0) {
        framebuffer_draw_string("readdir: No direct blocks!", 700, 640, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    debug_print_num("readdir: Reading dir block ", inode.direct_blocks[0], 700, 660, COLOR_CYAN);
    
    void* buffer_phys = pmm_alloc_page();
    if (!buffer_phys) {
        framebuffer_draw_string("readdir: Alloc failed!", 700, 680, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* buffer = (void*)((uint64_t)buffer_phys + g_hhdm_offset);
    
    if (read_fs_block(inode.direct_blocks[0], buffer) != 0) {
        framebuffer_draw_string("readdir: Block read failed!", 700, 700, COLOR_RED, 0x00101828);
        pmm_free_page(buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    grahafs_dirent_t* entries = (grahafs_dirent_t*)buffer;
    int max_entries = GRAHAFS_BLOCK_SIZE / sizeof(grahafs_dirent_t);
    
    // Debug: Show first few entries
    framebuffer_draw_string("readdir: Directory entries:", 700, 720, COLOR_YELLOW, 0x00101828);
    for (int i = 0; i < 5 && i < max_entries; i++) {
        char msg[80] = "  [";
        int p = 3;
        msg[p++] = '0' + i;
        msg[p++] = ']';
        msg[p++] = ' ';
        msg[p++] = 'i';
        msg[p++] = 'n';
        msg[p++] = 'o';
        msg[p++] = 'd';
        msg[p++] = 'e';
        msg[p++] = ':';
        msg[p++] = ' ';
        
        uint32_t inum = entries[i].inode_num;
        if (inum == 0) {
            msg[p++] = '0';
        } else {
            char digits[10];
            int dc = 0;
            while (inum > 0) {
                digits[dc++] = '0' + (inum % 10);
                inum /= 10;
            }
            while (dc > 0) {
                msg[p++] = digits[--dc];
            }
        }
        msg[p++] = ',';
        msg[p++] = ' ';
        msg[p++] = 'n';
        msg[p++] = 'a';
        msg[p++] = 'm';
        msg[p++] = 'e';
        msg[p++] = ':';
        msg[p++] = ' ';
        
        for (int j = 0; j < 10 && entries[i].name[j]; j++) {
            msg[p++] = entries[i].name[j];
        }
        msg[p] = '\0';
        
        framebuffer_draw_string(msg, 700, 740 + (i * 20), COLOR_CYAN, 0x00101828);
    }
    
    uint32_t current_index = 0;
    for (int i = 0; i < max_entries; i++) {
        // Check if entry is valid (inode 0 means empty)
        if (entries[i].inode_num == 0) continue;  // Skip empty entries
        if (entries[i].name[0] == '\0') continue;  // Skip entries with no name
        
        if (current_index == index) {
            debug_print_num("readdir: Found entry at index ", index, 700, 740, COLOR_GREEN);
            
            // Found the entry at the requested index
            grahafs_inode_t found_inode;
            if (read_inode(entries[i].inode_num, &found_inode) != 0) {
                framebuffer_draw_string("readdir: Failed to read entry inode!", 700, 760, COLOR_RED, 0x00101828);
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
                result->fs = (vfs_filesystem_t*)fs_device;
            }
            
            pmm_free_page(buffer_phys);
            spinlock_release(&grahafs_lock);
            return result;
        }
        current_index++;
    }
    
    debug_print_num("readdir: No entry at index ", index, 700, 780, COLOR_YELLOW);
    pmm_free_page(buffer_phys);
    spinlock_release(&grahafs_lock);
    return NULL;
}

void grahafs_init(void) {
    spinlock_init(&grahafs_lock, "grahafs");
    framebuffer_draw_string("GrahaFS: Driver initialized.", 10, 650, COLOR_GREEN, 0x00101828);
}

vfs_node_t* grahafs_mount(block_device_t* device) {
    if (!device) return NULL;
    
    spinlock_acquire(&grahafs_lock);
    fs_device = device;

    // Allocate a buffer for the superblock
    void* sb_buffer_phys = pmm_alloc_page();
    if (!sb_buffer_phys) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    void* sb_buffer = (void*)((uint64_t)sb_buffer_phys + g_hhdm_offset);

    // Read superblock (LBA 0)
    if (read_fs_block(0, sb_buffer) != 0) {
        framebuffer_draw_string("GrahaFS: Failed to read superblock.", 10, 750, COLOR_RED, 0x00101828);
        pmm_free_page(sb_buffer_phys);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    memcpy(&superblock, sb_buffer, sizeof(grahafs_superblock_t));
    pmm_free_page(sb_buffer_phys);

    // Verify magic number
    if (superblock.magic != GRAHAFS_MAGIC) {
        char msg[64] = "GrahaFS: Invalid magic: 0x";
        uint64_t magic = superblock.magic;
        for (int i = 0; i < 16; i++) {
            char hex = "0123456789ABCDEF"[(magic >> (60 - i * 4)) & 0xF];
            msg[27 + i] = hex;
        }
        msg[43] = '\0';
        framebuffer_draw_string(msg, 10, 750, COLOR_RED, 0x00101828);
        spinlock_release(&grahafs_lock);
        return NULL;
    }

    framebuffer_draw_string("GrahaFS: Filesystem mounted successfully!", 10, 750, COLOR_GREEN, 0x00101828);

    // Debug: Print filesystem layout
    debug_print_num("GrahaFS: Total blocks: ", superblock.total_blocks, 500, 570, COLOR_CYAN);
    debug_print_num("GrahaFS: Inode table at: ", superblock.inode_table_start_block, 500, 590, COLOR_CYAN);
    debug_print_num("GrahaFS: Data blocks at: ", superblock.data_blocks_start_block, 500, 610, COLOR_CYAN);
    debug_print_num("GrahaFS: Root inode: ", superblock.root_inode, 500, 630, COLOR_CYAN);

    // Create the root VFS node
    vfs_node_t* root = vfs_create_node("/", VFS_DIRECTORY);
    if (!root) {
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    // Read the root inode to get its information
    grahafs_inode_t root_inode;
    if (read_inode(superblock.root_inode, &root_inode) == 0) {
        root->inode = superblock.root_inode;
        root->size = root_inode.size;
        root->read = grahafs_read;
        root->write = grahafs_write;
        root->finddir = grahafs_finddir;
        root->readdir = grahafs_readdir;
        root->fs = (vfs_filesystem_t*)device;
        
        framebuffer_draw_string("GrahaFS: Root node created successfully", 500, 650, COLOR_GREEN, 0x00101828);
        debug_print_num("  Root size: ", (uint32_t)root_inode.size, 500, 670, COLOR_CYAN);
        debug_print_num("  Root type: ", root_inode.type, 500, 690, COLOR_CYAN);
    } else {
        framebuffer_draw_string("GrahaFS: Failed to read root inode!", 500, 650, COLOR_RED, 0x00101828);
        vfs_destroy_node(root);
        spinlock_release(&grahafs_lock);
        return NULL;
    }
    
    // Set as VFS root
    vfs_set_root(root);
    
    spinlock_release(&grahafs_lock);
    return root;
}

int grahafs_unmount(vfs_node_t* root) {
    if (!root) return -1;
    
    spinlock_acquire(&grahafs_lock);
    
    // Clear the root
    vfs_set_root(NULL);
    
    // Destroy the root node
    vfs_destroy_node(root);
    
    // Clear device reference
    fs_device = NULL;
    
    spinlock_release(&grahafs_lock);
    return 0;
}