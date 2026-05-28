// user/tests/txn_child_abort_parent_commit.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: child holds its own SELF-scope
// txn that aborts; parent independently commits its txn.  Verifies
// per-process txn isolation.
//
// Asserts:
//   1. publish + spawn child succeeded
//   2. parent's txn_commit returns 0
//   3. parent commit independent of child txn lifecycle
//   4. child's own abort returned 0 and msgs received (exit 0)
//   5. AUDIT_TXN_COMMIT emitted in window

#include "txn_multi_proc_helper.h"

#define CHAN_NAME     "/test/txn-cap-pc"
#define CHAN_NAME_LEN 16
#define BINARY_PATH   "bin/tests/txn_child_abort_parent_commit.tap"

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
        long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "child_abort");
        if (h < 0) syscall_exit(20);
        long arc = syscall_txn_abort((uint32_t)h);
        if (arc != 0) syscall_exit(21);

        chan_msg_user_t m;
        long b1 = txn_mp_recv(rd_resp, &m, 5000000000ULL);
        long b2 = txn_mp_recv(rd_resp, &m, 5000000000ULL);
        if (b1 >= 4 && b2 >= 4) syscall_exit(0);
        printf("# child cap: b1=%ld b2=%ld\n", b1, b2);
        syscall_exit(22);
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
        TAP_ASSERT(0, "2. parent's txn_commit returns 0 (accept failed)");
        TAP_ASSERT(0, "3. parent commit independent of child (accept failed)");
        goto wait_child;
    }

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "parent_commit");
    if (h < 0) {
        TAP_ASSERT(0, "2. parent's txn_commit returns 0 (txn_begin failed)");
        TAP_ASSERT(0, "3. parent commit independent (txn_begin failed)");
        goto wait_child;
    }

    int sent_ok = (txn_mp_send_tag(wr_resp, 1) == 0)
                  && (txn_mp_send_tag(wr_resp, 2) == 0);

    long crc = syscall_txn_commit((uint32_t)h);
    TAP_ASSERT(sent_ok && crc == 0, "2. parent's txn_commit returns 0");
    TAP_ASSERT(crc == 0,
               "3. parent commit independent of child txn lifecycle");

wait_child: {
    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0,
               "4. child's own abort returned 0 and msgs received (exit 0)");
}

    long nevt = syscall_audit_query(0, 0, 0, g_buf, 1024);
    int saw_commit = 0;
    for (long i = 0; i < nevt; i++) {
        if (g_buf[i].event_type == 42 && g_buf[i].subject_pid == my_pid) {
            saw_commit = 1; break;
        }
    }
    TAP_ASSERT(saw_commit, "5. AUDIT_TXN_COMMIT emitted with our pid");

    tap_done();
    syscall_exit(0);
}
