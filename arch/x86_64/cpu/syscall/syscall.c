#include "syscall.h"
#include "../../../../drivers/video/framebuffer.h"

// MSR (Model-Specific Register) addresses
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

// External assembly function for syscall entry
extern void syscall_entry(void);

// Helper functions to read and write MSRs
static void write_msr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
    // Enable System Call Extensions
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);

    // Set STAR MSR:
    // Bits 47:32 = Kernel CS (0x08). SS becomes (Kernel CS + 8) = 0x10.
    // Bits 63:48 = User CS base for sysret. CS becomes (base + 16), SS becomes (base + 8).
    // To get CS=0x20 and SS=0x18, we set the base to 0x10.
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);

    // Set LSTAR MSR to the entry point address
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    // Set FMASK MSR to clear Interrupt Flag on syscall
    write_msr(MSR_FMASK, 0x200);
}

// The C-level dispatcher, now using the simpler syscall_frame_t
void syscall_dispatcher(syscall_frame_t *frame) {
    // Get the syscall number from the user's RAX register.
    uint64_t syscall_num = frame->rax;

    switch (syscall_num) {
        case SYS_TEST:
            framebuffer_draw_string("Syscall 0 (SYS_TEST) called from user mode!", 10, 500, COLOR_MAGENTA, 0x00101828);
            // Set the return value in the frame's RAX, which will be restored.
            frame->rax = 42;
            break;

        default:
            framebuffer_draw_string("Unknown syscall number.", 10, 520, COLOR_RED, 0x00101828);
            frame->rax = (uint64_t)-1;
            break;
    }
}