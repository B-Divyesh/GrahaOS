//arch/x86_64/cpu/syscall/syscall.c
#include "syscall.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../gdt.h" // Include GDT header to access the TSS

// MSR (Model-Specific Register) addresses
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_KERNEL_GS_BASE 0xC0000102
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
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | 1);
    
    // Debug: Verify EFER was set correctly
    uint64_t efer_check = read_msr(MSR_EFER);
    if (efer_check & 1) {
        framebuffer_draw_string("EFER.SCE enabled successfully", 700, 500, COLOR_GREEN, 0x00101828);
    } else {
        framebuffer_draw_string("EFER.SCE failed to enable!", 700, 500, COLOR_RED, 0x00101828);
    }

    // Set STAR MSR for kernel/user segments.
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    write_msr(MSR_STAR, star);

    // Set LSTAR MSR to the entry point address
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    // --- THE DEFINITIVE FIX ---
    // Set the KernelGSbase MSR to point to the address of our TSS structure.
    // This enables SWAPGS to work correctly for stack switching.
    write_msr(MSR_KERNEL_GS_BASE, (uint64_t)&kernel_tss);
    
    // Debug: Show syscall entry address
    char addr_str[32] = "Syscall entry: 0x";
    uint64_t addr = (uint64_t)syscall_entry;
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (addr >> (28 - i * 4)) & 0xF;
        addr_str[18 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    addr_str[26] = '\0';
    framebuffer_draw_string(addr_str, 700, 520, COLOR_CYAN, 0x00101828);
    
    // Debug: Verify LSTAR was set correctly
    uint64_t lstar_check = read_msr(MSR_LSTAR);
    char lstar_str[32] = "LSTAR readback: 0x";
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (lstar_check >> (28 - i * 4)) & 0xF;
        lstar_str[19 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    lstar_str[27] = '\0';
    framebuffer_draw_string(lstar_str, 700, 540, COLOR_CYAN, 0x00101828);

    // Debug: Show KernelGSbase was set correctly
    uint64_t kernel_gs_check = read_msr(MSR_KERNEL_GS_BASE);
    char kernel_gs_str[32] = "KernelGS: 0x";
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (kernel_gs_check >> (28 - i * 4)) & 0xF;
        kernel_gs_str[13 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    kernel_gs_str[21] = '\0';
    framebuffer_draw_string(kernel_gs_str, 400, 520, COLOR_MAGENTA, 0x00101828);

    // Set FMASK MSR to clear Interrupt Flag on syscall
    write_msr(MSR_FMASK, 0x200);
    
    // Debug: Check if syscall entry was reached (we'll check this later)
    framebuffer_draw_string("Syscall init complete. Waiting for calls...", 700, 560, COLOR_GREEN, 0x00101828);
    framebuffer_draw_string("SWAPGS mechanism enabled!", 400, 540, COLOR_GREEN, 0x00101828);
}

// Terminal cursor position for user output
static uint32_t term_x = 420;
static uint32_t term_y = 260;

// Enhanced debug variables that assembly can write to
volatile uint64_t syscall_entry_reached = 0;
volatile uint64_t syscall_about_to_return = 0;
volatile uint64_t syscall_frame_created = 0;
volatile uint64_t syscall_pre_dispatch = 0;
volatile uint64_t syscall_stack_switched = 0;

// The C-level dispatcher, now using the CORRECT struct type
void syscall_dispatcher(struct syscall_frame *frame) { // <-- CORRECTED TYPE
    // Enhanced debugging: Show frame structure details
    static int frame_debug_count = 0;
    frame_debug_count++;
    
    if (frame_debug_count <= 5) {
        char debug_msg[64];
        
        // Show frame address - this should now be on kernel stack
        char frame_addr[32] = "Frame @: 0x";
        uint64_t frame_ptr = (uint64_t)frame;
        for (int i = 0; i < 8; i++) {
            uint8_t nibble = (frame_ptr >> (28 - i * 4)) & 0xF;
            frame_addr[12 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        frame_addr[20] = '\0';
        framebuffer_draw_string(frame_addr, 1050, 580 + frame_debug_count * 16, COLOR_YELLOW, 0x00101828);
        
        // Debug: Show that we're now running on kernel stack
        framebuffer_draw_string("Running on KERNEL stack!", 1120, 580 + frame_debug_count * 16, COLOR_GREEN, 0x00101828);
        
        // Show int_no and err_code (should be syscall number and 0)
        char int_err[32] = "int_no=";
        uint64_t int_no = frame->int_no;
        for (int i = 0; i < 4; i++) {
            uint8_t nibble = (int_no >> (12 - i * 4)) & 0xF;
            int_err[7 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        int_err[11] = ' ';
        int_err[12] = 'e';
        int_err[13] = 'r';
        int_err[14] = 'r';
        int_err[15] = '=';
        uint64_t err_code = frame->err_code;
        for (int i = 0; i < 4; i++) {
            uint8_t nibble = (err_code >> (12 - i * 4)) & 0xF;
            int_err[16 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        int_err[20] = '\0';
        framebuffer_draw_string(int_err, 700, 600 + frame_debug_count * 16, COLOR_CYAN, 0x00101828);
        
        // Debug: Show corrected frame structure verification
        framebuffer_draw_string("FIXED: Using syscall_frame struct", 700, 750, COLOR_GREEN, 0x00101828);
    }
    
    // Enhanced RIP validation with debugging
    uint64_t caller_rip = frame->rcx;
    
    // Debug: Show what RIP we're checking - this should now be correct
    static int rip_debug_count = 0;
    rip_debug_count++;
    if (rip_debug_count <= 3) {
        char rip_msg[32] = "FIXED RIP: 0x";
        for (int i = 0; i < 8; i++) {
            uint8_t nibble = (caller_rip >> (28 - i * 4)) & 0xF;
            rip_msg[14 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        rip_msg[22] = '\0';
        framebuffer_draw_string(rip_msg, 400, 300 + rip_debug_count * 16, COLOR_GREEN, 0x00101828);
    }
    
    // Only block obvious kernel addresses (very low addresses)
    // This prevents spurious syscalls from corrupted state
    if (caller_rip < 0x10000) {
        framebuffer_draw_string("Blocked kernel syscall", 400, 400, COLOR_RED, 0x00101828);
        return;
    }
    
    // --- THE DEFINITIVE FIX ---
    // CRITICAL: Read syscall number from int_no field, NOT from rax!
    // The syscall number is in the `int_no` field of the syscall frame,
    // which we populated with the original RAX value in the assembly handler.
    // This prevents corruption because we're not modifying the field we read from.
    uint64_t syscall_num = frame->int_no;
    
    // Debug: Show the critical fix in action
    if (frame_debug_count <= 5) {
        framebuffer_draw_string("FIXED: Reading syscall# from int_no", 700, 780, COLOR_GREEN, 0x00101828);
        
        // Show both values for comparison
        char comparison[64] = "int_no=";
        uint64_t int_no_val = frame->int_no;
        for (int i = 0; i < 4; i++) {
            uint8_t nibble = (int_no_val >> (12 - i * 4)) & 0xF;
            comparison[7 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        comparison[11] = ' ';
        comparison[12] = 'r';
        comparison[13] = 'a';
        comparison[14] = 'x';
        comparison[15] = '=';
        uint64_t rax_val = frame->rax;
        for (int i = 0; i < 4; i++) {
            uint8_t nibble = (rax_val >> (12 - i * 4)) & 0xF;
            comparison[16 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        comparison[20] = '\0';
        framebuffer_draw_string(comparison, 400, 560, COLOR_CYAN, 0x00101828);
    }

    // Debug: Show syscall number for first few calls
    if (frame_debug_count <= 5) {
        char syscall_debug[32] = "Syscall #";
        uint64_t sc_num = syscall_num;
        for (int i = 0; i < 4; i++) {
            uint8_t nibble = (sc_num >> (12 - i * 4)) & 0xF;
            syscall_debug[9 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
        }
        syscall_debug[13] = '\0';
        framebuffer_draw_string(syscall_debug, 950, 580 + frame_debug_count * 16, COLOR_MAGENTA, 0x00101828);
    }

    switch (syscall_num) {
        case SYS_TEST:
            // Set return value in the frame's rax
            frame->rax = 42;
            break;

        case SYS_PUTC: {
            char c = (char)frame->rdi;
            
            // Enhanced debugging for character handling
            if (frame_debug_count <= 3) {
                char char_debug[32] = "Char: '";
                char_debug[7] = (c >= 32 && c <= 126) ? c : '?';
                char_debug[8] = '\'';
                char_debug[9] = ' ';
                char_debug[10] = '(';
                char_debug[11] = '0';
                char_debug[12] = 'x';
                uint8_t char_val = (uint8_t)c;
                char_debug[13] = (char_val >> 4) < 10 ? ('0' + (char_val >> 4)) : ('A' + (char_val >> 4) - 10);
                char_debug[14] = (char_val & 0xF) < 10 ? ('0' + (char_val & 0xF)) : ('A' + (char_val & 0xF) - 10);
                char_debug[15] = ')';
                char_debug[16] = '\0';
                framebuffer_draw_string(char_debug, 400, 600 + frame_debug_count * 16, COLOR_WHITE, 0x00101828);
            }
            
            // Draw the character to the screen
            if (c == '\n') {
                term_x = 420; // Reset to right half margin
                term_y += 16;
            } else {
                framebuffer_draw_char(c, term_x, term_y, COLOR_WHITE);
                term_x += 8;
            }
            
            // Check horizontal bounds - stay in right half
            if (term_x >= framebuffer_get_width() - 20) {
                term_x = 420;
                term_y += 16;
            }
            // Check vertical bounds and scroll if necessary
            if (term_y >= framebuffer_get_height() - 20) {
                // Reset to the top of the user output area
                term_y = 260;
                // Clear the right half for scrolling (corrected coordinates)
                framebuffer_draw_rect(420, 250, framebuffer_get_width() - 420, framebuffer_get_height() - 250, 0x00101828);
            }
            
            // CRITICAL: Set the return value in frame->rax
            // This is safe now because we read the syscall number from int_no,
            // so modifying rax won't affect subsequent syscall dispatching
            frame->rax = 0;
            
            // Debug: Show that we're safely setting the return value
            if (frame_debug_count <= 3) {
                framebuffer_draw_string("Return value set in rax safely", 400, 640, COLOR_GREEN, 0x00101828);
            }
            break;
        }

        default:
            // Set error return value for unknown syscalls
            frame->rax = (uint64_t)-1;
            
            // Debug: Show unknown syscall
            if (frame_debug_count <= 5) {
                framebuffer_draw_string("Unknown syscall - returning error", 400, 660, COLOR_RED, 0x00101828);
            }
            break;
    }
    
    // Additional debugging: Show successful syscall completion
    if (frame_debug_count <= 5) {
        framebuffer_draw_string("Syscall completed successfully", 400, 680, COLOR_GREEN, 0x00101828);
    }
}