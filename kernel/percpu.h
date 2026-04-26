// kernel/percpu.h — Phase 14: first-class per-CPU data.
//
// GSBASE on every CPU points at one percpu_t. The prefix fields (cpu_id..
// syscall_scratch) are the pre-Phase-14 cpu_local_t layout, preserved byte
// for byte so existing GS-relative readers keep working:
//
//   gs:0   = cpu_id          (u32; read by smp_get_current_cpu_id)
//   gs:68  = tss.rsp0        (u64; read by syscall.S to switch stacks)
//   gs:168 = syscall_scratch (u64; R10 save area in syscall entry)
//
// Phase 14 appends slab-allocator magazines, a preemption-disable counter,
// a self-pointer (for percpu_get), and ~384 bytes reserved for Phases
// 17/18/20. Any field reordering breaks assembly — static_asserts guard
// the three load-bearing offsets.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../arch/x86_64/cpu/gdt.h"  // struct gdt_entry, struct tss
#include "../arch/x86_64/cpu/sched/runq.h"  // Phase 20 per-CPU runq

// --- Slab-allocator magazine constants ---
// KMEM_MAGAZINE_CAPACITY is the spec-fixed 8-object cache per (CPU, slab cache).
// KMEM_MAX_CACHES bounds the magazines[] array; Phase 14 ships with 12 caches
// (10 kheap buckets + task_cache + can_entry_cache); cap is 32 to leave room
// for Phase 15a (cap_object_t), Phase 17 (channel_t, vmo_t), Phase 18
// (submission_t), Phase 19 (segment_t) without resizing.
#define KMEM_MAGAZINE_CAPACITY 8
#define KMEM_MAX_CACHES        32

typedef struct kmem_magazine {
    uint8_t  count;                              // 0..KMEM_MAGAZINE_CAPACITY
    uint8_t  reserved[7];                        // pad objects[] to 8-alignment
    void    *objects[KMEM_MAGAZINE_CAPACITY];    // LIFO, push/pop at objects[count-1]
} kmem_magazine_t;

_Static_assert(sizeof(kmem_magazine_t) == 72,
               "kmem_magazine_t: 8 header + 8*8 objects = 72 bytes");

// Forward decl: scheduler's task_t is defined in sched.h; we only hold pointers.
struct task;

typedef struct percpu {
    // === Phase 7a prefix (cpu_local_t) — DO NOT REORDER === //
    uint32_t            cpu_id;              // gs:0   — smp.c:62 movl %gs:0
    uint32_t            lapic_id;            // gs:4
    struct gdt_entry    gdt[7];              // gs:8   — 7*8 = 56 bytes
    struct tss          tss;                 // gs:64  — rsp0 at gs:68
    uint64_t            syscall_scratch;     // gs:168 — syscall.S R10 save

    // === Phase 14 extensions === //
    uint16_t            numa_node;           // gs:176 — 0 in Phase 14; Phase 20 SRAT
    uint16_t            percpu_magic;        // gs:178 — PERCPU_MAGIC sanity
    uint8_t             preempt_saved_if;    // gs:180 — IF bit at outermost disable
    uint8_t             preempt_pad[3];      // gs:181..183
    uint32_t            preempt_disable_count; // gs:184 — nested disable counter
    uint32_t            irq_depth;           // gs:188 — nested ISR depth
    uint32_t            reserved_a;          // gs:192 — u32 to keep u64s aligned
    uint32_t            reserved_a2;         // gs:196
    uint64_t            klog_early_drops;    // gs:200 — per-CPU Phase 13 drop count
    uint64_t            test_slot;           // gs:208 — SYS_DEBUG percpu r/w slot
    struct percpu      *self;                // gs:216 — self-pointer for percpu_get
    uint8_t             reserved_b[32];      // gs:224..255 — pad to 256

    // === Magazines (cache-line aligned) === //
    kmem_magazine_t     magazines[KMEM_MAX_CACHES]; // gs:256, 32*72 = 2304 bytes

    // === Phase 20 scheduler === //
    // Inlined per-CPU runqueue at offset 2560 (where Phase 14 reserved
    // sched_rq_head/tail as placeholders). Full definition in runq.h.
    // Size: 128 bytes → ends at gs:2688.
    runq_t              runq;                // gs:2560..2687

    // === Reserved for Phase 17 (channels), 18 (streams), 20 (stats) === //
    uint8_t             future[256];         // gs:2688..2943

    // === Tail pad to 64-byte alignment (2944 + 64 = 3008 = 47 * 64) === //
    uint8_t             _tail_pad[64];
} __attribute__((aligned(64))) percpu_t;

