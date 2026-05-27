// kernel/console/cell_tx.h
//
// Phase 29 Session E — Cell-grid atomic transactions.
//
// SYS_CONSOLE_BEGIN_TX clones the console's cell-VMO into a "shadow" page
// set and remaps the live VMO's `pages[]` to those shadow pages so any
// userspace write to the previously-mapped cell-VMO actually lands in the
// shadow.  On COMMIT we leave the shadow in place (it IS the new live
// content).  On ABORT we restore the original `pages[]` array and free the
// shadow pages, reverting any in-flight writes.
//
// v1 simplifications:
//   * Per-console TX is mutually exclusive (one TX active per console at
//     a time) — concurrent TX from different processes returns -EBUSY.
//   * Shadow allocation goes through pmm_alloc_page so it's bounded by
//     PMM headroom; abort emits AUDIT_TUI_TX_ABORT.
//   * Userspace mapping pointers remain valid because the cell-VMO is
//     mapped via its `pages[]` lookup at fault time; we mutate those
//     entries under console_table.lock and rely on the user PTEs being
//     populated on demand.  For already-faulted-in pages, we update the
//     PTE directly via vmm_protect_page_by_cr3.
//
// v1 ABI note: the shadow's lifecycle is entirely kernel-side.  Userspace
// only sees a single tx_handle.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "console.h"

#define CONSOLE_TX_HANDLE_MAX  16u   // max concurrent TXs across all consoles

#define TX_STATE_FREE       0u
#define TX_STATE_ACTIVE     1u
#define TX_STATE_COMMITTED  2u
#define TX_STATE_ABORTED    3u

typedef struct console_tx {
    uint8_t   state;
    uint8_t   _pad[3];
    int32_t   owner_pid;
    uint32_t  console_id;
    uint32_t  npages;
    uint64_t *shadow_pages;     // kheap array of npages physical addrs
    uint64_t *saved_pages;      // saved original cell_vmo->pages snapshot
} console_tx_t;

// Boot-time init.  Called from console_init (or first BEGIN_TX).
void console_tx_init(void);

// SYS_CONSOLE_BEGIN_TX backend.  Returns tx_handle (>= 1) on success or
// negative -errno.  tx_handle 0 is reserved (so tests can detect "not set").
int console_tx_begin(int32_t caller_pid, uint32_t console_id);

// SYS_CONSOLE_COMMIT_TX backend.
int console_tx_commit(int32_t caller_pid, uint32_t tx_handle);

// SYS_CONSOLE_ABORT_TX backend.  Emits AUDIT_TUI_TX_ABORT.
int console_tx_abort(int32_t caller_pid, uint32_t tx_handle);
