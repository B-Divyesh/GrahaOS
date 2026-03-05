// kernel/fs/pipe.c
// Phase 10b: Kernel pipe implementation
#include "pipe.h"
#include "../../arch/x86_64/drivers/serial/serial.h"

static pipe_t pipes[MAX_PIPES];

void pipe_init(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].in_use = 0;
        pipes[i].read_pos = 0;
        pipes[i].write_pos = 0;
        pipes[i].count = 0;
        pipes[i].readers = 0;
        pipes[i].writers = 0;
        spinlock_init(&pipes[i].lock, "pipe");
    }
}

int pipe_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) {
            pipes[i].in_use = 1;
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].count = 0;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            serial_write("[PIPE] Allocated pipe ");
            serial_write_dec(i);
            serial_write("\n");
            return i;
        }
    }
    serial_write("[PIPE] ERROR: No free pipes\n");
    return -1;
}

int pipe_read(int idx, void *buf, int count) {
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].in_use) return -1;
    if (!buf || count <= 0) return -1;

    pipe_t *p = &pipes[idx];
    uint8_t *dst = (uint8_t *)buf;
    int bytes_read = 0;

    // Block while pipe is empty and writers still exist
    while (p->count == 0 && p->writers > 0) {
        // Yield CPU — same pattern as SYS_GETC keyboard wait
        asm volatile("sti; hlt; cli");
    }

    // If pipe is empty and no writers, return 0 (EOF)
    if (p->count == 0 && p->writers == 0) {
        return 0;
    }

    // Read available data (up to count bytes)
    spinlock_acquire(&p->lock);
    while (bytes_read < count && p->count > 0) {
        dst[bytes_read++] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    spinlock_release(&p->lock);

    return bytes_read;
}

int pipe_write(int idx, const void *buf, int count) {
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].in_use) return -1;
    if (!buf || count <= 0) return -1;

    pipe_t *p = &pipes[idx];
    const uint8_t *src = (const uint8_t *)buf;
    int bytes_written = 0;

    while (bytes_written < count) {
        // If no readers, broken pipe
        if (p->readers == 0) {
            return bytes_written > 0 ? bytes_written : -1;
        }

        // Block while pipe is full and readers still exist
        while (p->count >= PIPE_BUF_SIZE && p->readers > 0) {
            asm volatile("sti; hlt; cli");
        }

        if (p->readers == 0) {
            return bytes_written > 0 ? bytes_written : -1;
        }

        // Write available space
        spinlock_acquire(&p->lock);
        while (bytes_written < count && p->count < PIPE_BUF_SIZE) {
            p->buffer[p->write_pos] = src[bytes_written++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        spinlock_release(&p->lock);
    }

    return bytes_written;
}

int pipe_read_char(int idx) {
    uint8_t c = 0;
    int n = pipe_read(idx, &c, 1);
    if (n <= 0) return 0; // EOF or error
    return (int)(unsigned char)c;
}

void pipe_ref_inc(int idx, uint8_t fd_type) {
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].in_use) return;

    // fd_type 3 = FD_TYPE_PIPE_READ, 4 = FD_TYPE_PIPE_WRITE
    if (fd_type == 3) {
        pipes[idx].readers++;
    } else if (fd_type == 4) {
        pipes[idx].writers++;
    }
}

void pipe_ref_dec(int idx, uint8_t fd_type) {
    if (idx < 0 || idx >= MAX_PIPES || !pipes[idx].in_use) return;

    if (fd_type == 3) {
        if (pipes[idx].readers > 0) pipes[idx].readers--;
    } else if (fd_type == 4) {
        if (pipes[idx].writers > 0) pipes[idx].writers--;
    }

    // Free pipe when all ends are closed
    if (pipes[idx].readers == 0 && pipes[idx].writers == 0) {
        serial_write("[PIPE] Freed pipe ");
        serial_write_dec(idx);
        serial_write("\n");
        pipes[idx].in_use = 0;
        pipes[idx].count = 0;
        pipes[idx].read_pos = 0;
        pipes[idx].write_pos = 0;
    }
}