// Offset contracts enforced at compile time. These close spec risk #1 and
// tricky bit #1: any reorder that breaks the gs:0 / gs:68 / gs:168 reads
// must fail the build before it boots.
_Static_assert(offsetof(percpu_t, cpu_id) == 0,
               "percpu_t.cpu_id must remain at offset 0 for movl %gs:0");
_Static_assert(offsetof(percpu_t, lapic_id) == 4,
               "percpu_t.lapic_id offset drift");
_Static_assert(offsetof(percpu_t, gdt) == 8,
               "percpu_t.gdt base offset drift");
_Static_assert(offsetof(percpu_t, tss) == 64,
               "percpu_t.tss base offset drift (syscall.S reads gs:68 as tss.rsp0)");
_Static_assert(offsetof(percpu_t, syscall_scratch) == 168,
               "percpu_t.syscall_scratch must remain at offset 168");
_Static_assert(offsetof(percpu_t, numa_node) == 176,
               "percpu_t Phase 14 fields drifted into the cpu_local_t prefix area");
_Static_assert(offsetof(percpu_t, magazines) == 256,
               "percpu_t.magazines[] must be cache-line aligned at offset 256");
_Static_assert(offsetof(percpu_t, runq) == 2560,
               "percpu_t.runq must land at offset 2560 (replaces Phase 14 placeholders)");
_Static_assert(sizeof(percpu_t) % 64 == 0,
               "percpu_t total size must be a multiple of 64 bytes");

// Backward-compatibility alias so existing smp.c / syscall.c / gdt.c code
// that says `cpu_local_t` compiles unchanged. Phase 15a can delete this
// after auditing callers.
typedef percpu_t cpu_local_t;

// Sanity magic written into percpu.percpu_magic by percpu_init.
// Helps catch a code path that reads per_cpu() before percpu_init ran.
#define PERCPU_MAGIC 0xB14Eu   /* "B14E" ≈ "phase 14 byte" */

// --- Public API ---

// Populate the Phase 14 extension fields for CPU `cpu_id`. GSBASE must
// already be pointing at the right block (smp_init / ap_main does that).
// Idempotent — safe to call again.
void percpu_init(uint32_t cpu_id);

// Returns the current CPU's percpu_t pointer by reading the self-pointer
// via %gs:offset. One GS-relative load. Safe any time after percpu_init.
percpu_t *percpu_get(void);

// Nestable interrupt-safe preempt guard. Used to protect per-CPU magazine
// ops from ISR preemption. Pairs must be balanced on the same CPU.
void percpu_preempt_disable(void);
void percpu_preempt_enable(void);

// --- GS-relative scalar accessors ---
//
// per_cpu(field)       — reads a scalar field via one `mov %gs:imm, reg`.
// per_cpu_set(field, v) — writes a scalar field via one `mov reg, %gs:imm`.
//
// For composite access (arrays, struct fields) use percpu_get() and
// dereference normally; the compiler can't fold a runtime index into an
// immediate displacement.

#define per_cpu(field)                                                          \
    ({ __typeof__(((percpu_t *)0)->field) _pcv;                                 \
       __asm__ volatile("mov %%gs:%c1, %0"                                      \
                        : "=r"(_pcv)                                            \
                        : "i"(__builtin_offsetof(percpu_t, field)));            \
       _pcv; })

#define per_cpu_set(field, val)                                                 \
    do {                                                                        \
        __typeof__(((percpu_t *)0)->field) _pcx = (val);                        \
        __asm__ volatile("mov %0, %%gs:%c1"                                     \
                         :                                                      \
                         : "r"(_pcx),                                           \
                           "i"(__builtin_offsetof(percpu_t, field))             \
                         : "memory");                                           \
    } while (0)
