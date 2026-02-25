// kernel/capability.c
// Phase 8b-i: Capability Activation Network (CAN) - Core Engine
// 3-pass registration compiler, recursive activation, cascade deactivation.
#include "capability.h"
#include "sync/spinlock.h"
#include "../arch/x86_64/drivers/serial/serial.h"

// --- Static string helpers (kernel has no libc) ---

static int cap_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static void cap_strcpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int cap_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void cap_memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)c;
}

// --- Registry ---

static capability_t g_caps[MAX_CAPABILITIES];
static uint32_t g_cap_count = 0;
static spinlock_t g_cap_lock = SPINLOCK_INITIALIZER("cap_lock");

// --- Helper: cap_op_set ---

void cap_op_set(cap_op_t *op, const char *name, uint32_t param_count, uint32_t flags) {
    if (!op || !name) return;
    cap_strcpy(op->name, name, CAP_OP_NAME_LEN);
    op->param_count = param_count;
    op->flags = flags;
}

// --- Internal: Layer validation ---
// Returns 1 if a capability of type `my_type` is allowed to depend on a cap of type `dep_type`.
static int layer_allows_dep(uint32_t my_type, uint32_t dep_type) {
    switch (my_type) {
        case CAP_HARDWARE:
            return 0;  // HARDWARE has no deps at all
        case CAP_DRIVER:
            // DRIVER can depend on HARDWARE or DRIVER (same-layer OK)
            return (dep_type == CAP_HARDWARE || dep_type == CAP_DRIVER);
        case CAP_SERVICE:
            // SERVICE can depend on HARDWARE, DRIVER, or SERVICE (same-layer OK)
            return (dep_type <= CAP_SERVICE);
        case CAP_APPLICATION:
            // APPLICATION can depend on layers 0-2 only (no same-layer)
            return (dep_type < CAP_APPLICATION);
        case CAP_FEATURE:
            // FEATURE can depend on layers 0-3 (no same-layer)
            return (dep_type < CAP_FEATURE);
        case CAP_COMPOSITE:
            // COMPOSITE can depend on layers 0-4 (no same-layer)
            return (dep_type < CAP_COMPOSITE);
        default:
            return 0;
    }
}

// --- Registration (3-pass compiler) ---

