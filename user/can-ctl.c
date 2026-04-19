// user/can-ctl.c
//
// Phase 16: /bin/can-ctl — a small capability-table printer. Because the
// Phase 16 spawn path does not yet deliver argv to user programs (Phase 17
// channels + VMOs land that), `/bin/can-ctl` is argv-less and always prints
// the full CAN capability table. Mutation operations (`activate`,
// `deactivate`, `why-not`) live as gash builtins that call SYS_CAN_LOOKUP +
// SYS_CAN_{ACTIVATE,DEACTIVATE}_T directly.
//
// Output columns: NAME, TYPE, STATE, OWNER, ACTIVATIONS, DEPS.

#include <stdint.h>
#include "syscalls.h"
#include "../libc/include/stdio.h"
#include "../libc/include/stdlib.h"
#include "../libc/include/string.h"
#include "../kernel/state.h"

static const char *state_name(uint32_t s) {
    switch (s) {
        case 0: return "OFF";
        case 1: return "STARTING";
        case 2: return "ON";
        case 3: return "ERROR";
    }
    return "?";
}
static const char *type_name(uint32_t t) {
    switch (t) {
        case 0: return "HW";
        case 1: return "DRIVER";
        case 2: return "SERVICE";
        case 3: return "APP";
        case 4: return "FEATURE";
        case 5: return "COMPOSITE";
    }
    return "?";
}

void _start(void) {
    state_cap_list_t caps;
    long rc = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (rc < 0) {
        printf("can-ctl: SYS_GET_SYSTEM_STATE failed rc=%ld\n", rc);
        exit(1);
    }

    printf("%-22s %-9s %-8s %-7s %-5s %s\n",
           "NAME", "TYPE", "STATE", "OWNER", "ACTs", "DEPS");
    printf("---------------------- --------- -------- ------- ----- ----\n");
    for (uint32_t i = 0; i < caps.count; i++) {
        const state_cap_entry_t *c = &caps.caps[i];
        if (c->deleted) continue;
        printf("%-22s %-9s %-8s ",
               c->name, type_name(c->type), state_name(c->state));
        if (c->owner_pid < 0) printf("%-7s", "kernel");
        else printf("%-7d", c->owner_pid);
        printf(" %-5lu ", (unsigned long)c->activation_count);
        if (c->dep_count == 0) printf("-");
        else {
            for (uint32_t d = 0; d < c->dep_count; d++) {
                uint32_t di = c->dep_indices[d];
                if (di < caps.count) printf("%s%s", d ? "," : "", caps.caps[di].name);
            }
        }
        printf("\n");
    }
    exit(0);
}
