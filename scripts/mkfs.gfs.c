// scripts/mkfs.gfs.c
// Host tool to create a GrahaFS filesystem on a disk image.
// Compile with: gcc -o mkfs.gfs mkfs.gfs.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// Include the filesystem header
#include "../kernel/fs/grahafs.h"

// Helper to write a block to the disk image
void write_block(int fd, uint32_t block_num, void* buffer) {
    lseek(fd, block_num * GRAHAFS_BLOCK_SIZE, SEEK_SET);
    if (write(fd, buffer, GRAHAFS_BLOCK_SIZE) != GRAHAFS_BLOCK_SIZE) {
        perror("Failed to write block");
        exit(1);
    }
}

// Helper to read a block from the disk image
void read_block(int fd, uint32_t block_num, void* buffer) {
    lseek(fd, block_num * GRAHAFS_BLOCK_SIZE, SEEK_SET);
    if (read(fd, buffer, GRAHAFS_BLOCK_SIZE) != GRAHAFS_BLOCK_SIZE) {
        perror("Failed to read block");
        exit(1);
    }
}

// Bitmap manipulation
void bitmap_set(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

void bitmap_clear(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

int bitmap_test(uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_image>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("Failed to open disk image");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    uint32_t total_blocks = st.st_size / GRAHAFS_BLOCK_SIZE;

    printf("=== GrahaFS Formatter ===\n");
    printf("Disk image: %s\n", argv[1]);
    printf("Size: %ld bytes (%u blocks of %d bytes)\n", 
           st.st_size, total_blocks, GRAHAFS_BLOCK_SIZE);

    // --- 1. Prepare Superblock ---
    grahafs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = GRAHAFS_MAGIC;
    sb.total_blocks = total_blocks;
    sb.bitmap_start_block = 1;
    
    // Calculate bitmap size (one bit per block)
    uint32_t bitmap_blocks = (total_blocks + 8 * GRAHAFS_BLOCK_SIZE - 1) / (8 * GRAHAFS_BLOCK_SIZE);
    sb.inode_table_start_block = sb.bitmap_start_block + bitmap_blocks;
    
    // Calculate inode table size
    uint32_t inode_table_blocks = (GRAHAFS_MAX_INODES * sizeof(grahafs_inode_t) + GRAHAFS_BLOCK_SIZE - 1) / GRAHAFS_BLOCK_SIZE;
    sb.data_blocks_start_block = sb.inode_table_start_block + inode_table_blocks;
    sb.root_inode = 1;  // CHANGED: Root is now inode 1, not 0
    sb.free_blocks = total_blocks - sb.data_blocks_start_block - 1; // -1 for root dir
    sb.free_inodes = GRAHAFS_MAX_INODES - 2; // -2: one reserved (0) and one for root (1)

    printf("\nFilesystem Layout:\n");
    printf("  Superblock: block 0\n");
    printf("  Bitmap: blocks %u-%u (%u blocks)\n", 
           sb.bitmap_start_block, sb.bitmap_start_block + bitmap_blocks - 1, bitmap_blocks);
    printf("  Inode table: blocks %u-%u (%u blocks)\n", 
           sb.inode_table_start_block, sb.inode_table_start_block + inode_table_blocks - 1, inode_table_blocks);
    printf("  Data blocks: blocks %u-%u\n", 
           sb.data_blocks_start_block, total_blocks - 1);
    printf("  Root inode: %u\n", sb.root_inode);  // Added for clarity

    // --- 2. Prepare Bitmap ---
    uint8_t* bitmap = calloc(bitmap_blocks, GRAHAFS_BLOCK_SIZE);
    if (!bitmap) {
        fprintf(stderr, "Failed to allocate memory for bitmap\n");
        close(fd);
        return 1;
    }

    // Mark all metadata blocks as used
    for (uint32_t i = 0; i < sb.data_blocks_start_block; i++) {
        bitmap_set(bitmap, i);
    }
    
    // Mark first data block as used (for root directory)
    bitmap_set(bitmap, sb.data_blocks_start_block);

    // --- 3. Prepare Inode Table ---
    grahafs_inode_t* inode_table = calloc(inode_table_blocks, GRAHAFS_BLOCK_SIZE);
    if (!inode_table) {
        fprintf(stderr, "Failed to allocate memory for inode table\n");
        free(bitmap);
        close(fd);
        return 1;
    }

    // Inode 0 is reserved/unused - leave it zeroed

    // Configure root inode (inode 1)  // CHANGED: Now using inode 1
    inode_table[1].type = GRAHAFS_INODE_TYPE_DIRECTORY;
    inode_table[1].size = sizeof(grahafs_dirent_t) * 2; // . and .. entries
    inode_table[1].link_count = 2;  // . and .. both link to root
    inode_table[1].uid = 0;
    inode_table[1].gid = 0;
    inode_table[1].mode = 0755;
    inode_table[1].creation_time = time(NULL);
    inode_table[1].modification_time = inode_table[1].creation_time;
    inode_table[1].access_time = inode_table[1].creation_time;
    
    // Root directory data is in the first data block
    inode_table[1].direct_blocks[0] = sb.data_blocks_start_block;
    for (int i = 1; i < 12; i++) {
        inode_table[1].direct_blocks[i] = 0;
    }
    inode_table[1].indirect_block = 0;
    inode_table[1].double_indirect = 0;

    // --- 4. Prepare Root Directory ---
    grahafs_dirent_t* root_dir = calloc(1, GRAHAFS_BLOCK_SIZE);
    if (!root_dir) {
        fprintf(stderr, "Failed to allocate memory for root directory\n");
        free(bitmap);
        free(inode_table);
        close(fd);
        return 1;
    }

    // Add . and .. entries
    root_dir[0].inode_num = 1;  // CHANGED: . points to inode 1 (root)
    strcpy(root_dir[0].name, ".");
    
    root_dir[1].inode_num = 1;  // CHANGED: .. also points to inode 1 (no parent)
    strcpy(root_dir[1].name, "..");

    // --- 5. Write everything to disk ---
    printf("\nWriting filesystem structures...\n");
    
    // Write superblock
    printf("  Writing superblock...\n");
    write_block(fd, 0, &sb);

    // Write bitmap
    printf("  Writing bitmap (%u blocks)...\n", bitmap_blocks);
    for (uint32_t i = 0; i < bitmap_blocks; i++) {
        write_block(fd, sb.bitmap_start_block + i, bitmap + (i * GRAHAFS_BLOCK_SIZE));
    }

    // Write inode table
    printf("  Writing inode table (%u blocks)...\n", inode_table_blocks);
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        write_block(fd, sb.inode_table_start_block + i, 
                   (uint8_t*)inode_table + (i * GRAHAFS_BLOCK_SIZE));
    }
    
    // Write root directory data block
    printf("  Writing root directory...\n");
    write_block(fd, sb.data_blocks_start_block, root_dir);

    // --- 6. Verify the filesystem ---
    printf("\nVerifying filesystem...\n");
    
    // Read back superblock
    grahafs_superblock_t verify_sb;
    read_block(fd, 0, &verify_sb);
    if (verify_sb.magic != GRAHAFS_MAGIC) {
        fprintf(stderr, "ERROR: Superblock verification failed!\n");
        free(bitmap);
        free(inode_table);
        free(root_dir);
        close(fd);
        return 1;
    }
    printf("  ✓ Superblock verified (magic: 0x%lX)\n", verify_sb.magic);
    printf("  ✓ Root inode number: %u\n", verify_sb.root_inode);
    
    // Read back root inode (now at index 1)
    grahafs_inode_t* verify_inode_table = malloc(GRAHAFS_BLOCK_SIZE);
    read_block(fd, sb.inode_table_start_block, verify_inode_table);
    if (verify_inode_table[1].type != GRAHAFS_INODE_TYPE_DIRECTORY) {
        fprintf(stderr, "ERROR: Root inode verification failed!\n");
        free(bitmap);
        free(inode_table);
        free(root_dir);
        free(verify_inode_table);
        close(fd);
        return 1;
    }
    printf("  ✓ Root inode verified (type: directory, size: %lu bytes)\n", verify_inode_table[1].size);
    
    // Verify root directory entries
    grahafs_dirent_t* verify_root_dir = malloc(GRAHAFS_BLOCK_SIZE);
    read_block(fd, sb.data_blocks_start_block, verify_root_dir);
    printf("  ✓ Root directory entries:\n");
    printf("    - '%s' -> inode %u\n", verify_root_dir[0].name, verify_root_dir[0].inode_num);
    printf("    - '%s' -> inode %u\n", verify_root_dir[1].name, verify_root_dir[1].inode_num);
    
    free(verify_inode_table);
    free(verify_root_dir);

    printf("\n✓ GrahaFS filesystem created successfully!\n");
    printf("  Total blocks: %u\n", total_blocks);
    printf("  Free blocks: %u\n", sb.free_blocks);
    printf("  Free inodes: %u\n", sb.free_inodes);

    free(bitmap);
    free(inode_table);
    free(root_dir);
    close(fd);
    return 0;
}