// kernel/shutdown.c
// Phase 12: ACPI soft-off with triple-fault fallback.
//
// Call-sites today:
//   - autorun_on_init_exit() when PID 1 exits in autorun mode
//   - (future) kernel panic path (Phase 13)
//   - (future) watchdog TEST_TIMEOUT panic

#include "shutdown.h"
#include "../arch/x86_64/cpu/ports.h"
#include "../arch/x86_64/drivers/serial/serial.h"
#include <stdint.h>
#include "log.h"

// Rough busy-wait. The kernel has no unified sleep helper (timer
// subsystem is scheduler-bound). At typical QEMU simulated clock
// speeds this yields ~50 ms which is plenty for a PIIX4 ACPI write
// to take effect.
static inline void busy_delay_short(void) {
    for (volatile uint64_t i = 0; i < 50ull * 1000ull * 1000ull; i++) {
        asm volatile("pause");
    }
}

// Zero-limit IDTR forces a triple fault on the next interrupt/exception.
// `packed` matches the x86_64 LIDT descriptor layout (2-byte limit then
// 8-byte base).
struct __attribute__((packed)) idt_descriptor {
    uint16_t limit;
    uint64_t base;
};

void kernel_shutdown(void) {
    klog(KLOG_INFO, SUBSYS_CORE, "shutdown: attempting ACPI soft-off...");

    // Disable local interrupts so we don't get preempted between port
    // writes. Other CPUs may still run briefly before the shutdown
    // command reaches them; that's fine — the hypervisor tears the
    // whole VM down once any CPU completes the sequence.
    asm volatile("cli");

    // QEMU (modern): PIIX4 ACPI PM1a_CNT at 0x604. Writing
    // SLP_TYPa=5 (S5 soft-off) with SLP_EN (bit 13) halts the VM.
    outw(0x604, 0x2000);
    busy_delay_short();

    // Legacy QEMU / Bochs: same semantics on 0xB004.
    klog(KLOG_INFO, SUBSYS_CORE, "shutdown: trying 0xB004...");
    outw(0xB004, 0x2000);
    busy_delay_short();

    // VMware magic port (SLP_TYPa=5 << 10 | SLP_EN).
    klog(KLOG_INFO, SUBSYS_CORE, "shutdown: trying 0x4004...");
    outw(0x4004, 0x3400);
    busy_delay_short();

    // All three ACPI paths failed. Force a triple fault by loading a
    // zero-sized IDT then issuing an interrupt. Under `qemu -no-reboot`
    // this cleanly terminates the VM. On real hardware it resets.
    klog(KLOG_ERROR, SUBSYS_CORE, "shutdown: ACPI failed, forcing triple fault");
    struct idt_descriptor zero_idt = { .limit = 0, .base = 0 };
    asm volatile("lidt %0" : : "m"(zero_idt));
    asm volatile("int3");

    // Unreachable. If we somehow survive the triple fault, hang hard.
    for (;;) {
        asm volatile("cli; hlt");
    }
}
