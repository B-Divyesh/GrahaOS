// user/tests/gsh_chrome.tap.c
//
// Phase 29 Session F — gsh chrome rendering gate.  Five asserts:
//   1. spawn bin/gsh + wait returns clean exit (script-mode triggers
//      the chrome render in the same code path as interactive boot).
//   2. cell (0, 0) contains the double-line top-left corner glyph
//      (U+2554) — proves gsh_draw_chrome ran through libtui.
//   3. cell (1, 2) contains 'g' (start of the "gsh @ Phase 29" title).
//   4. cell (23, 2) contains 'c' (start of "cwd:" in the status footer).
//   5. at least one cell in the sidebar window (rows 3..21, cols 60..78)
//      is non-blank — proves gsh_refresh_cap_sidebar emitted entries.
//
// gsh is launched in script mode (the test stages /.gsh-script with a
// no-op line); the script-mode branch in gsh.c now ALSO draws chrome
// before consuming the script.  This keeps the test headless — no
// readline interaction needed — while still exercising the same TUI
// path interactive boot uses.

#include "../libtap.h"
#include "../syscalls.h"
#include <stdint.h>
#include <string.h>

extern int printf(const char *fmt, ...);

#define SENTINEL_PATH "/.gsh-script"

static int write_file(const char *path, const char *content, int len) {
    for (int attempt = 0; attempt < 3; attempt++) {
        syscall_create(path, 0);
        int fd = syscall_open(path);
        if (fd < 0) continue;
        (void)syscall_truncate(fd);
        ssize_t n = syscall_write(fd, content, (size_t)len);
        syscall_close(fd);
        if (n == len) return 0;
    }
    return -1;
}

void _start(void) {
    tap_plan(5);

    // Drain any stale sentinel state.
    {
        int fd = syscall_open(SENTINEL_PATH);
        if (fd >= 0) { (void)syscall_truncate(fd); syscall_close(fd); }
    }

    // One-line no-op script.  gsh runs gsh_draw_chrome + sidebar
    // BEFORE try_run_script_sentinel, so a trivial script suffices.
    const char script[] = "pwd\n";
    int wr = write_file(SENTINEL_PATH, script, (int)sizeof(script) - 1);
    if (wr != 0) printf("# sentinel write failed\n");

    int pid = syscall_spawn("bin/gsh");
    int status = -1;
    if (pid > 0) syscall_wait(&status);
    if (pid <= 0 || status != 0) {
        printf("# spawn pid=%d status=%d\n", pid, status);
    }
    TAP_ASSERT(pid > 0 && status == 0,
               "1. spawn bin/gsh and wait — clean exit");

    // Assert 2 — top-left double-line corner at (0, 0).
    long cell00 = syscall_debug_console_read_cell(0, 0, 0);
    if (cell00 != 0x2554) {
        printf("# cell(0,0) codepoint=0x%lx, expected 0x2554\n", cell00);
    }
    TAP_ASSERT(cell00 == 0x2554,
               "2. cell (0,0) is U+2554 (double-line top-left)");

    // Assert 3 — title bar starts with 'g' at (1, 2).
    long cell_title = syscall_debug_console_read_cell(0, 1, 2);
    if (cell_title != 'g') {
        printf("# cell(1,2) codepoint=0x%lx, expected 'g' (0x67)\n", cell_title);
    }
    TAP_ASSERT(cell_title == 'g',
               "3. cell (1,2) starts the title 'gsh @ Phase 29'");

    // Assert 4 — footer starts with 'c' at (23, 2).
    long cell_footer = syscall_debug_console_read_cell(0, 23, 2);
    if (cell_footer != 'c') {
        printf("# cell(23,2) codepoint=0x%lx, expected 'c' (0x63)\n", cell_footer);
    }
    TAP_ASSERT(cell_footer == 'c',
               "4. cell (23,2) starts 'cwd:' in the status footer");

    // Assert 5 — sidebar window has at least one non-blank cell.
    int sidebar_nonblank = 0;
    for (uint32_t r = 3; r <= 21 && !sidebar_nonblank; r++) {
        for (uint32_t c = 60; c < 78 && !sidebar_nonblank; c++) {
            long cp = syscall_debug_console_read_cell(0, r, c);
            if (cp > 0 && cp != ' ' && cp != 0) {
                sidebar_nonblank = 1;
            }
        }
    }
    if (!sidebar_nonblank) {
        printf("# sidebar (rows 3..21 cols 60..78) is entirely blank\n");
    }
    TAP_ASSERT(sidebar_nonblank,
               "5. cap sidebar window has at least one rendered cell");

    tap_done();
    syscall_exit(0);
}
