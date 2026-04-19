// user/caps.c — Phase 15a: /bin/caps capability inspector.
//
// Lists every registered CAN capability plus its paired cap_object_t
// backing (via SYS_GET_SYSTEM_STATE + SYS_CAP_INSPECT where possible).
// Per-command sub-modes (inspect/tree/derive/revoke) are deferred to
// Phase 16 when argv-over-spawn lands; for Phase 15a the binary is a
// read-only snapshot, matching the memstat model.
//
// Output format (default, human-readable):
//     [ON ] name=<n> kind=CAN type=<t> deps=<d> gen=<g> owner=<p> flags=<hex>
// One line per cap. Pipeable through gash's existing machinery.

#include <stdint.h>
#include <stdlib.h>
#include "syscalls.h"
#include "../libc/include/stdio.h"
#include "../libc/include/string.h"
#include "../kernel/state.h"

static const char *type_name(uint32_t t) {
    switch (t) {
    case 0: return "HW";
    case 1: return "DRV";
    case 2: return "SVC";
    case 3: return "APP";
    case 4: return "FEAT";
    case 5: return "COMP";
    default: return "?";
    }
}

static const char *state_name(uint32_t s) {
    switch (s) {
    case 0: return "OFF";
    case 1: return "STARTING";
    case 2: return "ON";
    case 3: return "ERROR";
    default: return "?";
    }
}

void _start(void) {
    state_cap_list_t list;
    long r = syscall_get_system_state(STATE_CAT_CAPABILITIES, &list, sizeof(list));
    if (r < 0 || list.count == 0) {
        printf("caps: no capabilities registered (state rc=%ld)\n", r);
        exit(0);
    }

    printf("GrahaOS capabilities (count=%u):\n", (unsigned)list.count);
    printf("  state  type  name                             deps  activations owner\n");
    printf("  -----  ----  -------------------------------  ----  ----------- -----\n");

    for (uint32_t i = 0; i < list.count && i < STATE_MAX_CAPS; i++) {
        state_cap_entry_t *e = &list.caps[i];
        if (e->deleted) continue;
        printf("  %-5s  %-4s  %-31s  %4u  %11lu ",
               state_name(e->state),
               type_name(e->type),
               e->name,
               (unsigned)e->dep_count,
               (unsigned long)e->activation_count);
        if (e->owner_pid < 0) printf("kern\n");
        else printf("%d\n", e->owner_pid);
    }

    // Phase 15a: bootstrap caps are paired with cap_object_t entries via
    // CAP_KIND_CAN / CAP_FLAG_IMMORTAL | CAP_FLAG_PUBLIC. We can
    // SYS_CAP_INSPECT against a synthetic token {gen=1, idx=1.., flags=0}
    // — but userspace doesn't yet have a canonical way to enumerate
    // cap_object idx ranges. Future phases (when gcp.json exposes this)
    // will add token-based inspection here.

    exit(0);
}
