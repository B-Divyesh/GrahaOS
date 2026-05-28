// user/tests/txn_concurrent_abort.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: nested SELF-SCOPE txns where the
// inner aborts but the outer commits.  Outer's buffer retains the messages
// (Plan-agent Q4 "buffer at every external layer" rule) so outer commit
// replays them.
//
// Asserts:
//   1. publish + spawn child succeeded
//   2. nested outer+inner sends buffered
//   3. inner txn_abort returns 0
//   4. outer txn_commit returns 0
//   5. child received both messages (exit 0)

#include "txn_multi_proc_helper.h"

#define CHAN_NAME     "/test/txn-conc-abort"
#define CHAN_NAME_LEN 20
#define BINARY_PATH   "bin/tests/txn_concurrent_abort.tap"

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
        chan_msg_user_t m;
        long b1 = txn_mp_recv(rd_resp, &m, 5000000000ULL);
        long b2 = txn_mp_recv(rd_resp, &m, 5000000000ULL);
        if (b1 >= 4 && b2 >= 4) syscall_exit(0);
        printf("# child: b1=%ld b2=%ld\n", b1, b2);
        syscall_exit(15);
    }

    tap_plan(5);

    cap_token_u_t accept_rd = txn_mp_publish(CHAN_NAME, CHAN_NAME_LEN);
    int child_pid = txn_mp_spawn_child(BINARY_PATH);
    TAP_ASSERT(accept_rd.raw != 0 && child_pid > 0,
               "1. publish + spawn child succeeded");

    cap_token_u_t rd_req  = { .raw = 0 };
    cap_token_u_t wr_resp = { .raw = 0 };
    int ar = txn_mp_accept(accept_rd, &rd_req, &wr_resp);
    if (ar != 0 || wr_resp.raw == 0) {
        TAP_ASSERT(0, "2. outer + inner sends buffered (accept failed)");
        TAP_ASSERT(0, "3. inner txn_abort returns 0");
        TAP_ASSERT(0, "4. outer txn_commit returns 0");
        goto wait_child;
    }

    long h_outer = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "conc_outer");
    long h_inner = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "conc_inner");
    if (h_outer < 0 || h_inner < 0) {
        TAP_ASSERT(0, "2. nested begins failed");
        TAP_ASSERT(0, "3. inner abort skipped");
        TAP_ASSERT(0, "4. outer commit skipped");
        goto wait_child;
    }

    int sent_ok = 1;
    if (txn_mp_send_tag(wr_resp, 1) != 0) sent_ok = 0;
    if (txn_mp_send_tag(wr_resp, 2) != 0) sent_ok = 0;
    TAP_ASSERT(sent_ok, "2. outer + inner sends buffered (both txns active)");

    long inner_abort = syscall_txn_abort((uint32_t)h_inner);
    TAP_ASSERT(inner_abort == 0, "3. inner txn_abort returns 0");

    long outer_commit = syscall_txn_commit((uint32_t)h_outer);
    TAP_ASSERT(outer_commit == 0, "4. outer txn_commit returns 0");

wait_child: {
    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0,
               "5. child received both messages from outer commit");
}

    tap_done();
    syscall_exit(0);
}
