// kernel/autorun.h
// Phase 12: PID-1 selection and init-exit hook.
//
// When the kernel command line specifies `autorun=<name>`, `<name>`
// replaces `gash` as the first user-space process. When that first
// process exits, autorun_on_init_exit() is called from the SYS_EXIT
// handler; in later work units it triggers kernel_shutdown() so QEMU
// powers off cleanly at the end of `make test`.

#ifndef GRAHAOS_AUTORUN_H
#define GRAHAOS_AUTORUN_H

#include <stdbool.h>

// Path inside the initrd (TAR archive) to spawn as PID 1. If the kernel
// command line set `autorun=NAME`, returns `bin/NAME`; otherwise the
// historical default `bin/gash`. Returned pointer references a static
// buffer owned by this module — stable for the lifetime of the kernel.
const char *autorun_decide(void);

// Register the PID assigned to the first user-space process. Called
// once from kmain() right after sched_create_user_process() returns.
void autorun_register_init_pid(int pid);

// Read the previously-registered init PID, or -1 if never set.
int autorun_get_init_pid(void);

// True iff the kernel command line explicitly set `autorun=<name>`.
// Used by SYS_EXIT to decide whether to shut the machine down when PID
// 1 exits, and by the watchdog to decide whether to arm.
bool autorun_is_active(void);

// Hook invoked from SYS_EXIT when a process exits. If the exiting PID
// is the init PID and autorun is active, later work units will call
// kernel_shutdown() from here. For now it only logs.
void autorun_on_init_exit(int pid, int status);

#endif
