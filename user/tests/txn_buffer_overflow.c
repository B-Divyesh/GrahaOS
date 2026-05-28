// user/tests/txn_buffer_overflow.c — Phase 29 Session H (FU25.C).
//
// External-peer multi-process txn test: txn buffer reaches capacity.
// Verifies the overflow is gracefully handled (chan_send returns < 0
// when buffer fills; txn_abort still cleans up).
//
// Asserts:
//   1. publish + spawn child succeeded
//   2. parent's first ~1000 sends during active txn succeeded
//   3. parent eventually gets chan_send rc < 0 (buffer overflow)
//   4. txn_abort returns 0 cleanly after partial fill
//   5. child exited cleanly

#include "txn_multi_proc_helper.h"

#define CHAN_NAME     "/test/txn-buf-over"
#define CHAN_NAME_LEN 18
#define BINARY_PATH   "bin/tests/txn_buffer_overflow.tap"

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

    cap_token_u_t accept_rd = txn_mp_publish(CHAN_NAME, CHAN_NAME_LEN);
    int child_pid = txn_mp_spawn_child(BINARY_PATH);
    TAP_ASSERT(accept_rd.raw != 0 && child_pid > 0,
               "1. publish + spawn child succeeded");

    cap_token_u_t rd_req  = { .raw = 0 };
    cap_token_u_t wr_resp = { .raw = 0 };
    int ar = txn_mp_accept(accept_rd, &rd_req, &wr_resp);
    if (ar != 0 || wr_resp.raw == 0) {
        TAP_ASSERT(0, "2. some sends buffered (accept failed)");
        TAP_ASSERT(0, "3. send fails when buffer full (accept failed)");
        TAP_ASSERT(0, "4. txn_abort returns 0 (accept failed)");
        goto wait_child;
    }

    long h = syscall_txn_begin(TXN_FLAG_SELF_SCOPE | TXN_FLAG_BUFFER_2MB,
                               "buf_overflow");
    if (h < 0) {
        TAP_ASSERT(0, "2. some sends buffered (txn_begin failed)");
        TAP_ASSERT(0, "3. send fails when buffer full (txn_begin failed)");
        TAP_ASSERT(0, "4. txn_abort returns 0 (txn_begin failed)");
        goto wait_child;
    }

    int first_1000_ok = 1;
    int saw_overflow  = 0;
    int sent_count    = 0;
    for (int i = 0; i < 8192; i++) {
        int rc = txn_mp_send_tag(wr_resp, (uint8_t)(i & 0xFF));
        if (rc < 0) {
            saw_overflow = 1;
            break;
        }
        sent_count++;
        if (i < 1000 && rc != 0) first_1000_ok = 0;
    }
    TAP_ASSERT(first_1000_ok && sent_count >= 1000,
               "2. parent's first ~1000 sends during active txn succeeded");
    TAP_ASSERT(saw_overflow,
               "3. parent eventually gets chan_send rc < 0 (buffer overflow)");

    long arc = syscall_txn_abort((uint32_t)h);
    TAP_ASSERT(arc == 0,
               "4. txn_abort returns 0 cleanly after partial fill");

wait_child: {
    int child_status = -1;
    (void)syscall_wait(&child_status);
    TAP_ASSERT(child_status == 0, "5. child exited cleanly");
}

    tap_done();
    syscall_exit(0);
}
