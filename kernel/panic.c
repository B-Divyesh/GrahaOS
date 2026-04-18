// kernel/panic.c
// Phase 13: kpanic / kpanic_at implementations.
//
// Rules of the panic path:
//   1. Disable interrupts IMMEDIATELY (before the CAS). This stops
//      the LAPIC timer from re-scheduling us or firing the watchdog.
//   2. First CPU to CAS g_oops_cpu from -1 to cpu_id wins the oops
//      emission. Every other CPU (including this CPU's interrupted
//      context re-entering via an exception within kpanic itself)
//      drops into a tight hlt loop — exactly one frame hits serial.
//   3. All serial output goes via serial_putc / serial_write_* — we
//      deliberately do NOT call klog(), so we can't re-entrancy
//      deadlock on the klog ring spinlock.
//   4. The klog tail is dumped by reading the ring directly. Other
//      CPUs are halted by step 2, so no writer is active; entries
//      still carrying the in-flight guard bit (KLOG_GUARD_BIT) are
//      skipped as "torn".
//   5. After the oops frame is complete, kernel_shutdown() runs. If
//      it returns (it shouldn't), fall into an hlt loop.

#include "panic.h"
#include "log.h"
#include "shutdown.h"

#include "../arch/x86_64/drivers/serial/serial.h"
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/interrupts.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/sched/sched.h"

#include <stdint.h>
#include <stdbool.h>

#ifndef GRAHAOS_BUILD_SHA
#define GRAHAOS_BUILD_SHA "unknown"
#endif

// --- Dual-CPU race guard ---------------------------------------------

static volatile int32_t g_oops_cpu = -1;

static bool try_claim_oops(int32_t cpu) {
    int32_t expected = -1;
    // Strong CAS — cheap, and we need acquire/release ordering to
    // publish subsequent oops writes before other CPUs observe us as
    // the designated emitter.
    return __atomic_compare_exchange_n(&g_oops_cpu, &expected, cpu,
                                       false,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static void __attribute__((noreturn)) hang_forever(void) {
    for (;;) { asm volatile("cli; hlt"); }
}

// --- Serial helpers (bypass klog entirely) ---------------------------

static void s_str(const char *s) {
    if (!s) s = "(null)";
    while (*s) serial_putc(*s++);
}

static void s_hex(uint64_t v) {
    serial_putc('0'); serial_putc('x');
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t n = (uint8_t)((v >> i) & 0xFu);
        serial_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

static void s_dec(int64_t v) {
    if (v < 0) { serial_putc('-'); v = -v; }
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 24) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    while (n > 0) serial_putc(tmp[--n]);
}

static void s_dec_padded(uint64_t v, int width) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 24) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    int pad = width - n;
    while (pad-- > 0) serial_putc('0');
    while (n > 0) serial_putc(tmp[--n]);
}

// --- Framebuffer warning banner --------------------------------------

static void panic_banner(const char *reason, int32_t cpu) {
    framebuffer_clear(COLOR_RED);
    framebuffer_draw_string("KERNEL PANIC", 50, 50,
                            COLOR_WHITE, COLOR_RED);
    framebuffer_draw_string(reason, 50, 80, COLOR_WHITE, COLOR_RED);

    char cpu_msg[16] = "CPU: ";
    cpu_msg[5] = (char)('0' + ((cpu >= 0 && cpu <= 9) ? cpu : 0));
    cpu_msg[6] = '\0';
    framebuffer_draw_string(cpu_msg, 50, 110, COLOR_WHITE, COLOR_RED);
}

// --- Register reads --------------------------------------------------

