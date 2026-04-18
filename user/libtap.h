// user/libtap.h
// Phase 12: TAP 1.4 producer for userspace test programs.
//
// Usage:
//   void _start(void) {
//       tap_plan(3);
//       TAP_ASSERT(1 + 1 == 2, "arithmetic");
//       TAP_ASSERT(strcmp("foo", "foo") == 0, "strcmp");
//       tap_skip("ipv6 not ready", "feature not implemented");
//       tap_done();   // optional — emits '# passed N/M' summary line
//   }
//
// Output goes to stdout (printf -> SYS_PUTC). Every call emits a full
// line terminated with '\n'. No internal buffering — each character is
// flushed to serial as it's written, which mitigates the 16550 FIFO
// truncation risk at shutdown.
//
// The TAP 1.4 grammar produced here is the dialect parse_tap.py
// consumes: `1..N`, `ok N - name`, `not ok N - name`, `ok N # SKIP
// reason`, `Bail out! reason`. See https://testanything.org/ for spec.

#ifndef GRAHAOS_LIBTAP_H
#define GRAHAOS_LIBTAP_H

#include <stdbool.h>
#include <stdint.h>

// Emit "1..N\n". Sets the expected assertion count for the summary.
// Call this once, before any tap_ok/tap_not_ok.
void tap_plan(int n);

// Emit "ok <counter> - <name>\n".
void tap_ok(const char *name);

// Emit "not ok <counter> - <name>\n# <reason>\n".
// `reason` may be NULL.
void tap_not_ok(const char *name, const char *reason);

// Emit "ok <counter> # SKIP <reason>\n". Counts toward "passed" in TAP.
void tap_skip(const char *name, const char *reason);

// Emit "Bail out! <reason>\n" and terminate the process with status 77
// (standard TAP bail-out exit code).
void tap_bail_out(const char *reason) __attribute__((noreturn));

// Optional end-of-test summary. Emits "# passed N/M\n". Safe to call
// zero or one times. Does NOT exit the process — caller decides.
void tap_done(void);

// Accessors for ktest integration (unit 11) — exposed so the harness
// can compute aggregate numbers without re-parsing its own output.
int tap_get_planned(void);
int tap_get_passed(void);
int tap_get_failed(void);

// Convenience macro: evaluate `cond`, dispatch to tap_ok / tap_not_ok.
// The failure reason carries the stringified expression and file/line.
#define TAP__XSTR(x) #x
#define TAP__STR(x)  TAP__XSTR(x)
#define TAP_ASSERT(cond, name) \
    do { \
        if (cond) { \
            tap_ok((name)); \
        } else { \
            tap_not_ok((name), \
                       "assertion failed: " #cond \
                       " (" __FILE__ ":" TAP__STR(__LINE__) ")"); \
        } \
    } while (0)

#endif
