// scripts/mkfs.gfs.c
// Host tool to create a GrahaFS v2 filesystem on a disk image.
//
// Phase 19 complete rewrite. Produces:
//   * v2 superblock (magic 0x...22, CRC32-checked)
//   * empty free-block bitmap (metadata blocks pre-marked)
//   * v2 inode table (512-byte inodes, 16384 max, CRC32-checked)
//   * segment table (pointing at one initial ACTIVE segment)
//   * empty journal region (16384 blocks = 64 MB, zeroed)
//   * directory tree: /, /illu/, /bin/, /bin/tests/, /etc/, /var/, /var/audit/
//
// Compile (host): gcc -o mkfs.gfs scripts/mkfs.gfs.c kernel/lib/crc32.c -I.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "kernel/fs/grahafs_v2.h"
#include "kernel/lib/crc32.h"

// Old v1 directory entry layout (kept compatible — 32 bytes).
typedef struct {
    uint32_t inode_num;
    char     name[28];
} __attribute__((packed)) v2_dirent_t;
_Static_assert(sizeof(v2_dirent_t) == 32, "v2_dirent_t must be 32 bytes");

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void write_block(int fd, uint64_t block_num, const void *buf) {
    if (lseek(fd, (off_t)(block_num * GRAHAFS_V2_BLOCK_SIZE), SEEK_SET) < 0) die("lseek");
    if (write(fd, buf, GRAHAFS_V2_BLOCK_SIZE) != (ssize_t)GRAHAFS_V2_BLOCK_SIZE) die("write");
}

static void read_block(int fd, uint64_t block_num, void *buf) {
    if (lseek(fd, (off_t)(block_num * GRAHAFS_V2_BLOCK_SIZE), SEEK_SET) < 0) die("lseek");
    if (read(fd, buf, GRAHAFS_V2_BLOCK_SIZE) != (ssize_t)GRAHAFS_V2_BLOCK_SIZE) die("read");
}

static void bitmap_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

// Compute superblock CRC over the prefix ending at offset 152 (checksum_sb).
static uint32_t sb_checksum(const grahafs_v2_superblock_t *sb) {
    return crc32_buf(sb, offsetof(grahafs_v2_superblock_t, checksum_sb));
}

// Compute inode CRC over prefix ending at offset 368 (checksum_inode).
static uint32_t inode_checksum(const grahafs_v2_inode_t *ino) {
    return crc32_buf(ino, offsetof(grahafs_v2_inode_t, checksum_inode));
}

// Segment header CRC over prefix ending at offset 4092 (checksum_header).
static uint32_t segment_checksum(const grahafs_v2_segment_header_t *hdr) {
    return crc32_buf(hdr, offsetof(grahafs_v2_segment_header_t, checksum_header));
}

// Fill a v2 inode with directory defaults.
static void init_dir_inode(grahafs_v2_inode_t *ino, uint32_t first_data_block,
                           uint32_t dirent_count, uint64_t now_ns) {
    memset(ino, 0, sizeof(*ino));
    ino->magic              = GRAHAFS_V2_INODE_MAGIC;
    ino->type               = GRAHAFS_V2_TYPE_DIRECTORY;
    ino->link_count         = (uint16_t)dirent_count;  // ., .., and children.
    ino->mode               = 0755;
    ino->size               = (uint64_t)(dirent_count * sizeof(v2_dirent_t));
    ino->blocks_allocated   = 1;
    ino->creation_time      = now_ns;
    ino->modification_time  = now_ns;
    ino->access_time        = now_ns;
    ino->direct_blocks[0]   = first_data_block;
    ino->version_chain_head_id = 0;
    ino->version_count      = 0;
    ino->checksum_inode     = inode_checksum(ino);
}

