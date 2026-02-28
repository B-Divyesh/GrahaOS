// user/eventtest.c
// Phase 8d: CAN Event Propagation test suite
// 12 automated tests covering watch/unwatch, event delivery, edge cases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "syscalls.h"
#include "../kernel/state.h"

// CAN constants (mirrored from kernel/capability.h)
#define CAP_APPLICATION 3
#define CAP_FEATURE     4

// Error codes (mirrored)
#define CAP_ERR_NOT_FOUND     -8
#define CAP_ERR_ALREADY_WATCH -16
#define CAP_ERR_NOT_WATCHING  -17

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

// Helper: zero memory
static void zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

// =====================================================
// Group 1: Watch/Unwatch (4 tests)
// =====================================================
void test_watch_unwatch(void) {
    printf("\n=== Group 1: Watch/Unwatch ===\n");
    int ret;

    // Test 1: Watch a known kernel capability (serial)
    ret = syscall_cap_watch("serial_output");
    TEST("1. Watch 'serial' succeeds", ret == 0);

    // Test 2: Watch same cap again → duplicate error
    ret = syscall_cap_watch("serial_output");
    TEST("2. Duplicate watch rejected (-16)", ret == CAP_ERR_ALREADY_WATCH);

    // Test 3: Unwatch succeeds
    ret = syscall_cap_unwatch("serial_output");
    TEST("3. Unwatch 'serial' succeeds", ret == 0);

    // Test 4: Unwatch when not watching → error
    ret = syscall_cap_unwatch("serial_output");
    TEST("4. Unwatch when not watching (-17)", ret == CAP_ERR_NOT_WATCHING);
}

// =====================================================
// Group 2: Event Delivery (4 tests)
// =====================================================
void test_event_delivery(void) {
    printf("\n=== Group 2: Event Delivery ===\n");
    int ret;

    // Test 5: Register a user cap for testing events
    const char *deps[] = {"serial_output"};
    ret = syscall_cap_register("evt_test_cap", CAP_APPLICATION, deps, 1);
    TEST("5. Register 'evt_test_cap' (dep: serial)", ret >= 0);

    // Test 6: Watch it, activate it, poll for ACTIVATED event
    ret = syscall_cap_watch("evt_test_cap");
    if (ret != 0) {
        printf("  (watch returned %d, skipping delivery tests)\n", ret);
        tests_failed += 3;
        return;
    }

    ret = syscall_cap_activate("evt_test_cap");
    // Activation should succeed (serial is already ON from boot)

    state_cap_event_t events[4];
    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TEST("6. Activate triggers ACTIVATED event", ret > 0 && events[0].type == STATE_CAP_EVENT_ACTIVATED);

    // Test 7: Deactivate → poll for DEACTIVATED event
    ret = syscall_cap_deactivate("evt_test_cap");

    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TEST("7. Deactivate triggers DEACTIVATED event", ret > 0 && events[0].type == STATE_CAP_EVENT_DEACTIVATED);

    // Test 8: Event has correct cap_name
    TEST("8. Event cap_name matches", strcmp(events[0].cap_name, "evt_test_cap") == 0);

    // Cleanup
    syscall_cap_unwatch("evt_test_cap");
    syscall_cap_unregister("evt_test_cap");
}

// =====================================================
// Group 3: Multiple Watchers (2 tests)
// =====================================================
void test_multiple_watchers(void) {
    printf("\n=== Group 3: Multiple Watchers ===\n");
    int ret;

    // Register two caps for testing
    ret = syscall_cap_register("evt_multi_a", CAP_APPLICATION, NULL, 0);
    int cap_a_ok = (ret >= 0);

    const char *deps_b[] = {"serial_output"};
    ret = syscall_cap_register("evt_multi_b", CAP_APPLICATION, deps_b, 1);
    int cap_b_ok = (ret >= 0);

    // Test 9: Watch two different caps from same process
    if (cap_a_ok) syscall_cap_watch("evt_multi_a");
    if (cap_b_ok) syscall_cap_watch("evt_multi_b");

    // Activate both
    if (cap_a_ok) syscall_cap_activate("evt_multi_a");
    if (cap_b_ok) syscall_cap_activate("evt_multi_b");

    // Poll should return events from both
    state_cap_event_t events[8];
    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 8);
    TEST("9. Watch two caps, both deliver events", ret >= 2);

    // Cleanup: deactivate both
    if (cap_b_ok) syscall_cap_deactivate("evt_multi_b");
    if (cap_a_ok) syscall_cap_deactivate("evt_multi_a");

    // Drain any deactivation events
    zero(events, sizeof(events));
    syscall_cap_poll_nonblock(events, 8);

    // Test 10: Unregister a watched cap → no crash
    if (cap_a_ok) {
        syscall_cap_unregister("evt_multi_a");
    }
    TEST("10. Unregister watched cap (no crash)", 1);

    // Cleanup
    if (cap_b_ok) {
        syscall_cap_unwatch("evt_multi_b");
        syscall_cap_unregister("evt_multi_b");
    }
}

// =====================================================
// Group 4: Edge Cases (2 tests)
// =====================================================
void test_edge_cases(void) {
    printf("\n=== Group 4: Edge Cases ===\n");
    int ret;

    // Test 11: Watch nonexistent cap → NOT_FOUND
    ret = syscall_cap_watch("nonexistent_capability_xyz");
    TEST("11. Watch nonexistent cap (-8)", ret == CAP_ERR_NOT_FOUND);

    // Test 12: Poll with no events → returns -99 or 0
    // (no events pending, non-blocking should return -99 or 0)
    state_cap_event_t events[4];
    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TEST("12. Poll with no events (0 or -99)", ret == 0 || ret == -99);
}

// Main
void _start(void) {
    printf("=== CAN Event Propagation Test Suite ===\n");
    printf("Phase 8d: Event watch/poll/delivery\n");

    test_watch_unwatch();
    test_event_delivery();
    test_multiple_watchers();
    test_edge_cases();

    printf("\n=== Results: %d passed, %d failed (Total: %d) ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    if (tests_failed == 0) {
        printf("ALL TESTS PASSED!\n");
    }

    syscall_exit(tests_failed);
}