static uint64_t read_cr2(void) {
    uint64_t v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static uint64_t read_cr3(void) {
    uint64_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static uint64_t read_rflags(void) {
    uint64_t v; asm volatile("pushfq; pop %0" : "=r"(v)); return v;
}
static uint64_t read_rbp_inline(void) {
    uint64_t v; asm volatile("mov %%rbp, %0" : "=r"(v)); return v;
}

// --- Stack trace -----------------------------------------------------

// Walk RBP chain from the provided starting rbp. Print up to 32
// frames. The walk stops on a NULL or mis-aligned rbp, or when the
// next frame pointer sits outside the kernel higher-half canonical
// range. This is deliberately conservative — we'd rather cut the
// trace short than page-fault a second time inside kpanic.
static void dump_stack_trace(uint64_t rbp) {
    for (int i = 0; i < 32; i++) {
        if (rbp == 0) break;
        if (rbp & 0x7) break;  // not 8-byte aligned
        if (rbp < 0xFFFF800000000000ULL) break;  // outside kernel half

        uint64_t *fp = (uint64_t *)rbp;
        uint64_t saved_rip = fp[1];
        uint64_t saved_rbp = fp[0];

        s_str("frame "); s_dec(i); s_str(": rip=");
        s_hex(saved_rip); serial_putc('\n');

        if (saved_rbp <= rbp) break;  // sanity: stack grows down
        rbp = saved_rbp;
    }
}

// --- klog ring tail dump --------------------------------------------

static void dump_klog_tail(void) {
    const klog_entry_t *ring = klog_ring_raw();
    uint64_t head = klog_head_absolute();

    uint64_t want = 256;
    if (want > head) want = head;
    uint64_t start = head - want;

    s_str("==KLOG BEGIN==\n");
    for (uint64_t i = start; i < head; i++) {
        const klog_entry_t *e = &ring[i & (KLOG_RING_ENTRIES - 1)];

        // Skip torn entries — another CPU was writing when we
        // panicked and never cleared the guard bit.
        if (e->level & KLOG_GUARD_BIT) continue;
        // Skip slots that were never written (seq == 0).
        if (e->seq == 0) continue;

        s_str("[seq=");
        s_dec_padded(e->seq, 0);
        s_str(" ");
        uint64_t secs = e->ns_timestamp / 1000000000ULL;
        uint64_t nsec = e->ns_timestamp % 1000000000ULL;
        s_dec_padded(secs, 4);
        serial_putc('.');
        s_dec_padded(nsec, 9);
        s_str("] ");
        s_str(klog_level_name(e->level));
        serial_putc(' ');
        s_str(klog_subsys_name(e->subsystem_id));
        serial_putc(' ');
        // Message can contain arbitrary bytes; stop at NUL or
        // KLOG_MSG_LEN, and skip non-printable characters.
        for (int k = 0; k < KLOG_MSG_LEN && e->message[k]; k++) {
            char c = e->message[k];
            serial_putc((c >= 0x20 && c < 0x7F) || c == '\t' ? c : ' ');
        }
        serial_putc('\n');
    }
    s_str("==KLOG END==\n");
}

// --- Unified emit path ------------------------------------------------

static void emit_oops(const char *reason, int32_t cpu_id,
                      struct interrupt_frame *frame,
                      uint64_t inline_rip, uint64_t inline_rsp,
                      uint64_t inline_rbp) {
    // Banner.
    panic_banner(reason, cpu_id);

    // Header line.
    s_str("\n==OOPS== phase=13 build=");
    s_str(GRAHAOS_BUILD_SHA);
    s_str(" cpu=");
    s_dec(cpu_id);
    s_str(" pid=");
    // Every other CPU is halted, so sampling the per-CPU current
    // task without a lock is safe here. If sched isn't up yet, get
    // NULL and record -1.
    {
        task_t *tk = sched_get_current_task();
        s_dec(tk ? (int64_t)tk->id : (int64_t)-1);
    }
    s_str(" reason=\"");
    s_str(reason);
    s_str("\"\n");

    s_str("oops.magic=");
    s_hex(OOPS_MAGIC);
    serial_putc('\n');

    s_str("oops.cr2=");
    s_hex(read_cr2());
    s_str(" oops.cr3=");
    s_hex(read_cr3());
    serial_putc('\n');

    uint64_t rip, rsp, rflags, rbp;
    if (frame) {
        rip    = frame->rip;
        rsp    = frame->rsp;
        rflags = frame->rflags;
        rbp    = frame->rbp;
    } else {
        rip    = inline_rip;
        rsp    = inline_rsp;
        rflags = read_rflags();
        rbp    = inline_rbp;
    }

    s_str("oops.rip=");    s_hex(rip);
    s_str(" oops.rsp=");   s_hex(rsp);
    s_str(" oops.rflags=");s_hex(rflags);
    serial_putc('\n');

    if (frame) {
        s_str("regs.rax="); s_hex(frame->rax);
        s_str(" rbx=");     s_hex(frame->rbx);
        s_str(" rcx=");     s_hex(frame->rcx);
        s_str(" rdx=");     s_hex(frame->rdx);
        serial_putc('\n');
        s_str("regs.rsi="); s_hex(frame->rsi);
        s_str(" rdi=");     s_hex(frame->rdi);
        s_str(" rbp=");     s_hex(frame->rbp);
        s_str(" r8=");      s_hex(frame->r8);
        serial_putc('\n');
        s_str("regs.r9=");  s_hex(frame->r9);
        s_str(" r10=");     s_hex(frame->r10);
        s_str(" r11=");     s_hex(frame->r11);
        s_str(" r12=");     s_hex(frame->r12);
        serial_putc('\n');
        s_str("regs.r13="); s_hex(frame->r13);
        s_str(" r14=");     s_hex(frame->r14);
        s_str(" r15=");     s_hex(frame->r15);
        serial_putc('\n');
        s_str("int_no=");   s_hex(frame->int_no);
        s_str(" err=");     s_hex(frame->err_code);
        serial_putc('\n');
    }

    dump_stack_trace(rbp);
    dump_klog_tail();
    s_str("==OOPS END==\n");

    // Let the serial FIFO drain before we trip the shutdown path —
    // otherwise the last line of the oops can be lost.
    for (volatile int i = 0; i < 100000; i++) { asm volatile("pause"); }

    kernel_shutdown();
}

// --- Public entry points ---------------------------------------------

void kpanic(const char *reason) {
    asm volatile("cli");

    int32_t cpu = (int32_t)smp_get_current_cpu();
    if (!try_claim_oops(cpu)) hang_forever();

    uint64_t rip, rsp, rbp;
    // Sample a rough rip from the return address of this frame.
    // __builtin_return_address(0) = our caller's rip.
    rip = (uint64_t)__builtin_return_address(0);
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    rbp = read_rbp_inline();

    if (!reason) reason = "(null)";
    emit_oops(reason, cpu, NULL, rip, rsp, rbp);
    hang_forever();
}

void kpanic_at(struct interrupt_frame *frame, const char *reason) {
    asm volatile("cli");

    int32_t cpu = (int32_t)smp_get_current_cpu();
    if (!try_claim_oops(cpu)) hang_forever();

    if (!reason) reason = "(null)";
    emit_oops(reason, cpu, frame, 0, 0, 0);
    hang_forever();
}