// Write a small directory block: `.` + `..` + up to N children.
static void fill_dirents(v2_dirent_t *slot, uint32_t self_inode,
                         uint32_t parent_inode, const uint32_t *child_inodes,
                         const char *const *child_names, int n_children) {
    memset(slot, 0, GRAHAFS_V2_BLOCK_SIZE);
    slot[0].inode_num = self_inode;
    strncpy(slot[0].name, ".", sizeof(slot[0].name) - 1);
    slot[1].inode_num = parent_inode;
    strncpy(slot[1].name, "..", sizeof(slot[1].name) - 1);
    for (int i = 0; i < n_children; ++i) {
        slot[2 + i].inode_num = child_inodes[i];
        strncpy(slot[2 + i].name, child_names[i], sizeof(slot[2 + i].name) - 1);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_image>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) die("open disk_image");

    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat");
    uint64_t total_bytes = (uint64_t)st.st_size;
    uint32_t total_blocks = (uint32_t)(total_bytes / GRAHAFS_V2_BLOCK_SIZE);

    printf("=== GrahaFS v2 Formatter ===\n");
    printf("Image: %s  size=%llu MB  blocks=%u\n",
           argv[1], (unsigned long long)(total_bytes >> 20), total_blocks);

    // ---- Layout ----
    // Block 0                          : superblock
    // bitmap_start_block..             : free-block bitmap
    // inode_table_start_block..        : 16384 inodes @ 512 B = 8 MB = 2048 blocks
    // segment_table_start..            : 1 block (32 entries × ≤128 B each fits)
    // journal_start_block..            : 16384 blocks (64 MB)
    // data_blocks_start_block..        : Segment 0 starts here.
    // ------------------------------------------------------------------------

    uint32_t bitmap_blocks =
        (total_blocks + 8u * GRAHAFS_V2_BLOCK_SIZE - 1u) /
        (8u * GRAHAFS_V2_BLOCK_SIZE);

    uint32_t inode_table_blocks =
        (GRAHAFS_V2_INODE_COUNT_MAX * GRAHAFS_V2_INODE_SIZE +
         GRAHAFS_V2_BLOCK_SIZE - 1u) / GRAHAFS_V2_BLOCK_SIZE;

    uint32_t segment_table_blocks = 1u;  // Up to 32 segments fit in 4 KB.
    uint32_t journal_blocks       = GRAHAFS_V2_JOURNAL_BLOCKS;

    uint32_t bitmap_start         = 1u;
    uint32_t inode_table_start    = bitmap_start + bitmap_blocks;
    uint32_t segment_table_start  = inode_table_start + inode_table_blocks;
    uint32_t journal_start        = segment_table_start + segment_table_blocks;
    uint32_t data_blocks_start    = journal_start + journal_blocks;

    if (data_blocks_start + GRAHAFS_V2_SEGMENT_BLOCKS > total_blocks) {
        fprintf(stderr,
                "ERROR: disk too small. Need at least %llu bytes for "
                "metadata + journal + one segment (have %llu).\n",
                (unsigned long long)(((uint64_t)data_blocks_start +
                                      GRAHAFS_V2_SEGMENT_BLOCKS) *
                                     GRAHAFS_V2_BLOCK_SIZE),
                (unsigned long long)total_bytes);
        close(fd);
        return 1;
    }

    uint32_t segment_count_max =
        (total_blocks - data_blocks_start) / GRAHAFS_V2_SEGMENT_BLOCKS;
    if (segment_count_max > 32u) segment_count_max = 32u;  // Segment table fits 32.

    printf("\nLayout:\n");
    printf("  superblock       : block 0\n");
    printf("  bitmap           : %u..%u  (%u blocks)\n",
           bitmap_start, bitmap_start + bitmap_blocks - 1, bitmap_blocks);
    printf("  inode table      : %u..%u  (%u blocks, %u inodes)\n",
           inode_table_start, inode_table_start + inode_table_blocks - 1,
           inode_table_blocks, GRAHAFS_V2_INODE_COUNT_MAX);
    printf("  segment table    : %u..%u  (%u blocks, %u segments max)\n",
           segment_table_start,
           segment_table_start + segment_table_blocks - 1,
           segment_table_blocks, segment_count_max);
    printf("  journal          : %u..%u  (%u blocks = %u MB)\n",
           journal_start, journal_start + journal_blocks - 1,
           journal_blocks, (journal_blocks * GRAHAFS_V2_BLOCK_SIZE) >> 20);
    printf("  segments (data)  : %u..%u\n",
           data_blocks_start, total_blocks - 1);

    // ---- 1. Superblock ----
    grahafs_v2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic                    = GRAHAFS_V2_SB_MAGIC;
    sb.version                  = 2;
    sb.block_size               = GRAHAFS_V2_BLOCK_SIZE;
    sb.total_blocks             = total_blocks;
    sb.bitmap_start_block       = bitmap_start;
    sb.bitmap_blocks            = bitmap_blocks;
    sb.inode_table_start_block  = inode_table_start;
    sb.inode_table_blocks       = inode_table_blocks;
    sb.inode_count_max          = GRAHAFS_V2_INODE_COUNT_MAX;
    sb.data_blocks_start_block  = data_blocks_start;
    sb.root_inode               = 1;
    sb.segment_table_start      = segment_table_start;
    sb.segment_table_blocks     = segment_table_blocks;
    sb.segment_count_max        = segment_count_max;
    sb.journal_start_block      = journal_start;
    sb.journal_blocks           = journal_blocks;
    sb.journal_head_block       = journal_start;
    sb.journal_tail_block       = journal_start;
    sb.last_txn_id              = 0;
    sb.fs_flags                 = 0;
    strncpy(sb.fs_label, "graha-v2", sizeof(sb.fs_label) - 1);

    // Inodes used at format time:
    //   0 = reserved sentinel
    //   1 = /
    //   2 = /illu/
    //   3 = /bin/
    //   4 = /bin/tests/
    //   5 = /etc/
    //   6 = /var/
    //   7 = /var/audit/
    // Data blocks used at format time: one block per directory = 7 blocks.
    // First data block = data_blocks_start + 1 (block 0 of segment 0 is the
    // segment header).
    uint32_t inodes_used       = 8u;          // 0..7 (0 is reserved).
    uint32_t data_blocks_used  = 7u;          // Seven directory payloads.
    sb.free_inodes             = GRAHAFS_V2_INODE_COUNT_MAX - inodes_used;
    sb.free_blocks             = total_blocks - data_blocks_start - 1u
                                 - data_blocks_used;
    sb.checksum_sb             = sb_checksum(&sb);

    // ---- 2. Bitmap ----
    uint8_t *bitmap = calloc(bitmap_blocks, GRAHAFS_V2_BLOCK_SIZE);
    if (!bitmap) die("calloc bitmap");
    // Mark all metadata blocks (through journal end) plus segment 0's header +
    // the 7 directory data blocks that immediately follow it.
    for (uint32_t b = 0; b < data_blocks_start + 1u + data_blocks_used; ++b) {
        bitmap_set(bitmap, b);
    }

    // ---- 3. Inode table ----
    grahafs_v2_inode_t *inodes =
        calloc(inode_table_blocks, GRAHAFS_V2_BLOCK_SIZE);
    if (!inodes) die("calloc inodes");

    uint64_t now_ns = (uint64_t)time(NULL) * 1000000000ULL;

    //   Segment 0 starts at data_blocks_start and the header consumes block 0
    //   of that segment (which is data_blocks_start itself). The seven
    //   directories sit in blocks data_blocks_start+1 .. +7.
    uint32_t dir_block_root     = data_blocks_start + 1u;
    uint32_t dir_block_illu     = data_blocks_start + 2u;
    uint32_t dir_block_bin      = data_blocks_start + 3u;
    uint32_t dir_block_bintests = data_blocks_start + 4u;
    uint32_t dir_block_etc      = data_blocks_start + 5u;
    uint32_t dir_block_var      = data_blocks_start + 6u;
    uint32_t dir_block_audit    = data_blocks_start + 7u;

    // / has 5 children: illu, bin, etc, var (size = 2 + 4 = 6 dirents).
    init_dir_inode(&inodes[1], dir_block_root, 6, now_ns);
    // /illu/ empty (2 dirents).
    init_dir_inode(&inodes[2], dir_block_illu, 2, now_ns);
    // /bin/ has 1 child (tests).
    init_dir_inode(&inodes[3], dir_block_bin, 3, now_ns);
    // /bin/tests/ empty.
    init_dir_inode(&inodes[4], dir_block_bintests, 2, now_ns);
    // /etc/ empty (populated at runtime with gcp.json etc — Phase 19 wires later).
    init_dir_inode(&inodes[5], dir_block_etc, 2, now_ns);
    // /var/ has 1 child (audit).
    init_dir_inode(&inodes[6], dir_block_var, 3, now_ns);
    // /var/audit/ empty.
    init_dir_inode(&inodes[7], dir_block_audit, 2, now_ns);

    // ---- 4. Segment table + segment 0 header ----
    // Segment table block: 32 entries × sizeof(segment table entry). We use a
    // minimal 16-byte entry: {first_block:u64, state:u8, _pad[7]}. That fits
    // 256 entries per block — plenty.
    uint8_t segment_table[GRAHAFS_V2_BLOCK_SIZE];
    memset(segment_table, 0, sizeof(segment_table));
    uint64_t *seg_entries = (uint64_t *)segment_table;
    seg_entries[0] = data_blocks_start;  // first_block of segment 0.
    // State byte is carried in the segment header on-disk, not the table.

    // Segment 0 header (at data_blocks_start).
    grahafs_v2_segment_header_t seg0_hdr;
    memset(&seg0_hdr, 0, sizeof(seg0_hdr));
    seg0_hdr.magic                     = GRAHAFS_V2_SEGMENT_MAGIC;
    seg0_hdr.segment_id                = 0;
    seg0_hdr.creation_txn              = 0;
    seg0_hdr.size_blocks               = GRAHAFS_V2_SEGMENT_BLOCKS;
    seg0_hdr.state                     = GRAHAFS_V2_SEG_ACTIVE;
    seg0_hdr.refcount                  = 0;  // No version records yet.
    seg0_hdr.first_version_offset      = 0;
    seg0_hdr.version_record_count      = 0;
    seg0_hdr.free_bytes_in_segment     =
        (GRAHAFS_V2_SEGMENT_BLOCKS - 1u - 7u) * GRAHAFS_V2_BLOCK_SIZE;
    seg0_hdr.next_free_block_offset    = (1u + 7u) * GRAHAFS_V2_BLOCK_SIZE;
    seg0_hdr.checksum_header           = segment_checksum(&seg0_hdr);

    // ---- 5. Directory data blocks ----
    v2_dirent_t root_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t illu_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t bin_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t bintests_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t etc_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t var_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];
    v2_dirent_t audit_dir[GRAHAFS_V2_BLOCK_SIZE / sizeof(v2_dirent_t)];

    uint32_t root_children_ino[]  = { 2, 3, 5, 6 };
    const char *root_children_name[] = { "illu", "bin", "etc", "var" };
    fill_dirents(root_dir, 1, 1, root_children_ino, root_children_name, 4);

    fill_dirents(illu_dir,     2, 1, NULL, NULL, 0);

    uint32_t bin_children_ino[] = { 4 };
    const char *bin_children_name[] = { "tests" };
    fill_dirents(bin_dir,      3, 1, bin_children_ino, bin_children_name, 1);

    fill_dirents(bintests_dir, 4, 3, NULL, NULL, 0);
    fill_dirents(etc_dir,      5, 1, NULL, NULL, 0);

    uint32_t var_children_ino[] = { 7 };
    const char *var_children_name[] = { "audit" };
    fill_dirents(var_dir,      6, 1, var_children_ino, var_children_name, 1);

    fill_dirents(audit_dir,    7, 6, NULL, NULL, 0);

    // ---- 6. Write to disk ----
    printf("\nWriting filesystem...\n");
    write_block(fd, 0, &sb);

    for (uint32_t i = 0; i < bitmap_blocks; ++i) {
        write_block(fd, bitmap_start + i,
                    bitmap + (size_t)i * GRAHAFS_V2_BLOCK_SIZE);
    }

    for (uint32_t i = 0; i < inode_table_blocks; ++i) {
        write_block(fd, inode_table_start + i,
                    (uint8_t *)inodes + (size_t)i * GRAHAFS_V2_BLOCK_SIZE);
    }

    write_block(fd, segment_table_start, segment_table);

    // Zero out journal region (head == tail means empty).
    uint8_t zero_block[GRAHAFS_V2_BLOCK_SIZE];
    memset(zero_block, 0, sizeof(zero_block));
    for (uint32_t i = 0; i < journal_blocks; ++i) {
        write_block(fd, journal_start + i, zero_block);
    }

    // Write segment 0 header at data_blocks_start.
    write_block(fd, data_blocks_start, &seg0_hdr);

    // Write directory data blocks.
    write_block(fd, dir_block_root,     root_dir);
    write_block(fd, dir_block_illu,     illu_dir);
    write_block(fd, dir_block_bin,      bin_dir);
    write_block(fd, dir_block_bintests, bintests_dir);
    write_block(fd, dir_block_etc,      etc_dir);
    write_block(fd, dir_block_var,      var_dir);
    write_block(fd, dir_block_audit,    audit_dir);

    // ---- 7. Verify ----
    printf("\nVerifying...\n");
    grahafs_v2_superblock_t vsb;
    read_block(fd, 0, &vsb);
    if (vsb.magic != GRAHAFS_V2_SB_MAGIC) {
        fprintf(stderr, "ERROR: superblock magic mismatch on readback\n");
        free(bitmap); free(inodes); close(fd);
        return 1;
    }
    uint32_t expected_crc = vsb.checksum_sb;
    vsb.checksum_sb = 0;
    // (Not strictly needed to zero — we computed crc over [0..152) originally;
    //  re-run on what we read back).
    vsb.checksum_sb = expected_crc;
    if (sb_checksum(&vsb) != expected_crc) {
        fprintf(stderr, "ERROR: superblock CRC mismatch on readback\n");
        free(bitmap); free(inodes); close(fd);
        return 1;
    }
    printf("  ✓ superblock verified (magic=0x%016llx version=%u CRC=0x%08x)\n",
           (unsigned long long)vsb.magic, vsb.version, expected_crc);

    grahafs_v2_inode_t vino;
    if (pread(fd, &vino, sizeof(vino),
              (off_t)inode_table_start * GRAHAFS_V2_BLOCK_SIZE +
              1u * GRAHAFS_V2_INODE_SIZE) != (ssize_t)sizeof(vino)) {
        die("pread root inode");
    }
    if (vino.magic != GRAHAFS_V2_INODE_MAGIC) {
        fprintf(stderr, "ERROR: root inode magic mismatch\n");
        free(bitmap); free(inodes); close(fd);
        return 1;
    }
    printf("  ✓ root inode verified (type=%u size=%llu CRC=0x%08x)\n",
           vino.type, (unsigned long long)vino.size, vino.checksum_inode);

    printf("\n✓ GrahaFS v2 formatted. Ready for mount.\n");

    free(bitmap);
    free(inodes);
    close(fd);
    return 0;
}
