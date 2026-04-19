// kernel/fs/pipe.c — Phase 17 shim over channels.
//
// Legacy pipe_t with its own 4 KiB ring is gone. Each pipe slot now holds
// a pointer to a channel_t created via chan_create_kernel(). pipe_read/
// write route through chan_recv/chan_send. The FD table still tags
// FD_TYPE_PIPE_READ / _WRITE for userspace ABI compatibility, but
// internally every pipe is a manifest-typed ("grahaos.pipe.bytes.v1")
// channel with MODE_BLOCKING and capacity 64.
#include "pipe.h"

#include "../ipc/channel.h"
#include "../ipc/manifest.h"
#include "../log.h"
#include "../../arch/x86_64/cpu/sched/sched.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct pipe_slot {
    channel_t *channel;
    uint8_t    in_use;
    uint8_t    readers;
    uint8_t    writers;
    uint8_t    _pad;
    // Shim-only: byte-stream view into the head ring slot. Without this a
    // 1-byte pipe_read on a slot carrying 2+ bytes would consume the whole
    // slot and silently drop the tail. Tracks bytes already delivered from
    // ring[c->head].inline_payload.
    uint16_t   head_read_off;
} pipe_slot_t;

static pipe_slot_t g_pipes[MAX_PIPES];

void pipe_init(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        g_pipes[i].channel  = NULL;
        g_pipes[i].in_use   = 0;
        g_pipes[i].readers  = 0;
        g_pipes[i].writers  = 0;
    }
}

int pipe_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!g_pipes[i].in_use) {
            channel_t *c = chan_create_kernel(manifest_hash_pipe_bytes_v1(),
                                              CHAN_MODE_BLOCKING, 64);
            if (!c) {
                klog(KLOG_ERROR, SUBSYS_VFS, "[PIPE] chan_create_kernel failed");
                return -1;
            }
            g_pipes[i].channel      = c;
            g_pipes[i].in_use       = 1;
            g_pipes[i].readers      = 1;
            g_pipes[i].writers      = 1;
            g_pipes[i].head_read_off = 0;
            klog(KLOG_INFO, SUBSYS_VFS, "[PIPE] Allocated pipe %lu (shim)", (unsigned long)i);
            return i;
        }
    }
    klog(KLOG_ERROR, SUBSYS_VFS, "[PIPE] ERROR: No free pipes");
    return -1;
}

// Byte-stream wrapper over chan_recv/chan_send. Each byte becomes a single
// 1-byte inline message. Coarse but correct for Phase 17 pipetest (10/10).
int pipe_read(int idx, void *buf, int count) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].in_use) return -1;
    if (!buf || count <= 0) return -1;
    channel_t *c = g_pipes[idx].channel;
    if (!c) return -1;

    uint8_t *dst = (uint8_t *)buf;
    int read_total = 0;
    while (read_total < count) {
        // If writers closed AND nothing in ring → EOF.
        if (g_pipes[idx].writers == 0 && c->msgcount == 0) {
            return read_total;
        }
        spinlock_acquire(&c->lock);
        while (c->msgcount > 0 && read_total < count) {
            channel_msg_t *head_slot = &c->ring[c->head];
            uint16_t n = head_slot->header.inline_len;
            while (g_pipes[idx].head_read_off < n && read_total < count) {
                dst[read_total++] = head_slot->inline_payload[g_pipes[idx].head_read_off++];
            }
            if (g_pipes[idx].head_read_off >= n) {
                // Slot fully consumed — advance ring head.
                g_pipes[idx].head_read_off = 0;
                c->head = (c->head + 1) % c->capacity;
                c->msgcount--;
                c->total_recvs++;
                sched_wake_one_on_channel(&c->write_waiters, 0);
            } else {
                break;  // count satisfied mid-slot
            }
        }
        spinlock_release(&c->lock);
        if (read_total > 0) return read_total;
        if (g_pipes[idx].writers == 0) return 0;
        asm volatile("sti; hlt; cli");
    }
    return read_total;
}

int pipe_write(int idx, const void *buf, int count) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].in_use) return -1;
    if (!buf || count <= 0) return -1;
    channel_t *c = g_pipes[idx].channel;
    if (!c) return -1;
    if (g_pipes[idx].readers == 0) return -1;  // broken pipe

    const uint8_t *src = (const uint8_t *)buf;
    int written = 0;
    while (written < count) {
        if (g_pipes[idx].readers == 0) return written > 0 ? written : -1;

        spinlock_acquire(&c->lock);
        while (c->msgcount < c->capacity && written < count) {
            channel_msg_t *slot = &c->ring[c->tail];
            // Pack up to CHAN_MSG_INLINE_MAX bytes per message.
            uint16_t chunk = (uint16_t)(count - written);
            if (chunk > CHAN_MSG_INLINE_MAX) chunk = CHAN_MSG_INLINE_MAX;
            memcpy(slot->inline_payload, src + written, chunk);
            slot->header.seq        = c->seq_next++;
            slot->header.type_hash  = c->type_hash;
            slot->header.inline_len = chunk;
            slot->header.nhandles   = 0;
            slot->header.flags      = 0;
            slot->header.sender_pid = 0;
            slot->header.timestamp_tsc = 0;
            for (int h = 0; h < CHAN_MSG_HANDLES_MAX; h++) slot->in_flight_idx[h] = 0;
            c->tail = (c->tail + 1) % c->capacity;
            c->msgcount++;
            c->total_sends++;
            written += chunk;
            sched_wake_one_on_channel(&c->read_waiters, 0);
        }
        spinlock_release(&c->lock);

        if (written == count) break;
        // Full; yield and retry.
        asm volatile("sti; hlt; cli");
    }
    return written;
}

int pipe_read_char(int idx) {
    uint8_t c = 0;
    int n = pipe_read(idx, &c, 1);
    if (n <= 0) return 0;
    return (int)(unsigned char)c;
}

void pipe_ref_inc(int idx, uint8_t fd_type) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].in_use) return;
    if (fd_type == 3) g_pipes[idx].readers++;
    else if (fd_type == 4) g_pipes[idx].writers++;
}

void pipe_ref_dec(int idx, uint8_t fd_type) {
    if (idx < 0 || idx >= MAX_PIPES || !g_pipes[idx].in_use) return;
    if (fd_type == 3 && g_pipes[idx].readers > 0) g_pipes[idx].readers--;
    else if (fd_type == 4 && g_pipes[idx].writers > 0) g_pipes[idx].writers--;

    if (g_pipes[idx].readers == 0 && g_pipes[idx].writers == 0) {
        klog(KLOG_INFO, SUBSYS_VFS, "[PIPE] Freed pipe %lu (shim)", (unsigned long)idx);
        chan_destroy_kernel(g_pipes[idx].channel);
        g_pipes[idx].channel = NULL;
        g_pipes[idx].in_use  = 0;
    }
}
