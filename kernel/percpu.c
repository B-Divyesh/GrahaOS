// kernel/percpu.c — Phase 14: first-class per-CPU data implementation.
//
// This file initialises the Phase 14 extension fields of percpu_t (the
// prefix fields are still set by smp.c / gdt.c). percpu_get() reads the
// self-pointer via GS. percpu_preempt_disable/enable wrap cli/sti with a
// nestable counter and remember the outermost IF state so we don't
// re-enable interrupts that were already disabled on entry.

#include "percpu.h"
#include "../arch/x86_64/cpu/smp.h"  // cpu_local_t g_cpu_locals[MAX_CPUS]

// --- percpu_init ---
// Called from smp_init (BSP) after GS_BASE is wired up for CPU 0, and from
// ap_main (AP) right after wrmsr(MSR_GS_BASE, ...). GSBASE must already
// point at &g_cpu_locals[cpu_id] when this runs; otherwise the self
// pointer write would land in the wrong block.
//
// This only touches the Phase 14 extension fields (offset 176 onwards).
// The Phase 7a prefix (cpu_id, lapic_id, gdt, tss, syscall_scratch) is
// already initialised by the GDT / SMP code and must not be clobbered.
void percpu_init(uint32_t cpu_id) {
    percpu_t *p = &g_cpu_locals[cpu_id];

    p->numa_node            = 0;               // Phase 20 populates from SRAT.
    p->percpu_magic         = PERCPU_MAGIC;
    p->preempt_saved_if     = 0;
    p->preempt_pad[0]       = 0;
    p->preempt_pad[1]       = 0;
    p->preempt_pad[2]       = 0;
    p->preempt_disable_count = 0;
    p->irq_depth            = 0;
    p->reserved_a           = 0;
    p->reserved_a2          = 0;
    p->klog_early_drops     = 0;
    p->test_slot            = 0;
    p->self                 = p;               // Self-pointer for percpu_get().

    for (unsigned i = 0; i < sizeof(p->reserved_b); ++i) {
        p->reserved_b[i] = 0;
    }

    // Zero the magazines. count=0 means empty; objects[] undefined until push.
    for (unsigned i = 0; i < KMEM_MAX_CACHES; ++i) {
        p->magazines[i].count       = 0;
        p->magazines[i].reserved[0] = 0;
        p->magazines[i].reserved[1] = 0;
        p->magazines[i].reserved[2] = 0;
        p->magazines[i].reserved[3] = 0;
        p->magazines[i].reserved[4] = 0;
        p->magazines[i].reserved[5] = 0;
        p->magazines[i].reserved[6] = 0;
        for (unsigned j = 0; j < KMEM_MAGAZINE_CAPACITY; ++j) {
            p->magazines[i].objects[j] = (void *)0;
        }
    }

    // Phase 20: the per-CPU runqueue lives where sched_rq_head/tail used to.
    // Full initialisation (magic, lock, counters, lists) via runq_init.
    runq_init(&p->runq, cpu_id);

    for (unsigned i = 0; i < sizeof(p->future); ++i) {
        p->future[i] = 0;
    }
    for (unsigned i = 0; i < sizeof(p->_tail_pad); ++i) {
        p->_tail_pad[i] = 0;
    }
}

// --- percpu_get ---
// Reads the self-pointer field (at gs:offsetof(self)) with one load.
// Callers who just need scalar fields should prefer per_cpu(field) which
// avoids the indirection entirely.
percpu_t *percpu_get(void) {
    return per_cpu(self);
}

// --- preempt_disable / _enable ---
//
// Nestable pattern:
//   At the outermost disable (count==0): save RFLAGS.IF, cli, count=1.
//   At nested disables (count>0): just cli + count++, no save.
//   At each enable: count--.
//   At the outermost enable (count reaches 0): sti if we saved IF=1,
//     otherwise leave interrupts disabled because the caller was already
//     in an IRQ-disabled context on entry.
//
// This avoids re-enabling interrupts inside an ISR that happens to call
// into a path that uses magazines.

#define RFLAGS_IF_BIT (1UL << 9)

void percpu_preempt_disable(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq\n\t"
                     "popq %0\n\t"
                     "cli"
                     : "=r"(rflags)
                     :
                     : "memory");

    percpu_t *p = percpu_get();
    if (p->preempt_disable_count == 0) {
        p->preempt_saved_if = (rflags & RFLAGS_IF_BIT) ? 1 : 0;
    }
    p->preempt_disable_count++;
}

void percpu_preempt_enable(void) {
    percpu_t *p = percpu_get();
    // Defensive: underflow would indicate a missing disable on this path.
    // We clamp rather than panic (preempt paths run in hot critical
    // sections; a panic here would cascade). A future slab_debug build
    // could add a kpanic on underflow.
    if (p->preempt_disable_count == 0) {
        return;
    }
    p->preempt_disable_count--;
    if (p->preempt_disable_count == 0 && p->preempt_saved_if) {
        __asm__ volatile("sti" ::: "memory");
    }
}
