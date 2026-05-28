// user/tests/txn_abort_drops.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: abort drops buffered messages.
// Asserts:
//   1. publish + spawn child succeeded
//   2. parent's 3 chan_sends buffered (rc==0 each)
//   3. parent's txn_abort returns 0
//   4. child observed 0 messages on rd_resp (exit 0)
//   5. AUDIT_TXN_ABORT (43) emitted in this test's window

#include "txn_multi_proc_helper.h"

#define CHAN_NAME     "/test/txn-abort-drops"
#define CHAN_NAME_LEN 21
#define BINARY_PATH   "bin/tests/txn_abort_drops.tap"

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
        // Wait for parent's send + abort to complete (500ms).
        spin_us(500000);
        chan_msg_user_t m;
        long b = txn_mp_recv(rd_resp, &m, 200000000ULL);
        if (b <= 0) syscall_exit(0);
        printf("# child: unexpected recv %ld bytes tag=%u\n",
               b, (unsigned)m.inline_payload[0]);
        syscall_exit(13);
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
        TAP_ASSERT(0, "2. parent's 3 chan_sends buffered (accept failed)");
        TAP_ASSERT(0, "3. txn_abort returns 0 (accept failed)");
        goto wait_child;
    }

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE, "abort_drops");
    if (h < 0) {
        TAP_ASSERT(0, "2. parent's 3 chan_sends buffered (txn_begin failed)");
        TAP_ASSERT(0, "3. txn_abort returns 0 (txn_begin failed)");
        goto wait_child;
    }

    int all_sent_ok = 1;
    for (uint8_t tag = 1; tag <= 3; tag++) {
        if (txn_mp_send_tag(wr_resp, tag) != 0) { all_sent_ok = 0; break; }
    }
    TAP_ASSERT(all_sent_ok, "2. parent's 3 chan_sends during active txn buffered");

    long arc = syscall_txn_abort((uint32_t)h);
    TAP_ASSERT(arc == 0, "3. txn_abort returns 0 (buffered messages dropped)");

wait_child: {
    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0,
               "4. child observed 0 messages (aborted sends not replayed)");
}

    long nevt = syscall_audit_query(0, 0, 0, g_buf, 1024);
    int saw_abort = 0;
    for (long i = 0; i < nevt; i++) {
        if (g_buf[i].event_type == 43 && g_buf[i].subject_pid == my_pid) {
            saw_abort = 1; break;
        }
    }
    TAP_ASSERT(saw_abort, "5. AUDIT_TXN_ABORT emitted with our pid");

    tap_done();
    syscall_exit(0);
}
