// user/cantest.c
// Phase 8b-ii: Capability Activation Network (CAN) test suite
// 20 automated tests covering registration, activation, cascade, unregister, ownership

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/state.h"

// CAN constants (mirrored from kernel/capability.h)
#define CAP_APPLICATION 3
#define CAP_FEATURE     4
#define CAP_COMPOSITE   5

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, condition) do { \
    if (condition) { \
        printf("[PASS] %s\n", name); \
        tests_passed++; \
    } else { \
        printf("[FAIL] %s\n", name); \
        tests_failed++; \
    } \
} while(0)

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
void test_registration(void) {
    printf("\n=== Group 1: Registration Compiler ===\n");
    int ret;

    // Test 1: Register APPLICATION cap, no deps
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TEST("1. Register APPLICATION (no deps)", ret >= 0);

    // Test 2: Register FEATURE cap, dep on cantest_app
    const char *deps_app[] = {"cantest_app"};
    ret = syscall_cap_register("cantest_feat", CAP_FEATURE, deps_app, 1);
    TEST("2. Register FEATURE (dep on app)", ret >= 0);

    // Test 3: Duplicate name
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TEST("3. Duplicate name rejected (-2)", ret == -2);

    // Test 4: Empty name
    ret = syscall_cap_register("", CAP_APPLICATION, NULL, 0);
    TEST("4. Empty name rejected (-1)", ret == -1);

    // Test 5: Register HARDWARE from user-space (layer violation)
    ret = syscall_cap_register("cantest_hw", 0, NULL, 0);
    TEST("5. HARDWARE from user rejected (-5)", ret == -5);

    // Test 6: Register DRIVER from user-space (layer violation)
    ret = syscall_cap_register("cantest_drv", 1, NULL, 0);
    TEST("6. DRIVER from user rejected (-5)", ret == -5);

    // Test 7: Unresolved dep name
    const char *bad_deps[] = {"nonexistent_cap_xyz"};
    ret = syscall_cap_register("cantest_bad", CAP_APPLICATION, bad_deps, 1);
    TEST("7. Unresolved dep rejected (-3)", ret == -3);

    // Test 8: Self-dependency
    const char *self_deps[] = {"cantest_self"};
    ret = syscall_cap_register("cantest_self", CAP_APPLICATION, self_deps, 1);
    TEST("8. Self-dep rejected (-4)", ret == -4);
}

// =====================================================
// Group 2: Activation / Deactivation (4 tests)
// =====================================================
void test_activation(void) {
    printf("\n=== Group 2: Activation / Deactivation ===\n");
    int ret;

    // Test 9: Activate APPLICATION cap
    ret = syscall_cap_activate("cantest_app");
    TEST("9. Activate cantest_app", ret == 0);

    // Test 10: Verify ON via state query
    int state = find_cap_state("cantest_app");
    TEST("10. cantest_app state is ON (2)", state == 2);

    // Test 11: Deactivate APPLICATION cap
    ret = syscall_cap_deactivate("cantest_app");
    TEST("11. Deactivate cantest_app", ret == 0);

    // Test 12: Verify OFF
    state = find_cap_state("cantest_app");
    TEST("12. cantest_app state is OFF (0)", state == 0);
}

// =====================================================
// Group 3: Cascade (3 tests)
// =====================================================
void test_cascade(void) {
    printf("\n=== Group 3: Cascade ===\n");
    int ret;

    // Test 13: Activate FEATURE (auto-activates APPLICATION dep)
    ret = syscall_cap_activate("cantest_feat");
    TEST("13. Activate cantest_feat (cascades)", ret == 0);

    int app_state = find_cap_state("cantest_app");
    int feat_state = find_cap_state("cantest_feat");
    // Both should be ON now (feat depends on app, so activating feat activates app first)
    TEST("14. Both app and feat are ON", app_state == 2 && feat_state == 2);

    // Test 15: Deactivate APPLICATION -> cascades FEATURE OFF
    ret = syscall_cap_deactivate("cantest_app");
    app_state = find_cap_state("cantest_app");
    feat_state = find_cap_state("cantest_feat");
    TEST("15. Deactivate app cascades feat OFF", app_state == 0 && feat_state == 0);
}

// =====================================================
// Group 4: Unregistration (3 tests)
// =====================================================
void test_unregistration(void) {
    printf("\n=== Group 4: Unregistration ===\n");
    int ret;

    // Test 16: Unregister APPLICATION cap
    // First unregister the feature (depends on app), then app
    ret = syscall_cap_unregister("cantest_feat");
    TEST("16a. Unregister cantest_feat", ret == 0);

    ret = syscall_cap_unregister("cantest_app");
    TEST("16b. Unregister cantest_app", ret == 0);

    // Test 17: Verify deleted (activate returns error)
    ret = syscall_cap_activate("cantest_app");
    TEST("17. Activate after unregister fails", ret != 0);

    // Test 18: Re-register with same name (slot was deleted)
    ret = syscall_cap_register("cantest_app", CAP_APPLICATION, NULL, 0);
    TEST("18. Re-register same name succeeds", ret >= 0);

    // Clean up for next group
    syscall_cap_unregister("cantest_app");
}

// =====================================================
// Group 5: Ownership (2 tests)
// =====================================================
void test_ownership(void) {
    printf("\n=== Group 5: Ownership ===\n");
    int ret;

    // Register a cap to check ownership
    ret = syscall_cap_register("cantest_owned", CAP_APPLICATION, NULL, 0);
    TEST("19. Register owned cap", ret >= 0);

    // Test 20: Try unregister kernel cap "cpu" -> rejected
    ret = syscall_cap_unregister("cpu");
    TEST("20. Unregister kernel 'cpu' rejected", ret < 0);

    // Clean up
    syscall_cap_unregister("cantest_owned");
}

// =====================================================
// Main
// =====================================================
void _start(void) {
    printf("=== CAN Test Suite (Phase 8b-ii) ===\n");
    printf("Testing Capability Activation Network...\n");
    printf("PID: %d\n", syscall_getpid());

    test_registration();
    test_activation();
    test_cascade();
    test_unregistration();
    test_ownership();

    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d/21\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("\nAll tests PASSED!\n");
    } else {
        printf("\nSome tests FAILED.\n");
    }

    syscall_exit(tests_failed);
}
