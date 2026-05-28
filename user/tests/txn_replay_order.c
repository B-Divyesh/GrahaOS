// user/tests/txn_replay_order.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: replay order.
//
// Parent begins SCOPE_SELF txn, sends 4 tagged messages to an external peer
// (child) via a published channel, commits.  Child receives messages and
// verifies they arrive in send-order.  Asserts:
//   1. publish + spawn succeeded
//   2. parent's 4 chan_sends buffered (rc==0 each)
//   3. parent's txn_commit returns 0
//   4. child read all 4 messages in correct order (exit 0)
//   5. AUDIT_TXN_COMMIT (42) emitted ≥ 1 in this test's window

#include "txn_multi_proc_helper.h"

#define CHAN_NAME    "/test/txn-replay-order"
#define CHAN_NAME_LEN 22
#define BINARY_PATH  "bin/tests/txn_replay_order.tap"

void _start(int argc, char **argv) {
    (void)argv;

    // ---------------- CHILD ----------------
    // Probe-by-connect: if the channel is already published, parent has
    // already run, so we're the child.  This handles the argc==0 race
    // (kernel sets argv-spawn regs.rdi after sched runnable).
    cap_token_u_t wr_req  = { .raw = 0 };
    cap_token_u_t rd_resp = { .raw = 0 };
    int child_mode = (argc >= 2) ||
                     (txn_mp_probe_child(CHAN_NAME, CHAN_NAME_LEN,
                                         &wr_req, &rd_resp) == 0);
    if (child_mode) {
        // If probe succeeded we already have handles; else connect now.
        if (wr_req.raw == 0 &&
            txn_mp_connect(CHAN_NAME, CHAN_NAME_LEN, &wr_req, &rd_resp) != 0) {
            syscall_exit(11);
        }
        chan_msg_user_t m;
        int prev_tag = 0;
        int ok_count = 0;
        int order_ok = 1;
        for (int i = 0; i < 4; i++) {
            long b = txn_mp_recv(rd_resp, &m, 5000000000ULL);
            if (b < 4) break;
            int tag = m.inline_payload[0];
            if (tag <= prev_tag) order_ok = 0;
            prev_tag = tag;
            ok_count++;
        }
        if (ok_count == 4 && order_ok) syscall_exit(0);
        printf("# child: ok_count=%d order_ok=%d prev=%d\n",
               ok_count, order_ok, prev_tag);
        syscall_exit(12);
    }

    // ---------------- PARENT ----------------
    tap_plan(5);
    int my_pid = syscall_getpid();
    // 1024 = kernel max; with smaller buffers the kernel returns OLDEST N
    // events.  Our test's events land near the END of the (growing) audit
    // ring, so we need the kernel's full pagination logic to reach them.
    static audit_entry_u_t g_buf[1024];

    cap_token_u_t accept_rd = txn_mp_publish(CHAN_NAME, CHAN_NAME_LEN);
    if (accept_rd.raw == 0) {
        TAP_ASSERT(0, "1. publish failed");
    }
    int child_pid = txn_mp_spawn_child(BINARY_PATH);
    TAP_ASSERT(accept_rd.raw != 0 && child_pid > 0,
               "1. publish + spawn child succeeded");

    cap_token_u_t rd_req  = { .raw = 0 };
    cap_token_u_t wr_resp = { .raw = 0 };
    int ar = txn_mp_accept(accept_rd, &rd_req, &wr_resp);
    if (ar != 0 || wr_resp.raw == 0) {
        TAP_ASSERT(0, "2. parent's 4 chan_sends buffered (accept failed)");
        TAP_ASSERT(0, "3. txn_commit returns 0 (accept failed)");
        goto wait_child;
    }

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "replay_order");
    if (h < 0) {
        TAP_ASSERT(0, "2. parent's 4 chan_sends buffered (txn_begin failed)");
        TAP_ASSERT(0, "3. txn_commit returns 0 (txn_begin failed)");
        goto wait_child;
    }

    int all_sent_ok = 1;
    for (uint8_t tag = 1; tag <= 4; tag++) {
        if (txn_mp_send_tag(wr_resp, tag) != 0) { all_sent_ok = 0; break; }
    }
    TAP_ASSERT(all_sent_ok, "2. parent's 4 chan_sends during active txn buffered (rc==0 each)");

    long crc = syscall_txn_commit((uint32_t)h);
    TAP_ASSERT(crc == 0, "3. txn_commit returns 0 (replay drained successfully)");

wait_child: {
    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0,
               "4. child read all 4 messages in correct order (exit 0)");
}

    // Scan the audit ring for any AUDIT_TXN_COMMIT with subject_pid == my_pid.
    // since_ns=0 gets oldest-first; max=256 covers all events my pid could have
    // emitted in this short test.  Filtering by pid is more robust than
    // by ns_timestamp window since the audit ring is large and FIFO.
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