int cap_register(const char *name, uint32_t type, int32_t owner,
                 const char **dep_names, int dep_count,
                 int (*activate_fn)(void), void (*deactivate_fn)(void),
                 cap_op_t *ops, int op_count,
                 int (*get_stats)(state_driver_stat_t*, int)) {

    spinlock_acquire(&g_cap_lock);

    // --- PASS 1: Lexical Validation ---

    // Check name is non-NULL and non-empty
    if (!name || name[0] == '\0') {
        serial_write("[CAN] ERR: empty name\n");
        spinlock_release(&g_cap_lock);
        return CAP_ERR_NAME_EMPTY;
    }

    // Check for duplicate name
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (cap_strcmp(g_caps[i].name, name) == 0) {
            serial_write("[CAN] ERR: duplicate name: ");
            serial_write(name);
            serial_write("\n");
            spinlock_release(&g_cap_lock);
            return CAP_ERR_NAME_DUPLICATE;
        }
    }

    // Check registry capacity
    if (g_cap_count >= MAX_CAPABILITIES) {
        serial_write("[CAN] ERR: registry full\n");
        spinlock_release(&g_cap_lock);
        return CAP_ERR_REGISTRY_FULL;
    }

    // Clamp dep_count
    if (dep_count < 0) dep_count = 0;
    if (dep_count > MAX_CAP_DEPS) dep_count = MAX_CAP_DEPS;

    // HARDWARE must have no deps
    if (type == CAP_HARDWARE && dep_count > 0) {
        serial_write("[CAN] ERR: HARDWARE cap has deps: ");
        serial_write(name);
        serial_write("\n");
        spinlock_release(&g_cap_lock);
        return CAP_ERR_HW_HAS_DEPS;
    }

    // --- PASS 2: Dependency Resolution ---

    uint32_t resolved_deps[MAX_CAP_DEPS];
    cap_memset(resolved_deps, 0, sizeof(resolved_deps));

    for (int i = 0; i < dep_count; i++) {
        if (!dep_names || !dep_names[i] || dep_names[i][0] == '\0') {
            serial_write("[CAN] ERR: empty dep name for: ");
            serial_write(name);
            serial_write("\n");
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_UNRESOLVED;
        }

        // Self-dependency check
        if (cap_strcmp(dep_names[i], name) == 0) {
            serial_write("[CAN] ERR: self-dep: ");
            serial_write(name);
            serial_write("\n");
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_SELF;
        }

        // Resolve name to index
        int found = -1;
        for (uint32_t j = 0; j < g_cap_count; j++) {
            if (cap_strcmp(g_caps[j].name, dep_names[i]) == 0) {
                found = (int)j;
                break;
            }
        }

        if (found < 0) {
            serial_write("[CAN] ERR: unresolved dep '");
            serial_write(dep_names[i]);
            serial_write("' for: ");
            serial_write(name);
            serial_write("\n");
            spinlock_release(&g_cap_lock);
            return CAP_ERR_DEP_UNRESOLVED;
        }

        resolved_deps[i] = (uint32_t)found;
    }

    // --- PASS 3: Layer Checking ---

    for (int i = 0; i < dep_count; i++) {
        uint32_t dep_type = g_caps[resolved_deps[i]].type;
        if (!layer_allows_dep(type, dep_type)) {
            serial_write("[CAN] ERR: layer violation: ");
            serial_write(name);
            serial_write(" (type=");
            serial_write_dec(type);
            serial_write(") cannot depend on ");
            serial_write(g_caps[resolved_deps[i]].name);
            serial_write(" (type=");
            serial_write_dec(dep_type);
            serial_write(")\n");
            spinlock_release(&g_cap_lock);
            return CAP_ERR_LAYER_VIOLATION;
        }
    }

    // --- All passes OK: commit to registry ---

    uint32_t id = g_cap_count;
    capability_t *cap = &g_caps[id];
    cap_memset(cap, 0, sizeof(*cap));

    cap_strcpy(cap->name, name, CAP_NAME_LEN);
    cap->type = type;
    cap->owner_pid = owner;
    cap->dep_count = (uint32_t)dep_count;

    for (int i = 0; i < dep_count; i++) {
        cap->dep_indices[i] = resolved_deps[i];
        if (dep_names[i]) {
            cap_strcpy(cap->dep_names[i], dep_names[i], CAP_NAME_LEN);
        }
    }

    cap->activate_fn = activate_fn;
    cap->deactivate_fn = deactivate_fn;
    cap->get_stats_fn = get_stats;

    // Copy operation descriptors
    cap->op_count = (op_count > MAX_CAP_OPS) ? MAX_CAP_OPS : (uint32_t)op_count;
    if (ops) {
        for (uint32_t i = 0; i < cap->op_count; i++) {
            cap_strcpy(cap->ops[i].name, ops[i].name, CAP_OP_NAME_LEN);
            cap->ops[i].param_count = ops[i].param_count;
            cap->ops[i].flags = ops[i].flags;
        }
    }

    // HARDWARE is always ON (represents physical presence)
    if (type == CAP_HARDWARE) {
        cap->state = CAP_STATE_ON;
        cap->activation_count = 1;
    } else {
        cap->state = CAP_STATE_OFF;
    }

    cap->compiled = 1;
    g_cap_count++;

    // Log
    serial_write("[CAN] Registered: ");
    serial_write(name);
    serial_write(" (type=");
    serial_write_dec(type);
    serial_write(", deps=");
    serial_write_dec(dep_count);
    serial_write(", ops=");
    serial_write_dec(cap->op_count);
    serial_write(", state=");
    serial_write(cap->state == CAP_STATE_ON ? "ON" : "OFF");
    serial_write(")\n");

    spinlock_release(&g_cap_lock);
    return (int)id;
}

// --- Activation ---

int cap_activate(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    capability_t *cap = &g_caps[cap_id];

    // Already ON - idempotent
    if (cap->state == CAP_STATE_ON) return CAP_OK;

    // STARTING means we're in a recursive activation chain - runtime cycle
    if (cap->state == CAP_STATE_STARTING) return CAP_ERR_CYCLE;

    // Mark as starting (cycle guard)
    cap->state = CAP_STATE_STARTING;

    // Activate all dependencies first (recursive DFS)
    for (uint32_t i = 0; i < cap->dep_count; i++) {
        int dep_id = (int)cap->dep_indices[i];
        int result = cap_activate(dep_id);
        if (result != CAP_OK) {
            cap->state = CAP_STATE_ERROR;
            cap->error_dep = cap->dep_indices[i];
            serial_write("[CAN] Activation failed for '");
            serial_write(cap->name);
            serial_write("': dep '");
            serial_write(g_caps[dep_id].name);
            serial_write("' failed\n");
            return CAP_ERR_DEP_FAILED;
        }
    }

    // Call activate function if provided
    if (cap->activate_fn) {
        int result = cap->activate_fn();
        if (result != 0) {
            cap->state = CAP_STATE_ERROR;
            serial_write("[CAN] Activation callback failed for '");
            serial_write(cap->name);
            serial_write("'\n");
            return CAP_ERR_ACTIVATE_FAIL;
        }
    }

    // Success
    cap->state = CAP_STATE_ON;
    cap->activation_count++;

    serial_write("[CAN] Activated: ");
    serial_write(cap->name);
    serial_write("\n");

    return CAP_OK;
}

