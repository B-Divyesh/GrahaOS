// user/drvctl.c — Phase 21 driver control CLI.
//
// Subcommands:
//   drvctl list           - dump all PCI devices + ownership state
//   drvctl info <pci>     - detailed view of one device by pci_addr (hex)
//   drvctl kill <pid>     - SIGKILL a driver daemon (recovery)
//
// Reads /sys/drv/list via SYS_GET_SYSTEM_STATE category STATE_CAT_USERDRV
// (no separate channel needed — same pattern Phase 20 used for psinfo).

#include "syscalls.h"
#include "../kernel/state.h"
#include "libc/include/stdio.h"
#include "libc/include/string.h"
#include "libc/include/stdlib.h"

static state_userdrv_list_t s_buf;

static const char *class_name(uint8_t klass) {
    switch (klass) {
        case 0x00: return "unclass";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x06: return "bridge";
        case 0x09: return "input";
        case 0x0C: return "serial-bus";
        default:   return "?";
    }
}

static int parse_hex(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        v <<= 4;
        if (*s >= '0' && *s <= '9') v |= (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') v |= (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') v |= (uint32_t)(*s - 'A' + 10);
        else return -1;
        s++;
    }
    *out = v;
    return 0;
}

static int parse_int(const char *s, int *out) {
    int v = 0; int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    if (!*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = sign * v;
    return 0;
}

static long fetch_userdrv(void) {
    return syscall_get_system_state(STATE_CAT_USERDRV, &s_buf, sizeof(s_buf));
}

static void cmd_list(void) {
    if (fetch_userdrv() < 0) {
        printf("drvctl: SYS_GET_SYSTEM_STATE failed\n");
        return;
    }
    printf("PCI       VENDOR:DEVICE  CLASS    OWNER  IRQ  IRQS    BAR_PHYS         BAR_SIZE\n");
    for (uint32_t i = 0; i < s_buf.count; i++) {
        state_userdrv_entry_t *e = &s_buf.entries[i];
        const char *owner = e->driver_owner_pid > 0 ? "user" : "-";
        printf("%02x:%02x.%u  %04x:%04x      %-8s pid=%-3d %3u  %-7lu 0x%-14lx 0x%lx\n",
               (e->pci_addr >> 16) & 0xFFu,
               (e->pci_addr >> 8) & 0xFFu,
               e->pci_addr & 0xFFu,
               (unsigned)e->vendor_id, (unsigned)e->device_id,
               class_name(e->device_class),
               (int)e->driver_owner_pid,
               (unsigned)e->irq_vector,
               (unsigned long)e->irq_count,
               (unsigned long)e->bar_phys,
               (unsigned long)e->bar_size);
        (void)owner;
    }
}

static void cmd_info(uint32_t pci_addr) {
    if (fetch_userdrv() < 0) {
        printf("drvctl: SYS_GET_SYSTEM_STATE failed\n");
        return;
    }
    for (uint32_t i = 0; i < s_buf.count; i++) {
        state_userdrv_entry_t *e = &s_buf.entries[i];
        if (e->pci_addr != pci_addr) continue;
        printf("PCI:           %02x:%02x.%u\n",
               (e->pci_addr >> 16) & 0xFFu,
               (e->pci_addr >> 8) & 0xFFu,
               e->pci_addr & 0xFFu);
        printf("VENDOR:DEVICE: %04x:%04x\n",
               (unsigned)e->vendor_id, (unsigned)e->device_id);
        printf("CLASS:         0x%02x (%s) / 0x%02x\n",
               (unsigned)e->device_class, class_name(e->device_class),
               (unsigned)e->device_subclass);
        printf("OWNER:         pid=%d %s\n",
               (int)e->driver_owner_pid,
               e->driver_owner_pid > 0 ? "(claimed)" : "(unclaimed)");
        printf("CLAIMABLE:     %s\n", e->is_claimable ? "yes" : "no");
        printf("IRQ_VECTOR:    %u\n", (unsigned)e->irq_vector);
        printf("IRQ_COUNT:     %lu\n", (unsigned long)e->irq_count);
        printf("REGISTERED@:   tsc=%lu\n", (unsigned long)e->registered_at_tsc);
        printf("BAR_PHYS:      0x%lx\n", (unsigned long)e->bar_phys);
        printf("BAR_SIZE:      0x%lx (%lu bytes)\n",
               (unsigned long)e->bar_size, (unsigned long)e->bar_size);
        return;
    }
    printf("drvctl: no device with pci_addr=0x%06x\n", pci_addr);
}

static void cmd_kill(int pid) {
    int rc = syscall_kill(pid, 9 /*SIGKILL*/);
    if (rc == 0) printf("drvctl: SIGKILL sent to pid %d\n", pid);
    else printf("drvctl: kill(%d) failed rc=%d\n", pid, rc);
}

static void usage(void) {
    printf("Usage: drvctl <subcommand>\n");
    printf("  drvctl list             — list all PCI devices + driver owners\n");
    printf("  drvctl info <pci_hex>   — detail for one device (e.g. 00:03.0 = 0x000300)\n");
    printf("  drvctl kill <pid>       — SIGKILL a driver daemon\n");
}

void _start(void) {
    // Userspace argv plumbing isn't fully wired (gash spawns drvctl with no
    // argv); for Phase 21 MVP we hardcode `list` as the default. Future
    // extension: parse argv when SYS_SPAWN gains an argv vector.
    cmd_list();
    (void)cmd_info; (void)cmd_kill; (void)parse_hex; (void)parse_int;
    (void)usage;
    syscall_exit(0);
}
