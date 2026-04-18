// user/tests/cantest.c
// Phase 12: TAP 1.4 port of user/cantest.c (Phase 8b-ii CAN tests).

#include "../libtap.h"
#include "../syscalls.h"
#include "../../kernel/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// CAN constants (mirrored from kernel/capability.h)
#define CAP_APPLICATION 3
#define CAP_FEATURE     4
#define CAP_COMPOSITE   5

// Helper: query capabilities and find one by name
static int find_cap_state(const char *name) {
    state_cap_list_t caps;
    long ret = syscall_get_system_state(STATE_CAT_CAPABILITIES, &caps, sizeof(caps));
    if (ret < 0) return -1;

    for (uint32_t i = 0; i < caps.count; i++) {
        if (strcmp(caps.caps[i].name, name) == 0 && !caps.caps[i].deleted) {
            return (int)caps.caps[i].state;
        }
    }
    return -1;  // not found
}

// =====================================================
// Group 1: Registration Compiler (8 tests)
// =====================================================
static void test_registration(void) {
    printf("\n=== Group 1: Registration Compiler ===\n");
    int ret;

    // Test 1: Register APPLICATION cap, no deps
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TAP_ASSERT(ret >= 0, "1. Register APPLICATION (no deps)");

    // Test 2: Register FEATURE cap, dep on cantest_app
    const char *deps_app[] = {"cantest_app"};
    ret = syscall_cap_register("cantest_feat", CAP_FEATURE, deps_app, 1);
    TAP_ASSERT(ret >= 0, "2. Register FEATURE (dep on app)");

    // Test 3: Duplicate name
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TAP_ASSERT(ret == -2, "3. Duplicate name rejected (-2)");

    // Test 4: Empty name
    ret = syscall_cap_register("", CAP_APPLICATION, NULL, 0);
    TAP_ASSERT(ret == -1, "4. Empty name rejected (-1)");

    // Test 5: Register HARDWARE from user-space (layer violation)
    ret = syscall_cap_register("cantest_hw", 0, NULL, 0);
    TAP_ASSERT(ret == -5, "5. HARDWARE from user rejected (-5)");

    // Test 6: Register DRIVER from user-space (layer violation)
    ret = syscall_cap_register("cantest_drv", 1, NULL, 0);
    TAP_ASSERT(ret == -5, "6. DRIVER from user rejected (-5)");

    // Test 7: Unresolved dep name
    const char *bad_deps[] = {"nonexistent_cap_xyz"};
    ret = syscall_cap_register("cantest_bad", CAP_APPLICATION, bad_deps, 1);
    TAP_ASSERT(ret == -3, "7. Unresolved dep rejected (-3)");

    // Test 8: Self-dependency
    const char *self_deps[] = {"cantest_self"};
    ret = syscall_cap_register("cantest_self", CAP_APPLICATION, self_deps, 1);
    TAP_ASSERT(ret == -4, "8. Self-dep rejected (-4)");
}

// =====================================================
// Group 2: Activation / Deactivation (4 tests)
// =====================================================
static void test_activation(void) {
    printf("\n=== Group 2: Activation / Deactivation ===\n");
    int ret;

    // Test 9: Activate APPLICATION cap
    ret = syscall_cap_activate("cantest_app");
    TAP_ASSERT(ret == 0, "9. Activate cantest_app");

    // Test 10: Verify ON via state query
    int state = find_cap_state("cantest_app");
    TAP_ASSERT(state == 2, "10. cantest_app state is ON (2)");

    // Test 11: Deactivate APPLICATION cap
    ret = syscall_cap_deactivate("cantest_app");
    TAP_ASSERT(ret == 0, "11. Deactivate cantest_app");

    // Test 12: Verify OFF
    state = find_cap_state("cantest_app");
    TAP_ASSERT(state == 0, "12. cantest_app state is OFF (0)");
}

// =====================================================
// Group 3: Cascade (3 tests)
// =====================================================
static void test_cascade(void) {
    printf("\n=== Group 3: Cascade ===\n");
    int ret;

    // Test 13: Activate FEATURE (auto-activates APPLICATION dep)
    ret = syscall_cap_activate("cantest_feat");
    TAP_ASSERT(ret == 0, "13. Activate cantest_feat (cascades)");

    int app_state = find_cap_state("cantest_app");
    int feat_state = find_cap_state("cantest_feat");
    TAP_ASSERT(app_state == 2 && feat_state == 2, "14. Both app and feat are ON");

    // Test 15: Deactivate APPLICATION -> cascades FEATURE OFF
    ret = syscall_cap_deactivate("cantest_app");
    app_state = find_cap_state("cantest_app");
    feat_state = find_cap_state("cantest_feat");
    TAP_ASSERT(app_state == 0 && feat_state == 0, "15. Deactivate app cascades feat OFF");
}

// =====================================================
// Group 4: Unregistration (3 "tests" expanding to 4 TAP_ASSERT calls)
// =====================================================
static void test_unregistration(void) {
    printf("\n=== Group 4: Unregistration ===\n");
    int ret;

    // Test 16a: Unregister cantest_feat
    ret = syscall_cap_unregister("cantest_feat");
    TAP_ASSERT(ret == 0, "16a. Unregister cantest_feat");

    // Test 16b: Unregister cantest_app
    ret = syscall_cap_unregister("cantest_app");
    TAP_ASSERT(ret == 0, "16b. Unregister cantest_app");

    // Test 17: Verify deleted (activate returns error)
    ret = syscall_cap_activate("cantest_app");
    TAP_ASSERT(ret != 0, "17. Activate after unregister fails");

    // Test 18: Re-register with same name (slot was deleted)
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TAP_ASSERT(ret >= 0, "18. Re-register same name succeeds");

    // Clean up for next group
    syscall_cap_unregister("cantest_app");
}

// =====================================================
// Group 5: Ownership (2 tests)
// =====================================================
static void test_ownership(void) {
    printf("\n=== Group 5: Ownership ===\n");
    int ret;

    // Register a cap to check ownership
    ret = syscall_cap_register("cantest_owned", CAP_APPLICATION, NULL, 0);
    TAP_ASSERT(ret >= 0, "19. Register owned cap");

    // Test 20: Try unregister kernel cap "cpu" -> rejected
    ret = syscall_cap_unregister("cpu");
    TAP_ASSERT(ret < 0, "20. Unregister kernel 'cpu' rejected");

    // Clean up
    syscall_cap_unregister("cantest_owned");
}

void _start(void) {
    printf("=== CAN Test Suite (Phase 8b-ii) ===\n");
    printf("Testing Capability Activation Network...\n");
    printf("PID: %d\n", syscall_getpid());

    // Total: 8 + 4 + 3 + 4 + 2 = 21
    tap_plan(21);

    test_registration();
    test_activation();
    test_cascade();
    test_unregistration();
    test_ownership();

    tap_done();
    exit(0);
}
