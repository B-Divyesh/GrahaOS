// user/tests/txn_nested_basic.c — Phase 25 Stage F TAP gate test.
//
// AW-25.4 (Plan-agent recommendation): nested transactions. Verifies that
// nesting via begin/begin/begin works up to TXN_MAX_NESTING and that
// abort/commit on inner txns doesn't tear down outer txns.

#include "../libtap.h"
#include "../syscalls.h"

#include <stdint.h>
#include <stddef.h>

void _start(void) {
    tap_plan(8);

    // 1. Begin outer.
    long h_outer = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "outer");
    TAP_ASSERT(h_outer >= 0, "1. outer begin returns valid handle");

    // 2. Begin inner-1.
    long h_inner1 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "inner1");
    TAP_ASSERT(h_inner1 >= 0, "2. inner1 begin returns valid handle");

    // 3. Begin inner-2.
    long h_inner2 = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "inner2");
    TAP_ASSERT(h_inner2 >= 0, "3. inner2 begin returns valid handle");

    // 4. Distinct handles.
    TAP_ASSERT(h_outer != h_inner1 && h_inner1 != h_inner2 &&
               h_outer != h_inner2,
               "4. all three handles distinct");

    // 5. Commit innermost (inner2). Outer + inner1 should remain active.
    long c2 = syscall_txn_commit((uint32_t)h_inner2);
    TAP_ASSERT(c2 == 0, "5. innermost commit returns 0");

    // 6. Abort inner1. Outer should remain active.
    long a1 = syscall_txn_abort((uint32_t)h_inner1);
    TAP_ASSERT(a1 == 0, "6. inner1 abort returns 0");

    // 7. Commit outer.
    long co = syscall_txn_commit((uint32_t)h_outer);
    TAP_ASSERT(co == 0, "7. outer commit returns 0");

    // 8. After full teardown, fresh begin/commit still works.
    long h_post = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "post_nested");
    long c_post = syscall_txn_commit((uint32_t)h_post);
    TAP_ASSERT(h_post >= 0 && c_post == 0,
               "8. fresh begin/commit works after nested teardown");

    tap_done();
    syscall_exit(0);
}
