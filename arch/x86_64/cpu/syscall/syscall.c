//arch/x86_64/cpu/syscall/syscall.c
#include "syscall.h"
#include "../../../../drivers/video/framebuffer.h"
#include "../gdt.h" // Include GDT header to access the TSS
#include "../../../../kernel/fs/vfs.h" // <-- ADDED

// Add this at file scope to reduce stack usage
static char debug_buffer[256];

// MSR (Model-Specific Register) addresses
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_FMASK 0xC0000084

// Screen organization constants
#define DEBUG_X_START 400    // Start of debug area (x: 200-1000)
#define DEBUG_X_MID 600      // Middle column for debug
#define DEBUG_Y_START 320    // Start of debug area (y: 300-780)
#define DEBUG_LINE_HEIGHT 16 // Height between debug lines
#define USER_OUTPUT_X 520    // User output area starts here
#define USER_OUTPUT_Y 240    // User output starts here

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

// Helper function to format hex values for debugging
static void format_hex64(char *buffer, uint64_t value, int digits) {
    for (int i = 0; i < digits; i++) {
        uint8_t nibble = (value >> ((digits - 1 - i) * 4)) & 0xF;
        buffer[i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buffer[digits] = '\0';
}

// Helper to safely copy string from user-space
// Returns number of bytes copied (including null), or -1 on error.
static int copy_string_from_user(const char *user_src, char *k_dest, size_t max_len) {
    // Basic validation for now
    if (user_src == NULL || k_dest == NULL) return -1;
    // A more robust check would validate the entire user memory range.
    
    size_t i = 0;
    for (i = 0; i < max_len - 1; ++i) {
        k_dest[i] = user_src[i];
        if (user_src[i] == '\0') {
            return i + 1;
        }
    }
    k_dest[max_len - 1] = '\0';
    return max_len;
}

// Add this function to syscall.c after the format_hex64 function
static void debug_memory_layout(struct syscall_frame *frame, int call_num) {
    if (call_num > 1) return; // Only debug first syscall
    
    framebuffer_draw_string("=== MEMORY LAYOUT DEBUG ===", 50, 600, COLOR_YELLOW, 0x00101828);
    
    // Cast to raw uint64_t pointer to examine memory
    uint64_t *raw = (uint64_t *)frame;
    
    // Show key offsets and their values
    char msg[80];
    
    // Offset 0 (should be RAX with syscall number)
    char *p = msg;
    const char *s1 = "Off[0](rax?)=0x";
    while (*s1) *p++ = *s1++;
    format_hex64(p, raw[0], 16);
    p += 16;
    *p = '\0';
    framebuffer_draw_string(msg, 50, 620, COLOR_WHITE, 0x00101828);
    
    // Offset 2 (should be RCX with user RIP)
    p = msg;
    const char *s2 = "Off[2](rcx?)=0x";
    while (*s2) *p++ = *s2++;
    format_hex64(p, raw[2], 16);
    p += 16;
    *p = '\0';
    framebuffer_draw_string(msg, 50, 640, COLOR_WHITE, 0x00101828);
    
    // Offset 5 (should be RDI with 'H')
    p = msg;
    const char *s3 = "Off[5](rdi?)=0x";
    while (*s3) *p++ = *s3++;
    format_hex64(p, raw[5], 16);
    p += 16;
    const char *s3b = " char='";
    while (*s3b) *p++ = *s3b++;
    *p++ = (raw[5] >= 32 && raw[5] <= 126) ? (char)raw[5] : '?';
    *p++ = '\'';
    *p = '\0';
    framebuffer_draw_string(msg, 50, 660, COLOR_WHITE, 0x00101828);
    
    // Offset 15 (should be int_no)
    p = msg;
    const char *s4 = "Off[15](int_no?)=0x";
    while (*s4) *p++ = *s4++;
    format_hex64(p, raw[15], 4);
    p += 4;
    *p = '\0';
    framebuffer_draw_string(msg, 50, 680, COLOR_WHITE, 0x00101828);
    
    // Offset 16 (should be err_code)
    p = msg;
    const char *s5 = "Off[16](err?)=0x";
    while (*s5) *p++ = *s5++;
    format_hex64(p, raw[16], 4);
    p += 4;
    *p = '\0';
    framebuffer_draw_string(msg, 50, 700, COLOR_WHITE, 0x00101828);
    
    // Offset 17 (should be user_rsp)
    p = msg;
    const char *s6 = "Off[17](usp?)=0x";
    while (*s6) *p++ = *s6++;
    format_hex64(p, raw[17], 16);
    p += 16;
    *p = '\0';
    framebuffer_draw_string(msg, 50, 720, COLOR_WHITE, 0x00101828);
}

// Organized frame debugging function
// Optimized debug function
static void debug_syscall_frame(struct syscall_frame *frame, int call_number) {
    if (call_number > 3) return;
    
    int base_y = DEBUG_Y_START + (call_number - 1) * 120;
    
    // Use single buffer instead of multiple stack arrays
    char *buf = debug_buffer;
    
    // Header
    int len = 0;
    const char *header = "=== SYSCALL #";
    while (*header) buf[len++] = *header++;
    buf[len++] = '0' + call_number;
    const char *suffix = " DEBUG ===";
    while (*suffix) buf[len++] = *suffix++;
    buf[len] = '\0';
    framebuffer_draw_string(buf, DEBUG_X_START, base_y, COLOR_YELLOW, 0x00101828);
    
    // Line 1: Syscall number and frame address
    len = 0;
    const char *s1 = "Syscall: 0x";
    while (*s1) buf[len++] = *s1++;
    format_hex64(buf + len, frame->int_no, 4);
    len += 4;
    const char *s2 = " Frame@0x";
    while (*s2) buf[len++] = *s2++;
    format_hex64(buf + len, (uint64_t)frame, 8);
    len += 8;
    buf[len] = '\0';
    framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT, COLOR_CYAN, 0x00101828);
    
    // Line 2: Critical registers
    len = 0;
    const char *s3 = "RAX=0x";
    while (*s3) buf[len++] = *s3++;
    format_hex64(buf + len, frame->rax, 8);
    len += 8;
    const char *s4 = " RDI=0x";
    while (*s4) buf[len++] = *s4++;
    format_hex64(buf + len, frame->rdi, 8);
    len += 8;
    buf[len] = '\0';
    framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT * 2, COLOR_WHITE, 0x00101828);
    
    // Line 3: Return address validation
    len = 0;
    const char *s5 = "RCX(RIP)=0x";
    while (*s5) buf[len++] = *s5++;
    format_hex64(buf + len, frame->rcx, 16);
    len += 16;
    if (frame->rcx >= 0x400000 && frame->rcx <= 0x7FFFFFFFFFFF) {
        const char *s6 = " OK";
        while (*s6) buf[len++] = *s6++;
        buf[len] = '\0';
        framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT * 3, COLOR_GREEN, 0x00101828);
    } else {
        const char *s6 = " BAD!";
        while (*s6) buf[len++] = *s6++;
        buf[len] = '\0';
        framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT * 3, COLOR_RED, 0x00101828);
    }
    
    // Line 4: Stack and flags info
    len = 0;
    const char *s7 = "R11(RFLAGS)=0x";
    while (*s7) buf[len++] = *s7++;
    format_hex64(buf + len, frame->r11, 4);
    len += 4;
    const char *s8 = " USP=0x";
    while (*s8) buf[len++] = *s8++;
    format_hex64(buf + len, frame->user_rsp, 8);
    len += 8;
    buf[len] = '\0';
    framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT * 4, COLOR_MAGENTA, 0x00101828);
    
    // Line 5: Error code and interrupt info  
    len = 0;
    const char *s9 = "int_no=0x";
    while (*s9) buf[len++] = *s9++;
    format_hex64(buf + len, frame->int_no, 4);
    len += 4;
    const char *s10 = " err_code=0x";
    while (*s10) buf[len++] = *s10++;
    format_hex64(buf + len, frame->err_code, 4);
    len += 4;
    buf[len] = '\0';
    framebuffer_draw_string(buf, DEBUG_X_START, base_y + DEBUG_LINE_HEIGHT * 5, COLOR_CYAN, 0x00101828);
}

