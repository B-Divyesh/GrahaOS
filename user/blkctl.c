// user/blkctl.c — Phase 23 S6.
//
// Block-device CLI. Twin of drvctl (Phase 21). Today it's a minimal
// reporter — connects to /sys/blk/list (published by ahcid) when the
// daemon is up, otherwise prints a friendly message. Future expansions:
// /info <dev> for full IDENTIFY parse, /benchmark <dev> for sequential
// read throughput.

#include "syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

static int strcmp_(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

static void cmd_list(void) {
    cap_token_u_t wr = {.raw = 0};
    cap_token_u_t rd = {.raw = 0};
    long rc = syscall_chan_connect("/sys/blk/list", 17 /* arbitrary */,
                                   &wr, &rd);
    if (rc != 0) {
        printf("blkctl: ahcid is not running (no /sys/blk/list)\n");
        printf("        spawn /bin/ahcid first or boot with autorun=init\n");
        return;
    }
    printf("DEV  MODEL              SECTORS    SIZE    RD_OPS  WR_OPS\n");
    printf("0    QEMU HARDDISK      32768      16 MiB  -       -\n");
    printf("(Phase 23 S6 stub: connection succeeded but the diagnostic\n");
    printf(" protocol stream is not yet implemented in ahcid; see plan.)\n");
}

static void cmd_info(const char *dev_str) {
    (void)dev_str;
    printf("blkctl info: not yet implemented (Phase 23 S6 stub)\n");
}

static void usage(void) {
    printf("Usage: blkctl <command>\n");
    printf("  list         — enumerate block devices (queries /sys/blk/list)\n");
    printf("  info <dev>   — show full IDENTIFY parse for a drive\n");
}

void _start(int argc, char **argv) {
    if (argc < 2) {
        usage();
        syscall_exit(1);
    }
    if (strcmp_(argv[1], "list") == 0) {
        cmd_list();
    } else if (strcmp_(argv[1], "info") == 0) {
        if (argc < 3) { usage(); syscall_exit(1); }
        cmd_info(argv[2]);
    } else {
        usage();
        syscall_exit(1);
    }
    syscall_exit(0);
}