// --- Deactivation ---

int cap_deactivate(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return CAP_ERR_NOT_FOUND;

    capability_t *cap = &g_caps[cap_id];

    // Already OFF - no-op
    if (cap->state == CAP_STATE_OFF) return CAP_OK;

    // First, cascade: deactivate all capabilities that depend on this one
    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (g_caps[i].state != CAP_STATE_ON && g_caps[i].state != CAP_STATE_STARTING)
            continue;
        for (uint32_t j = 0; j < g_caps[i].dep_count; j++) {
            if (g_caps[i].dep_indices[j] == (uint32_t)cap_id) {
                cap_deactivate((int)i);
                break;
            }
        }
    }

    // Call deactivate function if provided
    if (cap->deactivate_fn) {
        cap->deactivate_fn();
    }

    cap->state = CAP_STATE_OFF;

    serial_write("[CAN] Deactivated: ");
    serial_write(cap->name);
    serial_write("\n");

    return CAP_OK;
}

// --- Query Functions ---

int cap_find(const char *name) {
    if (!name || name[0] == '\0') return CAP_ERR_NOT_FOUND;

    for (uint32_t i = 0; i < g_cap_count; i++) {
        if (cap_strcmp(g_caps[i].name, name) == 0) {
            return (int)i;
        }
    }
    return CAP_ERR_NOT_FOUND;
}

int cap_get_count(void) {
    return (int)g_cap_count;
}

int cap_get_state(int cap_id) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) return -1;
    return (int)g_caps[cap_id].state;
}

int cap_why_not(int cap_id, char *buf, int buflen) {
    if (cap_id < 0 || (uint32_t)cap_id >= g_cap_count) {
        if (buf && buflen > 0) cap_strcpy(buf, "not found", buflen);
        return CAP_ERR_NOT_FOUND;
    }

    capability_t *cap = &g_caps[cap_id];

    // Already ON
    if (cap->state == CAP_STATE_ON) {
        if (buf && buflen > 0) cap_strcpy(buf, "already active", buflen);
        return CAP_OK;
    }

    // Check each dep
    int pos = 0;
    int found_issue = 0;
    for (uint32_t i = 0; i < cap->dep_count; i++) {
        uint32_t dep_id = cap->dep_indices[i];
        if (dep_id >= g_cap_count) continue;
        if (g_caps[dep_id].state != CAP_STATE_ON) {
            if (buf && pos < buflen - 1) {
                if (found_issue && pos < buflen - 3) {
                    buf[pos++] = ',';
                    buf[pos++] = ' ';
                }
                const char *prefix = "dep OFF: ";
                if (!found_issue) {
                    for (int k = 0; prefix[k] && pos < buflen - 1; k++)
                        buf[pos++] = prefix[k];
                }
                for (int k = 0; g_caps[dep_id].name[k] && pos < buflen - 1; k++)
                    buf[pos++] = g_caps[dep_id].name[k];
                found_issue = 1;
            }
        }
    }

    if (!found_issue) {
        // All deps are ON but we're still OFF - might be error state
        if (cap->state == CAP_STATE_ERROR) {
            if (buf && buflen > 0) {
                const char *msg = "activation callback failed";
                if (cap->error_dep < g_cap_count) {
                    // A dep failed
                    cap_strcpy(buf, "dep failed: ", buflen);
                    int l = cap_strlen(buf);
                    cap_strcpy(buf + l, g_caps[cap->error_dep].name, buflen - l);
                } else {
                    cap_strcpy(buf, msg, buflen);
                }
            }
            return CAP_ERR_ACTIVATE_FAIL;
        }
        // All deps ON and state is OFF - can be activated
        if (buf && buflen > 0) cap_strcpy(buf, "ready to activate", buflen);
        return CAP_OK;
    }

    if (buf && pos < buflen) buf[pos] = '\0';
    return CAP_ERR_DEP_FAILED;
}

int cap_query_all(state_cap_entry_t *out, int max) {
    if (!out || max <= 0) return 0;

    spinlock_acquire(&g_cap_lock);

    int count = (int)g_cap_count;
    if (count > max) count = max;

    for (int i = 0; i < count; i++) {
        cap_strcpy(out[i].name, g_caps[i].name, STATE_CAP_NAME_LEN);
        out[i].type = g_caps[i].type;
        out[i].state = g_caps[i].state;
        out[i].owner_pid = g_caps[i].owner_pid;
        out[i].dep_count = g_caps[i].dep_count;
        for (uint32_t j = 0; j < g_caps[i].dep_count && j < STATE_MAX_CAP_DEPS; j++) {
            out[i].dep_indices[j] = g_caps[i].dep_indices[j];
        }
        out[i].op_count = g_caps[i].op_count;
        out[i].activation_count = g_caps[i].activation_count;
    }

    spinlock_release(&g_cap_lock);
    return count;
}
