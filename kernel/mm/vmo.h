// kernel/mm/vmo.h — Phase 17.
//
// Virtual Memory Object. A named, reference-counted physical-backed memory
// region. VMOs are the zero-copy vehicle for IPC payloads larger than the
// channel inline slot (256 B), and the substrate Phase 25 will use for
// transactional speculation via copy-on-write clones.
//
// Every live vmo_t is paired with exactly one cap_object_t of kind
// CAP_KIND_VMO whose kind_data stores the vmo_t pointer. Handle-holders
// derive weaker rights via cap_object_derive (e.g., RIGHT_READ-only).
//
// Lock order: task.vmo_mappings.lock → vmo.lock → pmm_lock.
//
// All VMOs are laid out as an array of 4-KiB physical frames. For eager
// VMOs (default) every frame is allocated at vmo_create. For VMO_ONDEMAND
// a page-fault triggers allocation on first read/write. For VMO_COW_CHILD
// a page-fault triggers private-copy on first write; reads follow parent.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../sync/spinlock.h"

// --- Flags ---------------------------------------------------------------
#define VMO_ZEROED       0x1u   // Zero the backing pages at creation
#define VMO_ONDEMAND     0x2u   // Pages allocated on first access (Phase 18+)
#define VMO_PINNED       0x4u   // Pages cannot be evicted (all current VMOs)
#define VMO_COW_CHILD    0x8u   // This is a child of a COW clone
#define VMO_MMIO         0x10u  // Backing is device MMIO (no pmm)

// --- Limits --------------------------------------------------------------
#define VMO_MAX_SIZE    (256ull * 1024 * 1024)  // 256 MiB per VMO
#define VMO_MAPPINGS_PER_TASK 8

struct task_struct;  // forward decl — vmo_mapping_t holds no task pointer
struct cap_object;   // forward decl — vmo_cap_deactivate only takes a pointer

// --- vmo_t: 80-byte (rounded to 96 by slab aligner) VMO header ------------
typedef struct vmo {
    uint32_t  magic;          //   0..3    0xCAFEF00D canary
    uint32_t  flags;          //   4..7
    uint64_t  id;             //   8..15   Monotonic global id; never reused
    uint64_t  size_bytes;     //  16..23   Always 4 KiB aligned
    uint32_t  npages;         //  24..27   size_bytes / 4096
    uint32_t  refcount;       //  28..31   # cap_objects referencing + # maps
    uint64_t *pages;          //  32..39   kheap array of npages uint64 phys addrs (0 ⇒ on-demand)
    struct vmo *parent;       //  40..47   For VMO_COW_CHILD; NULL for roots
    int32_t   owner_pid;      //  48..51   Creator pid (audience bootstrap)
    uint32_t  cap_object_idx; //  52..55   Back-link to the paired cap_object
    spinlock_t lock;          //  56..103  Protects refcount + pages[]
} vmo_t;

// vmo_mapping_t lives inside task_t.vmo_mappings[]. One entry per live
// vmo_map() result. Torn down by vmo_unmap.
typedef struct vmo_mapping {
    uint64_t vaddr;      // Base virtual address; 0 ⇒ slot empty
    vmo_t   *vmo;        // Backing vmo_t (live reference held)
    uint64_t offset;     // Byte offset into vmo; 4-KiB aligned
    uint32_t len_pages;  // # pages mapped
    uint32_t prot;       // PROT_READ | PROT_WRITE | PROT_EXEC
} vmo_mapping_t;

_Static_assert(sizeof(vmo_mapping_t) == 32, "vmo_mapping_t must be 32 bytes");

// --- Lifecycle -----------------------------------------------------------
void vmo_init(void);

// Create a VMO. audience_pid is the sole initial audience; use PID_PUBLIC
// to allow any caller to resolve. Returns vmo_t* (caller wraps in cap_object)
// or NULL on -ENOMEM / -EINVAL. Pages are allocated immediately for eager
// VMOs; zeroed if VMO_ZEROED.
vmo_t *vmo_create(uint64_t size_bytes, uint32_t flags,
                  int32_t owner_pid, int32_t audience_pid);

// Destroy a VMO. For each page, decrement pp_refcount via pmm_page_unref.
// Free pages[] array. Slab-free the vmo_t.
void vmo_free(vmo_t *v);

// Increment refcount (when a second cap_object handle is derived).
void vmo_ref(vmo_t *v);
// Decrement refcount; free if == 0.
void vmo_unref(vmo_t *v);

// --- Mapping -------------------------------------------------------------
// Map a VMO into a task's address space. Uses vmm_reserve_va_by_cr3 if
// addr_hint == 0. prot bits drive PTE flags; caller is responsible for
// verifying cap handle rights against prot before calling.
// Returns mapped virtual base or 0 on failure.
uint64_t vmo_map(vmo_t *v, struct task_struct *t,
                 uint64_t addr_hint, uint64_t offset, uint64_t len,
                 uint32_t prot);

// Unmap a previously mapped VMO region. Frees the mapping slot; decrements
// pp_refcount of each frame; decrements vmo refcount.
// Returns 0 on success, CAP_V2_EINVAL if vaddr/len is not a live mapping.
int vmo_unmap(struct task_struct *t, uint64_t vaddr, uint64_t len);

// --- COW clone -----------------------------------------------------------
// Produce a COW child VMO. All existing mappings of src in any task are
// write-protected; child starts with read-only view of the same frames.
// Writes in either side trigger vmo_cow_fault.
vmo_t *vmo_clone_cow(vmo_t *src, int32_t owner_pid);

// --- Page fault handler --------------------------------------------------
// Returns 0 if the fault was handled (caller should resume). Negative
// otherwise. Registered into vmm's PF hook via vmm_install_pf_handler at
// vmo_init.
int vmo_pf_dispatch(uint64_t fault_va, uint64_t error_code);

// --- cap_object integration ----------------------------------------------
// Called from cap_object_destroy() when the last handle to a VMO cap_object
// closes. Decrements vmo->refcount and frees the VMO at zero.
void vmo_cap_deactivate(struct cap_object *obj);

// Called from sched_reap_zombie to release any residual VMO mappings held
// by the exiting task. Idempotent; no-op if the task had no mappings.
void vmo_cleanup_task(int32_t task_id);

// PROT_* bits (userspace-visible mirror in user/syscalls.h).
#define PROT_READ   0x1u
#define PROT_WRITE  0x2u
#define PROT_EXEC   0x4u

#define VMO_MAGIC  0xCAFEF00Du