// Terminal cursor position - adjusted for user output area
static uint32_t term_x = USER_OUTPUT_X;
static uint32_t term_y = USER_OUTPUT_Y;

// Enhanced debug variables that assembly can write to
volatile uint64_t syscall_entry_reached = 0;
volatile uint64_t syscall_about_to_return = 0;
volatile uint64_t syscall_frame_created = 0;
volatile uint64_t syscall_pre_dispatch = 0;
volatile uint64_t syscall_stack_switched = 0;

// The C-level dispatcher, cleaned and organized
void syscall_dispatcher(struct syscall_frame *frame) {
    static int call_count = 0;
    call_count++;
    
    // Debug the syscall frame for first few calls
    debug_syscall_frame(frame, call_count);
    
    // Validate caller RIP - should be in user space
    uint64_t caller_rip = frame->rcx;
    if (caller_rip < 0x400000 || caller_rip > 0x7FFFFFFFFFFF) {
        // Show RIP validation error in organized debug area
        char error_msg[64] = "ERROR: Invalid RIP 0x";
        format_hex64(error_msg + 22, caller_rip, 16);
        framebuffer_draw_string(error_msg, DEBUG_X_START, DEBUG_Y_START + 400, COLOR_RED, 0x00101828);
        
        frame->rax = (uint64_t)-1;
        return;
    }
    
    // Read syscall number from int_no field
    uint64_t syscall_num = frame->int_no;
    
    // Show syscall processing status in organized area
    if (call_count <= 5) {
        char status[64] = "Processing syscall #";
        format_hex64(status + 20, syscall_num, 4);
        status[24] = ' ';
        status[25] = 'c';
        status[26] = 'a';
        status[27] = 'l';
        status[28] = 'l';
        status[29] = '#';
        status[30] = '0' + call_count;
        status[31] = '\0';
        framebuffer_draw_string(status, DEBUG_X_MID, DEBUG_Y_START - 20 + (call_count - 1) * 80, COLOR_GREEN, 0x00101828);
    }

    switch (syscall_num) {
        case SYS_DEBUG: {
            uint64_t marker = frame->rdi;
            char msg[32] = "DEBUG MARKER: 0x";
            format_hex64(msg + 16, marker, 4);
            framebuffer_draw_string(msg, 50, 760, COLOR_YELLOW, 0x00101828);
            frame->rax = 0;
            break;
        }
        
        case SYS_TEST:
            // Set return value in the frame's rax
            frame->rax = 42;
            
            // Debug output for test syscall
            if (call_count <= 5) {
                framebuffer_draw_string("SYS_TEST: returning 42", DEBUG_X_MID, 
                                       DEBUG_Y_START + (call_count - 1) * 80 + DEBUG_LINE_HEIGHT, 
                                       COLOR_YELLOW, 0x00101828);
            }
            break;

        case SYS_PUTC: {
            char c = (char)frame->rdi;
            
            // Debug character info in organized area
            if (call_count <= 20) {
                char char_debug[32] = "PUTC: '";
                char_debug[7] = (c >= 32 && c <= 126) ? c : '?';
                char_debug[8] = '\'';
                char_debug[9] = ' ';
                char_debug[10] = '(';
                char_debug[11] = '0';
                char_debug[12] = 'x';
                format_hex64(char_debug + 13, (uint8_t)c, 2);
                char_debug[15] = ')';
                char_debug[16] = '\0';
                // Wrap debug output to avoid clutter
                int debug_y = DEBUG_Y_START + ((call_count - 1) % 5) * 80 + DEBUG_LINE_HEIGHT;
                framebuffer_draw_string(char_debug, DEBUG_X_MID + 70, debug_y, COLOR_WHITE, 0x00101828);
            }
            
            // Draw the character to the user output area
            if (c == '\n') {
                term_x = USER_OUTPUT_X; // Reset to user output margin
                term_y += 16;
            } else {
                framebuffer_draw_char(c, term_x, term_y, COLOR_WHITE);
                term_x += 8;
            }
            
            // Check horizontal bounds - stay in right half
            if (term_x >= framebuffer_get_width() - 20) {
                term_x = USER_OUTPUT_X;
                term_y += 16;
            }
            // Check vertical bounds and scroll if necessary
            if (term_y >= framebuffer_get_height() - 20) {
                // Reset to the top of the user output area
                term_y = USER_OUTPUT_Y;
                // Clear the right half for scrolling
                framebuffer_draw_rect(USER_OUTPUT_X, USER_OUTPUT_Y - 10, 
                                    framebuffer_get_width() - USER_OUTPUT_X, 
                                    framebuffer_get_height() - USER_OUTPUT_Y + 10, 
                                    0x00101828);
            }
            
            // Set success return value
            frame->rax = 0;
            break;
        }

        // --- NEW: Filesystem Syscall Handlers ---
        case SYS_OPEN: {
            const char *pathname_user = (const char *)frame->rdi;
            char pathname_kernel[256]; // Max path length

            if (copy_string_from_user(pathname_user, pathname_kernel, sizeof(pathname_kernel)) > 0) {
                frame->rax = vfs_open(pathname_kernel);
            } else {
                frame->rax = -1; // Error copying path
            }
            break;
        }

        case SYS_READ: {
            int fd = (int)frame->rdi;
            void *buffer_user = (void *)frame->rsi;
            size_t count = (size_t)frame->rdx;
            frame->rax = vfs_read(fd, buffer_user, count);
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)frame->rdi;
            frame->rax = vfs_close(fd);
            break;
        }

        default:
            // Set error return value for unknown syscalls
            frame->rax = (uint64_t)-1;
            
            // Debug output for unknown syscalls
            if (call_count <= 5) {
                char unknown_msg[32] = "Unknown syscall: 0x";
                format_hex64(unknown_msg + 19, syscall_num, 4);
                framebuffer_draw_string(unknown_msg, DEBUG_X_MID, 
                                       DEBUG_Y_START + (call_count - 1) * 80 + DEBUG_LINE_HEIGHT, 
                                       COLOR_RED, 0x00101828);
            }
            break;
    }
    
    // Show completion status in organized debug area
    if (call_count <= 5) {
        char completion[32] = "Completed with rax=0x";
        format_hex64(completion + 21, frame->rax, 8);
        framebuffer_draw_string(completion, DEBUG_X_MID - 160, 
                               DEBUG_Y_START + 70 + (call_count - 1) * 80 + DEBUG_LINE_HEIGHT * 2, 
                               COLOR_GREEN, 0x00101828);
    }
}