// user/tests/txn_exit_cleanup.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: child exits while parent has a
// txn open and pending buffered sends.  Parent's txn_abort after child
// exit still returns 0 (no kernel mishap).
//
// Asserts:
//   1. publish + spawn child succeeded
//   2. parent's 2 chan_sends buffered
//   3. child exited cleanly (status 0)
//   4. txn_abort after child exit returns 0
//   5. no AUDIT_TXN_PARTIAL_EXTERNAL (nothing was delivered)

#include "txn_multi_proc_helper.h"

#define CHAN_NAME     "/test/txn-exit-clean"
#define CHAN_NAME_LEN 20
#define BINARY_PATH   "bin/tests/txn_exit_cleanup.tap"

void _start(int argc, char **argv) {
    (void)argv;
    cap_token_u_t wr_req  = { .raw = 0 };
    cap_token_u_t rd_resp = { .raw = 0 };
    int child_mode = (argc >= 2) ||
                     (txn_mp_probe_child(CHAN_NAME, CHAN_NAME_LEN,
                                         &wr_req, &rd_resp) == 0);
    if (child_mode) {
        if (wr_req.raw == 0 &&
            txn_mp_connect(CHAN_NAME, CHAN_NAME_LEN, &wr_req, &rd_resp) != 0) {
            syscall_exit(11);
        }
        syscall_exit(0);
    }

    tap_plan(5);
    int my_pid = syscall_getpid();
    static audit_entry_u_t g_buf[1024];

    cap_token_u_t accept_rd = txn_mp_publish(CHAN_NAME, CHAN_NAME_LEN);
    int child_pid = txn_mp_spawn_child(BINARY_PATH);
    TAP_ASSERT(accept_rd.raw != 0 && child_pid > 0,
               "1. publish + spawn child succeeded");

    cap_token_u_t rd_req  = { .raw = 0 };
    cap_token_u_t wr_resp = { .raw = 0 };
    int ar = txn_mp_accept(accept_rd, &rd_req, &wr_resp);
    if (ar != 0 || wr_resp.raw == 0) {
        TAP_ASSERT(0, "2. parent's 2 chan_sends buffered (accept failed)");
        TAP_ASSERT(0, "3. child exited cleanly (accept failed)");
        TAP_ASSERT(0, "4. txn_abort returned 0 (accept failed)");
        TAP_ASSERT(0, "5. no AUDIT_TXN_PARTIAL_EXTERNAL");
        goto done;
    }

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "exit_cleanup");
    if (h < 0) {
        TAP_ASSERT(0, "2. parent's 2 chan_sends buffered (txn_begin failed)");
        TAP_ASSERT(0, "3. child exited cleanly (txn_begin failed)");
        TAP_ASSERT(0, "4. txn_abort returned 0 (txn_begin failed)");
        TAP_ASSERT(0, "5. no AUDIT_TXN_PARTIAL_EXTERNAL");
        goto done;
    }

    int all_sent = 1;
    for (uint8_t tag = 1; tag <= 2; tag++) {
        if (txn_mp_send_tag(wr_resp, tag) != 0) { all_sent = 0; break; }
    }
    TAP_ASSERT(all_sent, "2. parent's 2 chan_sends during active txn buffered");

    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0, "3. child exited cleanly (status 0)");

    long arc = syscall_txn_abort((uint32_t)h);
    TAP_ASSERT(arc == 0, "4. txn_abort after child exit returned 0");

    long nevt = syscall_audit_query(0, 0, 0, g_buf, 1024);
    int saw_partial = 0;
    for (long i = 0; i < nevt; i++) {
        if (g_buf[i].event_type == 44 && g_buf[i].subject_pid == my_pid) {
            saw_partial = 1; break;
        }
    }
    TAP_ASSERT(!saw_partial,
               "5. no AUDIT_TXN_PARTIAL_EXTERNAL with our pid");

done:
    tap_done();
    syscall_exit(0);
}
