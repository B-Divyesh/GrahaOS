// kernel/fs/vfs.h
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../sync/spinlock.h"

#define MAX_OPEN_FILES 64
#define MAX_BLOCK_DEVICES 8
#define MAX_FILESYSTEMS 4
#define VFS_MAX_NAME 256

typedef long ssize_t;

// Forward declarations
struct vfs_node;
struct vfs_filesystem;

// VFS node types
#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02
#define VFS_CHARDEV   0x03
#define VFS_BLOCKDEV  0x04
#define VFS_PIPE      0x05
#define VFS_SYMLINK   0x06
#define VFS_MOUNTPOINT 0x08

// VFS node operations
typedef ssize_t (*vfs_read_t)(struct vfs_node*, uint64_t, size_t, void*);
typedef ssize_t (*vfs_write_t)(struct vfs_node*, uint64_t, size_t, void*);
typedef struct vfs_node* (*vfs_open_t)(struct vfs_node*, const char*);
typedef void (*vfs_close_t)(struct vfs_node*);
typedef struct vfs_node* (*vfs_readdir_t)(struct vfs_node*, uint32_t);
typedef struct vfs_node* (*vfs_finddir_t)(struct vfs_node*, const char*);
typedef int (*vfs_create_t)(struct vfs_node*, const char*, uint32_t);
typedef int (*vfs_unlink_t)(struct vfs_node*, const char*);

// Phase 18: async entry points. The `cb` is invoked when the operation
// finishes (possibly synchronously from within the call). `user_data` is
// echoed back. Return value: 0 if the operation was dispatched (completion
// via cb), negative errno on synchronous failure (cb is NOT called in that
// case). For Phase 18 MVP, grahafs_async_read invokes cb synchronously from
// within the call — the async benefit comes from the worker-thread context,
// not interrupt-driven AHCI.
typedef void    (*vfs_async_completion_t)(int64_t result, void *user_data);
typedef int     (*vfs_async_read_fn_t)(struct vfs_node*, uint64_t offset,
                                       uint64_t len, void *dst,
                                       vfs_async_completion_t cb,
                                       void *user_data);
typedef int     (*vfs_async_write_fn_t)(struct vfs_node*, uint64_t offset,
                                        uint64_t len, const void *src,
                                        vfs_async_completion_t cb,
                                        void *user_data);

// VFS node structure
typedef struct vfs_node {
    char name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t flags;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t size;
    uint32_t inode;
    
    // Operations
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;
    vfs_create_t create;
    vfs_unlink_t unlink;

    // Phase 18: async I/O entry points. NULL-means-unsupported; the generic
    // vfs_async_read/write wrappers fall back to the sync path + immediate
    // callback invocation if these are NULL.
    vfs_async_read_fn_t  async_read;
    vfs_async_write_fn_t async_write;
    
    // Filesystem this node belongs to
    struct vfs_filesystem* fs;
    
    // Implementation-specific data
    void* impl;
    
    // Reference counting
    uint32_t refcount;
    
    // For directory entries
    struct vfs_node* parent;
    struct vfs_node* next;
} vfs_node_t;

// Filesystem structure
typedef struct vfs_filesystem {
    char name[32];
    vfs_node_t* root;
    struct block_device* device;
    void* fs_data;  // Filesystem-specific data
    bool mounted;
} vfs_filesystem_t;

// Represents an open file in the system
typedef struct {
    bool in_use;
    int refcount;       // Phase 10c: reference count for dup/dup2
    void *file_data;
    size_t size;
    size_t offset;
    vfs_node_t* node;  // NEW: Associated VFS node
} open_file_t;

// Represents a block device in the system
typedef struct block_device {
    bool in_use;
    int device_id;
    size_t block_size;
    int (*read_blocks)(int dev_id, uint64_t lba, uint16_t count, void* buf);
    int (*write_blocks)(int dev_id, uint64_t lba, uint16_t count, void* buf);
} block_device_t;

// VFS operations
void vfs_init(void);
int vfs_open(const char *pathname);
ssize_t vfs_read(int fd, void *buffer, size_t count);
ssize_t vfs_write(int fd, void *buffer, size_t count);
int vfs_close(int fd);
void vfs_ref_inc(int fd);   // Phase 10c: increment refcount for dup/dup2
int vfs_truncate(int fd);   // Phase 10c: truncate file to 0 bytes

// Block device operations
void vfs_register_block_device(int dev_id, size_t block_size, 
                               int (*read_func)(int, uint64_t, uint16_t, void*),
                               int (*write_func)(int, uint64_t, uint16_t, void*));
block_device_t* vfs_get_block_device(int dev_id);

// NEW: Filesystem operations
int vfs_mount(const char* device_name, const char* mount_point, const char* fs_type);
int vfs_unmount(const char* mount_point);
vfs_node_t* vfs_get_root(void);
void vfs_set_root(vfs_node_t* root);

// NEW: Node operations
vfs_node_t* vfs_create_node(const char* name, uint32_t type);
void vfs_destroy_node(vfs_node_t* node);

int vfs_create(const char* path, uint32_t mode);
int vfs_mkdir(const char* path, uint32_t mode);
void vfs_sync(void);
vfs_node_t* vfs_path_to_node(const char* path);

// Phase 8a: Get VFS statistics for system state reporting
void vfs_get_stats(uint32_t *open_files, uint32_t *block_devs, uint32_t *mounted_fs);

// Phase 18: async entry-point wrappers. Resolves to the node's async_read /
// async_write fn if installed; otherwise falls back to the sync node->read
// / node->write and invokes the callback synchronously. Returns 0 on
// dispatch (cb will fire) or a negative errno on synchronous failure (cb
// NOT invoked). Called from kernel/io/dispatch_fs.c.
int vfs_async_read(vfs_node_t *node, uint64_t offset, uint64_t len,
                   void *dst, vfs_async_completion_t cb, void *user_data);
int vfs_async_write(vfs_node_t *node, uint64_t offset, uint64_t len,
                    const void *src, vfs_async_completion_t cb, void *user_data);

// Phase 18: resolve a global open_file_table slot into its backing node.
// The caller supplies the file-table index (per-process fd_table[].ref for
// FD_TYPE_FILE). Returns NULL if the slot is not in use or has no node.
// Does not bump the node's refcount; caller must not dereference after
// the underlying fd is closed.
vfs_node_t *vfs_node_for_file_slot(int slot);