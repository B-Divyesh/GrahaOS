// user/tests/eventtest.c
// Phase 12: TAP 1.4 port of user/eventtest.c (Phase 8d CAN event propagation tests).

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

// Error codes (mirrored)
#define CAP_ERR_NOT_FOUND     -8
#define CAP_ERR_ALREADY_WATCH -16
#define CAP_ERR_NOT_WATCHING  -17

// Helper: zero memory
static void zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

// =====================================================
// Group 1: Watch/Unwatch (4 tests)
// =====================================================
static void test_watch_unwatch(void) {
    printf("\n=== Group 1: Watch/Unwatch ===\n");
    int ret;

    // Test 1: Watch a known kernel capability (serial)
    ret = syscall_cap_watch("serial_output");
    TAP_ASSERT(ret == 0, "1. Watch 'serial' succeeds");

    // Test 2: Watch same cap again → duplicate error
    ret = syscall_cap_watch("serial_output");
    TAP_ASSERT(ret == CAP_ERR_ALREADY_WATCH, "2. Duplicate watch rejected (-16)");

    // Test 3: Unwatch succeeds
    ret = syscall_cap_unwatch("serial_output");
    TAP_ASSERT(ret == 0, "3. Unwatch 'serial' succeeds");

    // Test 4: Unwatch when not watching → error
    ret = syscall_cap_unwatch("serial_output");
    TAP_ASSERT(ret == CAP_ERR_NOT_WATCHING, "4. Unwatch when not watching (-17)");
}

// =====================================================
// Group 2: Event Delivery (4 tests)
// =====================================================
static void test_event_delivery(void) {
    printf("\n=== Group 2: Event Delivery ===\n");
    int ret;

    // Test 5: Register a user cap for testing events
    const char *deps[] = {"serial_output"};
    ret = syscall_cap_register("evt_test_cap", CAP_APPLICATION, deps, 1);
    TAP_ASSERT(ret >= 0, "5. Register 'evt_test_cap' (dep: serial)");

    // Test 6: Watch it, activate it, poll for ACTIVATED event
    ret = syscall_cap_watch("evt_test_cap");
    if (ret != 0) {
        printf("  (watch returned %d, skipping delivery tests)\n", ret);
        // Original behaviour: tests_failed += 3; — emit 3 failures
        tap_not_ok("6. Activate triggers ACTIVATED event", "watch failed");
        tap_not_ok("7. Deactivate triggers DEACTIVATED event", "watch failed");
        tap_not_ok("8. Event cap_name matches", "watch failed");
        return;
    }

    ret = syscall_cap_activate("evt_test_cap");
    // Activation should succeed (serial is already ON from boot)

    state_cap_event_t events[4];
    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TAP_ASSERT(ret > 0 && events[0].type == STATE_CAP_EVENT_ACTIVATED,
               "6. Activate triggers ACTIVATED event");

    // Test 7: Deactivate → poll for DEACTIVATED event
    ret = syscall_cap_deactivate("evt_test_cap");

    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TAP_ASSERT(ret > 0 && events[0].type == STATE_CAP_EVENT_DEACTIVATED,
               "7. Deactivate triggers DEACTIVATED event");

    // Test 8: Event has correct cap_name
    TAP_ASSERT(strcmp(events[0].cap_name, "evt_test_cap") == 0,
               "8. Event cap_name matches");

    // Cleanup
    syscall_cap_unwatch("evt_test_cap");
    syscall_cap_unregister("evt_test_cap");
}

// =====================================================
// Group 3: Multiple Watchers (2 tests)
// =====================================================
static void test_multiple_watchers(void) {
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
    TAP_ASSERT(ret >= 2, "9. Watch two caps, both deliver events");

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
    TAP_ASSERT(1, "10. Unregister watched cap (no crash)");

    // Cleanup
    if (cap_b_ok) {
        syscall_cap_unwatch("evt_multi_b");
        syscall_cap_unregister("evt_multi_b");
    }
}

// =====================================================
// Group 4: Edge Cases (2 tests)
// =====================================================
static void test_edge_cases(void) {
    printf("\n=== Group 4: Edge Cases ===\n");
    int ret;

    // Test 11: Watch nonexistent cap → NOT_FOUND
    ret = syscall_cap_watch("nonexistent_capability_xyz");
    TAP_ASSERT(ret == CAP_ERR_NOT_FOUND, "11. Watch nonexistent cap (-8)");

    // Test 12: Poll with no events → returns -99 or 0
    state_cap_event_t events[4];
    zero(events, sizeof(events));
    ret = syscall_cap_poll_nonblock(events, 4);
    TAP_ASSERT(ret == 0 || ret == -99, "12. Poll with no events (0 or -99)");
}

void _start(void) {
    printf("=== CAN Event Propagation Test Suite ===\n");
    printf("Phase 8d: Event watch/poll/delivery\n");

    tap_plan(12);

    test_watch_unwatch();
    test_event_delivery();
    test_multiple_watchers();
    test_edge_cases();

    tap_done();
    exit(0);
}
