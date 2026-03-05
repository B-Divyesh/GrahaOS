// kernel/fs/pipe.h
// Phase 10b: Kernel pipe implementation
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../sync/spinlock.h"

#define MAX_PIPES       16
#define PIPE_BUF_SIZE   4096

typedef struct {
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;      // Next byte to read
    uint32_t write_pos;     // Next byte to write
    uint32_t count;         // Bytes currently in buffer
    uint32_t readers;       // Number of open read-end FDs
    uint32_t writers;       // Number of open write-end FDs
    uint8_t  in_use;        // 1 = allocated, 0 = free
    spinlock_t lock;
} pipe_t;

// Allocate a new pipe, returns pipe index or -1 on failure
int pipe_alloc(void);

// Read from pipe (blocking). Returns bytes read, 0 on EOF (no writers left)
int pipe_read(int idx, void *buf, int count);

// Write to pipe (blocking). Returns bytes written, -1 on broken pipe (no readers)
int pipe_write(int idx, const void *buf, int count);

// Read single char from pipe (for SYS_GETC). Returns char or 0 on EOF
int pipe_read_char(int idx);

// Increment reader or writer refcount
// fd_type: FD_TYPE_PIPE_READ or FD_TYPE_PIPE_WRITE
void pipe_ref_inc(int idx, uint8_t fd_type);

// Decrement reader or writer refcount, free pipe when both hit 0
void pipe_ref_dec(int idx, uint8_t fd_type);

// Initialize the pipe subsystem
void pipe_init(void);
