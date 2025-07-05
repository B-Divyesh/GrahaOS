#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "limine.h"
#include "initrd.h"
#include "elf.h"
#include "fs/vfs.h"
#include "gcp.h" // <-- ADDED
#include "../drivers/video/framebuffer.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/mm/pmm.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/cpu/sched/sched.h"
#include "../arch/x86_64/cpu/syscall/syscall.h"
#include "../arch/x86_64/drivers/timer/pit.h"
#include "../arch/x86_64/cpu/interrupts.h"

// --- Limine Requests (no changes) ---
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = NULL };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = NULL };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST, .revision = 0, .response = NULL };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = { .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = NULL };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = { .id = LIMINE_MODULE_REQUEST, .revision = 0, .response = NULL };

// --- Standard C functions (no changes) ---
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) pdest[i] = psrc[i];
    return dest;
}
void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    if (src > dest) for (size_t i = 0; i < n; i++) pdest[i] = psrc[i];
    else if (src < dest) for (size_t i = n; i > 0; i--) pdest[i-1] = psrc[i-1];
    return dest;
}
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) if (p1[i] != p2[i]) return p1[i] < p2[i] ? -1 : 1;
    return 0;
}

static void uint_to_string(uint64_t value, char *buffer) {
    if (value == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    char temp[21]; int i = 0;
    while (value > 0) { temp[i++] = '0' + (value % 10); value /= 10; }
    int j; for (j = 0; j < i; j++) buffer[j] = temp[i - 1 - j];
    buffer[j] = '\0';
}

static void hex_to_string(uint64_t value, char *buffer) {
    const char hex_chars[] = "0123456789ABCDEF";
    char temp[17]; int i = 0;
    if (value == 0) { buffer[0] = '0'; buffer[1] = '\0'; return; }
    while (value > 0) { temp[i++] = hex_chars[value & 0xF]; value >>= 4; }
    int j; for (j = 0; j < i; j++) buffer[j] = temp[i - 1 - j];
    buffer[j] = '\0';
}

static void hcf(void) { asm ("cli"); for (;;) { asm ("hlt"); } }

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) hcf();
    if (framebuffer_request.response == NULL || memmap_request.response == NULL ||
        executable_address_request.response == NULL || hhdm_request.response == NULL ||
        module_request.response == NULL) hcf();

    if (!framebuffer_init(&framebuffer_request)) hcf();

    framebuffer_clear(0x00101828);

    // UPDATED: Banner for Phase 6b
    framebuffer_draw_rect(50, 50, 600, 140, COLOR_GRAHA_BLUE);
    framebuffer_draw_rect(52, 52, 596, 136, 0x00004488);
    framebuffer_draw_rect(54, 54, 592, 132, 0x000066AA);
    framebuffer_draw_string("GrahaOS - Phase 6b: Local GCP Interpreter", 70, 70, COLOR_WHITE, 0x000066AA);
    framebuffer_draw_string("Executing multi-step plan from /etc/plan.json", 70, 90, COLOR_LIGHT_GRAY, 0x000066AA);
    framebuffer_draw_rect_outline(40, 40, 620, 160, COLOR_WHITE);
    framebuffer_draw_rect_outline(42, 42, 616, 156, COLOR_LIGHT_GRAY);

    int y_pos = 220;

    // --- INITIALIZATION SEQUENCE ---
    uint64_t kernel_phys_base = executable_address_request.response->physical_base;
    uint64_t kernel_virt_base = executable_address_request.response->virtual_base;
    uint64_t hhdm_offset = hhdm_request.response->offset;

    pmm_init(memmap_request.response);
    framebuffer_draw_string("PMM Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    vmm_init(memmap_request.response, framebuffer_request.response, kernel_phys_base, kernel_virt_base, hhdm_offset);
    framebuffer_init(&framebuffer_request);
    framebuffer_draw_string("VMM Initialized. Paging is now active!", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    gdt_init();
    framebuffer_draw_string("GDT & TSS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    idt_init();
    framebuffer_draw_string("IDT Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    sched_init();
    framebuffer_draw_string("Scheduler Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    syscall_init();
    framebuffer_draw_string("Syscall Interface Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;
    vfs_init();
    framebuffer_draw_string("VFS Initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 40;

    // --- PHASE 6b: ELF Loading and Execution ---
    framebuffer_draw_string("=== Phase 6b: Loading GCP Interpreter ===", 50, y_pos, COLOR_WHITE, 0x00101828); y_pos += 30;

    initrd_init(&module_request);
    framebuffer_draw_string("Initrd initialized.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;

    size_t grahai_size;
    void *grahai_data = initrd_lookup("bin/grahai", &grahai_size);
    if (!grahai_data) {
        framebuffer_draw_string("FATAL: Could not find bin/grahai in initrd!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("Found bin/grahai in initrd.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;

    uint64_t entry_point, cr3;
    if (!elf_load(grahai_data, &entry_point, &cr3)) {
        framebuffer_draw_string("FATAL: Failed to load ELF file!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("ELF loaded successfully into memory.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 20;

    int process_id = sched_create_user_process(entry_point, cr3);
    if (process_id == -1) {
        framebuffer_draw_string("FATAL: Failed to create user process!", 50, y_pos, COLOR_RED, 0x00101828);
        hcf();
    }
    framebuffer_draw_string("User process created.", 50, y_pos, COLOR_GREEN, 0x00101828); y_pos += 30;

    framebuffer_draw_string("=== Starting Process Execution ===", 50, y_pos, COLOR_WHITE, 0x00101828); y_pos += 20;

    framebuffer_draw_string("User program will now execute the plan...", 50, y_pos, COLOR_YELLOW, 0x00101828);

    pit_init(100);
    irq_init();
    
    while (1) {
        asm ("hlt");
    }
}