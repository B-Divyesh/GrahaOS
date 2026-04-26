// user/libdriver.c — Phase 21 userspace driver helper library.

#include "libdriver.h"
#include "syscalls.h"

// libc — linked separately into each daemon binary.
extern int printf(const char *fmt, ...);
extern void *sys_vmo_map_user(uint64_t handle, uint64_t addr_hint,
                              uint64_t offset, uint64_t len, uint32_t prot);

// SYS_VMO_CREATE / SYS_VMO_MAP wrappers (mirror of phase 17 user wrappers).
// We re-declare them here to keep libdriver self-contained.
static inline long lvmo_create(uint64_t size_bytes, uint32_t flags) {
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_CREATE), "D"(size_bytes), "S"((uint64_t)flags)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t lvmo_map(uint64_t handle, uint64_t addr_hint,
                                uint64_t offset, uint64_t len, uint32_t prot) {
    long ret;
    register uint64_t r10 asm("r10") = len;
    register uint64_t r8  asm("r8")  = (uint64_t)prot;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_VMO_MAP), "D"(handle), "S"(addr_hint), "d"(offset),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (uint64_t)ret;
}

int drv_register(uint16_t vendor_id, uint16_t device_id,
                 uint8_t device_class, drv_caps_t *out) {
    if (!out) return -14;  // -EFAULT
    long rc = syscall_drv_register(vendor_id, device_id, device_class, out);
    return (int)rc;
}

long drv_irq_wait(uint64_t irq_handle, drv_irq_msg_t *out_msgs,
                  uint32_t max_msgs, uint32_t timeout_ms) {
    return syscall_drv_irq_wait(irq_handle, out_msgs, max_msgs, timeout_ms);
}

void *drv_mmio_map(uint64_t phys, uint64_t size, uint32_t prot) {
    long handle = syscall_mmio_vmo_create(phys, size, 0);
    if (handle < 0) return (void *)0;
    uint64_t va = lvmo_map((uint64_t)handle, 0, 0, size, prot);
    // Phase 21.1: sys_vmo_map returns a negative errno on failure (not 0).
    // Detect by sign-bit on the cast; addresses are < 2^63 so positive.
    if (va == 0 || (int64_t)va < 0) return (void *)0;
    return (void *)(uintptr_t)va;
}

#define VMO_CONTIGUOUS_U  0x20u

void *drv_dma_alloc(uint32_t npages, uint64_t *phys_out) {
    return drv_dma_alloc_ex(npages, phys_out, (cap_token_u_t *)0);
}

void *drv_dma_alloc_ex(uint32_t npages, uint64_t *phys_out,
                       cap_token_u_t *handle_out) {
    if (npages == 0 || npages > 64 || !phys_out) return (void *)0;
    uint64_t bytes = (uint64_t)npages * 4096ull;
    long handle = lvmo_create(bytes, VMO_CONTIGUOUS_U | VMO_ZEROED | VMO_PINNED);
    if (handle < 0) return (void *)0;
    uint64_t va = lvmo_map((uint64_t)handle, 0, 0, bytes,
                           PROT_READ | PROT_WRITE);
    if (va == 0 || (int64_t)va < 0) return (void *)0;
    // Discover the physical base via PHYS_QUERY for page 0.
    uint64_t phys = 0;
    long pq = syscall_vmo_phys((uint64_t)handle, 0, &phys);
    if (pq < 0 || phys == 0) {
        // Caller cannot reliably program DMA descriptors without the phys
        // address. Map remains live (no easy unmap-by-handle here in
        // libdriver MVP); caller should treat as a fatal startup error.
        *phys_out = 0;
        return (void *)0;
    }
    *phys_out = phys;
    if (handle_out) handle_out->raw = (uint64_t)handle;
    return (void *)(uintptr_t)va;
}

// Reads our pledge mask via the existing DEBUG_READ_PLEDGE op (Phase 15b).
// Available in WITH_DEBUG_SYSCALL builds; otherwise returns 0xFFFF (assume OK).
static inline uint16_t drv_read_pledge(void) {
#ifdef WITH_DEBUG_SYSCALL
    long ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(SYS_DEBUG), "D"((uint64_t)DEBUG_READ_PLEDGE)
        : "rcx", "r11", "memory");
    return (uint16_t)ret;
#else
    return 0xFFFFu;
#endif
}

void drv_self_pledge_check(uint16_t required_pledge) {
    uint16_t have = drv_read_pledge();
    if ((have & required_pledge) != required_pledge) {
        printf("[libdriver] FATAL: missing pledge bits (have=0x%04x need=0x%04x)\n",
               (unsigned)have, (unsigned)required_pledge);
        syscall_exit(99);
    }
}
