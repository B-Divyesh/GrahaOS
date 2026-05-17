// user/tests/inject_chan_fail.c
//
// Phase 28 Session G.1 — fault injection harness.  Validates that the
// chan_send sample-gated -EAGAIN hook (kernel/ipc/channel.c) fires
// when g_debug_chan_send_fail_rate is non-zero.  Sample uses rdtsc &
// 0xFF so we expect ~1/256 sends to return -EAGAIN.  We drive 4000
// sends — expected hits ~16.

#include "../libtap.h"
#include "../syscalls.h"

extern int printf(const char *fmt, ...);

static void zero_msg(chan_msg_user_t *m) {
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t *)m)[i] = 0;
}

void _start(void) {
    tap_plan(3);

    syscall_debug_inject_reset_all();

    uint64_t hash_notify = gcp_type_hash("grahaos.notify.v1");

    cap_token_u_t wr = {.raw = 0};
    long rc = syscall_chan_create(hash_notify,
                                  /* CHAN_MODE_NONBLOCKING */ 1,
                                  /* capacity */ 64, &wr);
    if (rc <= 0) {
        printf("# setup chan_create rc=%ld\n", rc);
        tap_not_ok("1. setup", "chan_create failed");
        tap_not_ok("2. inject", "skipped");
        tap_not_ok("3. recovery", "skipped");
        syscall_exit(1);
    }
    cap_token_u_t rd = {.raw = (uint64_t)rc};

    chan_msg_user_t msg, in;
    zero_msg(&msg);
    msg.header.type_hash = hash_notify;
    msg.header.inline_len = 4;

    long bs = syscall_chan_send(wr, &msg, 0);
    TAP_ASSERT(bs == 0, "1. baseline chan_send succeeds with no injection");
    zero_msg(&in);
    syscall_chan_recv(rd, &in, 0);

    // Arm the chan_send sample-gated hook.
    syscall_debug_inject_chan_send_fail_rate(1);

    int n_ok = 0, n_eagain = 0, n_other = 0;
    for (int i = 0; i < 4000; i++) {
        zero_msg(&msg);
        msg.header.type_hash = hash_notify;
        msg.header.inline_len = 4;
        long sr = syscall_chan_send(wr, &msg, 0);
        if (sr == 0) {
            n_ok++;
            zero_msg(&in);
            syscall_chan_recv(rd, &in, 0);
        } else if (sr == -11 /* CAP_V2_EAGAIN */) {
            n_eagain++;
        } else {
            n_other++;
        }
    }
    printf("# chan inject: ok=%d eagain=%d other=%d\n",
           n_ok, n_eagain, n_other);
    TAP_ASSERT(n_eagain >= 1,
               "2. at least one chan_send returns -EAGAIN when hook armed");

    syscall_debug_inject_reset_all();
    zero_msg(&msg);
    msg.header.type_hash = hash_notify;
    msg.header.inline_len = 4;
    long final = syscall_chan_send(wr, &msg, 0);
    if (final != 0) printf("# final chan_send rc=%ld\n", final);
    TAP_ASSERT(final == 0, "3. chan_send works again after reset");

    tap_done();
    syscall_exit(0);
}
