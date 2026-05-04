// user/txnctl.c — Phase 25 Stage H informational CLI.
//
// Transactions are PROCESS-SCOPED: SYS_TXN_BEGIN binds the txn handle to
// the caller's process, and the txn is force-dropped when that process
// exits (via task_exit's txn_task_exit_cleanup). This means a CLI tool
// invoked as `bin/txn-begin` cannot meaningfully expose begin/commit/abort
// as separate shell commands — each invocation is a fresh process and the
// handle dies with it.
//
// The intended consumers are:
//   1. gash's `txn { ... } commit|abort` shell built-in (Stage I).
//      Keeps a single gash process across the body, so the handle lives.
//   2. grahai --txn <plan> (Stage I).
//      Wraps a single plan execution in begin/commit (or abort on error).
//   3. Userspace programs that link the syscall wrappers in
//      user/syscalls.h and call begin/commit/abort within one process.
//
// This binary is therefore a status / explainer tool only. Subcommands:
//   txn-status      — print Phase 25 transaction subsystem status.
//   txn-help (or any other arg) — usage hint.
//
// argv[0]-dispatch matches user/snapshot.c's pattern: gash's built-in
// shim copies the verb to argv[0], so we branch on the basename.

#include "syscalls.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

static int eq_(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (uint8_t)*a == (uint8_t)*b;
}

static const char *basename_(const char *p) {
    const char *last = p;
    for (const char *s = p; *s; s++) if (*s == '/') last = s + 1;
    return last;
}

static int cmd_status(void) {
    printf("Phase 25 transactional speculation\n");
    printf("  syscalls : SYS_TXN_BEGIN=%u SYS_TXN_COMMIT=%u SYS_TXN_ABORT=%u\n",
           (unsigned)SYS_TXN_BEGIN, (unsigned)SYS_TXN_COMMIT,
           (unsigned)SYS_TXN_ABORT);
    printf("  state    : substrate active. Per-process; force-dropped on exit.\n");
    printf("  consumer : use gash 'txn { ... } commit|abort' or grahai --txn.\n");
    printf("  v1 limit : SCOPE_GLOBAL not yet permitted (CAP_KIND_SYSTEM gate).\n");
    return 0;
}

static int cmd_help(const char *argv0) {
    printf("usage: %s status\n", argv0);
    printf("\n");
    printf("Transactions are process-scoped. To run a transactional region,\n");
    printf("use the gash shell syntax:\n");
    printf("  gash> txn { <command_group> } commit\n");
    printf("  gash> txn { <command_group> } abort\n");
    printf("Or wrap a grahai plan:\n");
    printf("  $ grahai --txn 'do something risky'\n");
    return 0;
}

void _start(int argc, char **argv) {
    const char *verb = (argc > 0 && argv && argv[0]) ?
                       basename_(argv[0]) : "txn";
    if (eq_(verb, "txn-status") || eq_(verb, "txnctl-status")) {
        syscall_exit(cmd_status());
    }
    if (argc > 1 && argv[1] && eq_(argv[1], "status")) {
        syscall_exit(cmd_status());
    }
    syscall_exit(cmd_help(verb));
}
