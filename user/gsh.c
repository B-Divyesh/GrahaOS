// user/gsh.c
//
// Phase 28 Session G.3 — gsh: GrahaOS Shell.
//
// A TUI-first interactive shell parallel to gash.  Features:
//   - manifest-driven tab completion (built-ins + every GCP syscall name)
//   - `?<word>` inline help that prints the manifest JSON entry for word
//   - built-ins: cd, pwd, ls, cap-list, snapshot, restore, wasm, ai, txn { } commit|abort, exit
//   - external command spawn (anything in /bin)
//   - script mode via /.gsh-script sentinel for headless gate tests
//
// This is a deliberate full rewrite (NOT a gash extension) per the
// Phase 28 spec.  Where logic mirrors gash exactly (history ring,
// arrow-key readline) we copy the pattern; where it diverges
// (manifest cache, tab completion, ?-help) we author fresh.

#include "syscalls.h"
#include <stdint.h>
#include <stddef.h>

extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern size_t strlen(const char *s);

// =========================================================================
//   Helpers
// =========================================================================

static void print(const char *s) { while (*s) syscall_putc(*s++); }

static void print_int(long v) {
    char buf[24];
    int n = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg) syscall_putc('-');
    while (n--) syscall_putc(buf[n]);
}

static int gsh_atoi(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

static void strncpy_local(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// =========================================================================
//   History (32-deep ring)
// =========================================================================

#define HISTORY_SIZE 32
#define HISTORY_LINE_MAX 256
static char g_history[HISTORY_SIZE][HISTORY_LINE_MAX];
static int  g_history_count = 0;
static int  g_history_write = 0;

static void history_add(const char *line) {
    if (line[0] == '\0') return;
    if (g_history_count > 0) {
        int last = (g_history_write + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(g_history[last], line) == 0) return;
    }
    strncpy_local(g_history[g_history_write], line, HISTORY_LINE_MAX);
    g_history_write = (g_history_write + 1) % HISTORY_SIZE;
    if (g_history_count < HISTORY_SIZE) g_history_count++;
}

static const char *history_get(int idx) {
    if (idx < 0 || idx >= g_history_count) return (const char *)0;
    int pos = (g_history_write - 1 - idx + HISTORY_SIZE * 2) % HISTORY_SIZE;
    return g_history[pos];
}

// =========================================================================
//   Manifest cache + completion-term table
// =========================================================================

#define MANIFEST_BUF_SIZE 16384
#define COMPLETION_MAX    200
#define COMPLETION_NAME_MAX 48

typedef struct {
    char name[COMPLETION_NAME_MAX];
    uint8_t kind;  // 0=builtin, 1=syscall, 2=type, 3=audit
} completion_term_t;

static uint8_t g_manifest_buf[MANIFEST_BUF_SIZE];
static long g_manifest_bytes = 0;
static uint64_t g_manifest_generation = 0;

static completion_term_t g_terms[COMPLETION_MAX];
static int g_term_count = 0;

static const char *g_builtin_names[] = {
    "cd", "pwd", "ls", "cap-list", "snapshot", "restore",
    "wasm", "ai", "txn", "exit", "help", (const char *)0,
};

static void seed_builtins(void) {
    for (int i = 0; g_builtin_names[i]; i++) {
        if (g_term_count >= COMPLETION_MAX) break;
        strncpy_local(g_terms[g_term_count].name, g_builtin_names[i],
                      COMPLETION_NAME_MAX);
        g_terms[g_term_count].kind = 0;
        g_term_count++;
    }
}

// Mini JSON scanner: walks the manifest buffer counting brace depth.
// At depth 2 (inside `"syscalls":{...}`, `"types":{...}`, `"audit_events":{...}`)
// every `"KEY":` becomes a completion term.  No full JSON parser
// needed — manifest is well-formed and we only need keys.
static void parse_manifest_keys(void) {
    long i = 0;
    int depth = 0;
    uint8_t current_kind = 1;  // default to syscall; updated when we
                               // enter "types" / "audit_events".
    while (i < g_manifest_bytes && g_term_count < COMPLETION_MAX) {
        char c = (char)g_manifest_buf[i];
        if (c == '{') depth++;
        else if (c == '}') depth--;
        else if (c == '"' && depth >= 1) {
            // Read the key.
            long start = i + 1;
            long j = start;
            while (j < g_manifest_bytes && g_manifest_buf[j] != '"') j++;
            if (j < g_manifest_bytes && j - start > 0 && j - start < COMPLETION_NAME_MAX) {
                // Skip past closing quote + any whitespace then check
                // for ':' — that confirms this is a KEY, not a string
                // value.
                long k = j + 1;
                while (k < g_manifest_bytes &&
                       (g_manifest_buf[k] == ' ' || g_manifest_buf[k] == '\t' ||
                        g_manifest_buf[k] == '\n')) k++;
                if (k < g_manifest_bytes && g_manifest_buf[k] == ':') {
                    char key[COMPLETION_NAME_MAX];
                    long klen = j - start;
                    for (long m = 0; m < klen; m++) key[m] = (char)g_manifest_buf[start + m];
                    key[klen] = '\0';

                    if (depth == 1) {
                        // Top-level section name: update current_kind.
                        if (strcmp(key, "types") == 0) current_kind = 2;
                        else if (strcmp(key, "syscalls") == 0) current_kind = 1;
                        else if (strcmp(key, "audit_events") == 0) current_kind = 3;
                    } else if (depth == 2) {
                        // Section member key — add as a completion term.
                        // Skip duplicates.
                        int dup = 0;
                        for (int t = 0; t < g_term_count; t++) {
                            if (strcmp(g_terms[t].name, key) == 0) { dup = 1; break; }
                        }
                        if (!dup) {
                            strncpy_local(g_terms[g_term_count].name, key,
                                          COMPLETION_NAME_MAX);
                            g_terms[g_term_count].kind = current_kind;
                            g_term_count++;
                        }
                    }
                }
            }
            i = j;  // skip past the closing quote
        }
        i++;
    }
}

static void load_manifest(void) {
    g_manifest_bytes = syscall_manifest_export(g_manifest_buf,
                                               sizeof(g_manifest_buf),
                                               &g_manifest_generation);
    if (g_manifest_bytes <= 0) {
        print("# gsh: manifest_export failed; tab completion disabled\n");
        return;
    }
    parse_manifest_keys();
}

// =========================================================================
//   Inline help (?<word>): scan manifest for "word": and print value
// =========================================================================

static void show_help(const char *word) {
    if (g_manifest_bytes <= 0) {
        print("? help unavailable (manifest_export failed)\n");
        return;
    }
    // Build the search key: `"word":`.
    char target[COMPLETION_NAME_MAX + 4];
    int ti = 0;
    target[ti++] = '"';
    for (int i = 0; word[i] && ti < (int)sizeof(target) - 4; i++) target[ti++] = word[i];
    target[ti++] = '"';
    target[ti++] = ':';
    target[ti] = '\0';

    // Naïve substring scan.
    long tlen = ti;
    long matchpos = -1;
    for (long p = 0; p + tlen <= g_manifest_bytes; p++) {
        int ok = 1;
        for (long q = 0; q < tlen; q++) {
            if (g_manifest_buf[p + q] != (uint8_t)target[q]) { ok = 0; break; }
        }
        if (ok) { matchpos = p; break; }
    }
    if (matchpos < 0) {
        print("? no manifest entry for ");
        print(word);
        print("\n");
        return;
    }

    // Find the value start (skip whitespace after ':').
    long vstart = matchpos + tlen;
    while (vstart < g_manifest_bytes &&
           (g_manifest_buf[vstart] == ' ' || g_manifest_buf[vstart] == '\t' ||
            g_manifest_buf[vstart] == '\n')) vstart++;

    // Print up to ~600 bytes from vstart, stopping at the matching
    // close-brace / next top-level entry.  Naïve depth tracker.
    int depth = 0;
    long vp = vstart;
    int started = 0;
    long printed = 0;
    while (vp < g_manifest_bytes && printed < 600) {
        char c = (char)g_manifest_buf[vp];
        syscall_putc(c);
        printed++;
        if (c == '{' || c == '[') { depth++; started = 1; }
        else if (c == '}' || c == ']') {
            depth--;
            if (started && depth <= 0) { syscall_putc('\n'); break; }
        } else if (!started && c == ',') {
            break;  // scalar value ended
        }
        vp++;
    }
    syscall_putc('\n');
}

// =========================================================================
//   Tab completion
// =========================================================================

// Find current token's start in buf[0..cursor].
static int token_start(const char *buf, int cursor) {
    int i = cursor;
    while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') i--;
    return i;
}

// On Tab: scan g_terms for prefix matches.  If exactly one, complete.
// If many, print the list to stdout and return without modifying buf
// (user can keep typing).
static int handle_tab(char *buf, int *cursor_ptr) {
    int cursor = *cursor_ptr;
    int tstart = token_start(buf, cursor);
    int tlen = cursor - tstart;
    if (tlen == 0) return 0;

    int matches[COMPLETION_MAX];
    int n_match = 0;
    for (int i = 0; i < g_term_count; i++) {
        int ok = 1;
        for (int j = 0; j < tlen; j++) {
            if (buf[tstart + j] != g_terms[i].name[j]) { ok = 0; break; }
        }
        if (ok && (int)strlen(g_terms[i].name) >= tlen) {
            matches[n_match++] = i;
            if (n_match >= COMPLETION_MAX) break;
        }
    }
    if (n_match == 0) {
        syscall_putc('\a');
        return 0;
    }
    if (n_match == 1) {
        // Append remainder.
        const char *full = g_terms[matches[0]].name;
        int flen = (int)strlen(full);
        while (tlen < flen && cursor < 255) {
            buf[cursor] = full[tlen];
            syscall_putc(full[tlen]);
            cursor++;
            tlen++;
        }
        buf[cursor] = '\0';
        *cursor_ptr = cursor;
        return 1;
    }
    // Multi-match: list to stdout below current line, then redraw prompt.
    print("\n");
    for (int i = 0; i < n_match; i++) {
        print("  ");
        print(g_terms[matches[i]].name);
        print("\n");
    }
    print("gsh$ ");
    // re-echo current buffer
    buf[cursor] = '\0';
    print(buf);
    return 0;
}

// =========================================================================
//   readline — line editor with arrow keys + Tab + ?
// =========================================================================

static void readline(char *buf, int max_len) {
    int i = 0;
    int hist_pos = -1;
    char saved[256];
    saved[0] = '\0';
    while (i < max_len - 1) {
        char c = syscall_getc();
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 0x7F) {
            if (i > 0) { i--; print("\b \b"); }
            continue;
        }
        if (c == '\t') {
            buf[i] = '\0';
            handle_tab(buf, &i);
            continue;
        }
        if (c == '\033') {
            char c2 = syscall_getc();
            if (c2 != '[') continue;
            char c3 = syscall_getc();
            if (c3 == 'A') {
                // up arrow
                if (hist_pos < g_history_count - 1) {
                    if (hist_pos == -1) {
                        buf[i] = '\0';
                        strncpy_local(saved, buf, (int)sizeof(saved));
                    }
                    hist_pos++;
                    const char *h = history_get(hist_pos);
                    if (h) {
                        while (i > 0) { print("\b \b"); i--; }
                        i = 0;
                        while (h[i] && i < max_len - 1) {
                            buf[i] = h[i];
                            syscall_putc(h[i]);
                            i++;
                        }
                    }
                }
            } else if (c3 == 'B') {
                // down arrow
                if (hist_pos > -1) {
                    hist_pos--;
                    while (i > 0) { print("\b \b"); i--; }
                    i = 0;
                    const char *src = (hist_pos == -1) ? saved : history_get(hist_pos);
                    if (src) {
                        while (src[i] && i < max_len - 1) {
                            buf[i] = src[i];
                            syscall_putc(src[i]);
                            i++;
                        }
                    }
                }
            }
            continue;
        }
        buf[i++] = c;
        syscall_putc(c);
    }
    buf[i] = '\0';
    print("\n");
}

// =========================================================================
//   Builtins + dispatcher
// =========================================================================

#define MAX_ARGV 16
#define MAX_TOKEN 96

static int parse_argv(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = (char *)0;
    return argc;
}

static char g_cwd[128] = "/";

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) { strncpy_local(g_cwd, "/", (int)sizeof(g_cwd)); return; }
    const char *path = argv[1];
    int fd = syscall_open(path);
    if (fd < 0) {
        print("cd: cannot access ");
        print(path);
        print("\n");
        return;
    }
    syscall_close(fd);
    strncpy_local(g_cwd, path, (int)sizeof(g_cwd));
}

static void cmd_pwd(void) {
    print(g_cwd);
    print("\n");
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : g_cwd;
    // syscall_readdir signature: int syscall_readdir(const char *path,
    // int index, grahafs_dirent_t *out) — index iterates; -ENOENT
    // ends.  We don't include kernel headers here; use the raw syscall
    // and a stack buffer.
    // The dirent layout (from kernel/fs/grahafs.h) is name[256] + ino:u32
    // + type:u32 — 264 bytes.  But we just emit names so we copy out
    // up to the first NUL.
    uint8_t dirent[264];
    for (int i = 0; i < 256; i++) {
        long rc = syscall_readdir(path, i, (void *)dirent);
        if (rc < 0) break;
        // dirent[0..255] is the name.
        if (dirent[0] == '\0') break;
        print((const char *)dirent);
        print("\n");
    }
}

static void cmd_cap_list(void) {
    // Walk the calling task's cap_handles via SYS_CAP_INSPECT.  Best-
    // effort enumeration: try every well-known cap_token shape.  We
    // don't have a per-process iteration syscall yet, so this just
    // prints metadata for tokens whose object_idx is in [0..63].
    print("# cap-list (probing object_idx 0..63):\n");
    for (uint32_t idx = 0; idx < 64; idx++) {
        cap_token_raw_t tok = ((uint64_t)idx);  // gen=0 flags=0 — best effort
        cap_inspect_result_u_t out;
        long rc = syscall_cap_inspect(tok, &out);
        if (rc < 0) continue;
        print("  idx=");
        print_int((long)idx);
        print(" kind=");
        print_int((long)out.kind);
        print(" rights=0x");
        // hex (64-bit bitmap)
        for (int sh = 60; sh >= 0; sh -= 4) {
            uint8_t nib = (out.rights_bitmap >> sh) & 0xF;
            syscall_putc(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
        }
        print("\n");
    }
}

static void cmd_snapshot(int argc, char **argv) {
    const char *name = (argc >= 2) ? argv[1] : "default";
    long h = syscall_snap_create(0, name);
    print("snapshot: handle=");
    print_int(h);
    print("\n");
}

static void cmd_restore(int argc, char **argv) {
    if (argc < 2) { print("restore: usage: restore <handle>\n"); return; }
    long h = (long)gsh_atoi(argv[1]);
    long rc = syscall_snap_restore((uint32_t)h);
    print("restore: rc=");
    print_int(rc);
    print("\n");
}

static void cmd_wasm(int argc, char **argv) {
    (void)argc; (void)argv;
    // syscall_spawn doesn't pass argv across the kernel ABI, so we
    // can't drive `wasm run <path>` directly from gsh today.  The
    // operator CLI lives in bin/wasm + bin/grahai; both honour
    // /ai_prompt.txt + sentinel-based config.  This builtin documents
    // the path; a follow-up adds a spawn_with_argv ABI.
    print("wasm: launch via gash> exec bin/wasm OR grahai wasm-run <path>.\n");
    print("      (gsh-direct spawn pending syscall_spawn_argv — Phase 29.)\n");
}

static void cmd_ai(int argc, char **argv) {
    if (argc < 2) { print("ai: usage: ai <prompt...>\n"); return; }
    // Concatenate argv[1..argc] with spaces into /ai_prompt.txt.
    char prompt[512];
    int p = 0;
    for (int i = 1; i < argc; i++) {
        for (int j = 0; argv[i][j] && p < (int)sizeof(prompt) - 2; j++) {
            prompt[p++] = argv[i][j];
        }
        if (i < argc - 1 && p < (int)sizeof(prompt) - 2) prompt[p++] = ' ';
    }
    prompt[p++] = '\n';
    prompt[p] = '\0';

    syscall_create("/ai_prompt.txt", 0);
    int fd = syscall_open("/ai_prompt.txt");
    if (fd >= 0) {
        syscall_truncate(fd);
        syscall_write(fd, prompt, p);
        syscall_close(fd);
    }
    long pid = syscall_spawn("bin/grahai");
    if (pid < 0) { print("ai: spawn failed\n"); return; }
    int status = 0;
    syscall_wait(&status);
    print("ai: exit=");
    print_int(status);
    print("\n");
}

// Forward decl — cmd_txn calls process_cmdline recursively.
static void process_cmdline(char *line);

// Minimal `txn { body } commit|abort` — `body` may contain multiple
// commands separated by `;` or newlines.  Fence: must end with `}` then
// `commit` or `abort`.
static void cmd_txn(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "{") != 0) {
        print("txn: usage: txn { cmd; cmd } commit|abort\n");
        return;
    }
    // Reconstruct the body by walking argv until we hit `}`.
    char body[256];
    int bp = 0;
    int i = 2;
    int saw_close = 0;
    const char *verdict = (const char *)0;
    while (i < argc) {
        if (strcmp(argv[i], "}") == 0) {
            saw_close = 1;
            if (i + 1 < argc) verdict = argv[i + 1];
            break;
        }
        for (int j = 0; argv[i][j] && bp < (int)sizeof(body) - 2; j++) body[bp++] = argv[i][j];
        if (i + 1 < argc && bp < (int)sizeof(body) - 2) body[bp++] = ' ';
        i++;
    }
    body[bp] = '\0';
    if (!saw_close || !verdict) {
        print("txn: missing `}` or commit|abort\n");
        return;
    }
    long h = syscall_txn_begin(0, "gsh-txn");
    if (h < 0) { print("txn: begin failed\n"); return; }
    // Execute body commands separated by `;`.
    char *p = body;
    char *start = p;
    while (*p) {
        if (*p == ';') { *p = '\0'; if (*start) process_cmdline(start); start = p + 1; }
        p++;
    }
    if (*start) process_cmdline(start);
    long rc;
    if (strcmp(verdict, "commit") == 0) rc = syscall_txn_commit((uint32_t)h);
    else if (strcmp(verdict, "abort") == 0) rc = syscall_txn_abort((uint32_t)h);
    else { rc = syscall_txn_abort((uint32_t)h); print("txn: unknown verdict, aborted\n"); }
    print("txn: ");
    print(verdict);
    print(" rc=");
    print_int(rc);
    print("\n");
}

// =========================================================================
//   External command spawn
// =========================================================================

static int try_spawn_external(int argc, char **argv) {
    (void)argc;
    char path[64];
    int pi = 0;
    const char *prefix = "bin/";
    while (prefix[pi] && pi < (int)sizeof(path) - 1) { path[pi] = prefix[pi]; pi++; }
    for (int j = 0; argv[0][j] && pi < (int)sizeof(path) - 1; j++) path[pi++] = argv[0][j];
    path[pi] = '\0';
    int fd = syscall_open(path);
    if (fd < 0) return 0;
    syscall_close(fd);
    long pid = syscall_spawn(path);
    if (pid < 0) {
        print("spawn: failed\n");
        return 1;  // we *tried* but the spawn failed
    }
    int status = 0;
    syscall_wait(&status);
    return 1;
}

static void process_cmdline(char *line) {
    // Strip leading whitespace.
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    // Inline help: `?word` (no space).
    if (line[0] == '?') {
        const char *w = line + 1;
        while (*w == ' ') w++;
        if (*w) show_help(w);
        return;
    }

    char *argv[MAX_ARGV];
    int argc = parse_argv(line, argv, MAX_ARGV);
    if (argc == 0) return;

    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc >= 2) ? gsh_atoi(argv[1]) : 0;
        syscall_exit(code);
    }
    if (strcmp(argv[0], "cd") == 0)        { cmd_cd(argc, argv);    return; }
    if (strcmp(argv[0], "pwd") == 0)       { cmd_pwd();             return; }
    if (strcmp(argv[0], "ls") == 0)        { cmd_ls(argc, argv);    return; }
    if (strcmp(argv[0], "cap-list") == 0)  { cmd_cap_list();        return; }
    if (strcmp(argv[0], "snapshot") == 0)  { cmd_snapshot(argc, argv); return; }
    if (strcmp(argv[0], "restore") == 0)   { cmd_restore(argc, argv); return; }
    if (strcmp(argv[0], "wasm") == 0)      { cmd_wasm(argc, argv);  return; }
    if (strcmp(argv[0], "ai") == 0)        { cmd_ai(argc, argv);    return; }
    if (strcmp(argv[0], "txn") == 0)       { cmd_txn(argc, argv);   return; }
    if (strcmp(argv[0], "help") == 0) {
        print("gsh built-ins:\n");
        print("  cd <path>\n  pwd\n  ls [path]\n");
        print("  cap-list\n  snapshot <name>\n  restore <handle>\n");
        print("  wasm run <path>\n  ai <prompt...>\n");
        print("  txn { cmd; cmd } commit|abort\n");
        print("  ?<word>  — manifest help for word\n");
        print("  exit [N]\n");
        return;
    }

    if (try_spawn_external(argc, argv)) return;
    print("gsh: unknown command: ");
    print(argv[0]);
    print("\n");
}

// =========================================================================
//   Script mode for headless gate tests (G.3.i)
// =========================================================================

#define GSH_SCRIPT_SENTINEL "/.gsh-script"
#define GSH_COMPLETE_OUT    "/gsh_complete_out"

// grahafs has no seek/append; multiple syscall_write calls to a freshly-
// opened fd all start at offset 0 → the SECOND completion would overwrite
// the FIRST.  Buffer completions in-memory and flush all at end-of-script.
#define GSH_COMPLETE_BUF_MAX 2048
static char g_complete_buf[GSH_COMPLETE_BUF_MAX];
static int  g_complete_len = 0;

static void script_emit_completion(const char *buf) {
    int len = (int)strlen(buf);
    if (g_complete_len + len + 1 >= GSH_COMPLETE_BUF_MAX) return;
    for (int i = 0; i < len; i++) g_complete_buf[g_complete_len++] = buf[i];
    g_complete_buf[g_complete_len++] = '\n';
}

static void script_flush_completions(void) {
    if (g_complete_len == 0) return;
    syscall_create(GSH_COMPLETE_OUT, 0);
    int fd = syscall_open(GSH_COMPLETE_OUT);
    if (fd < 0) return;
    (void)syscall_truncate(fd);
    syscall_write(fd, g_complete_buf, (size_t)g_complete_len);
    syscall_close(fd);
}

static int try_run_script_sentinel(void) {
    int fd = syscall_open(GSH_SCRIPT_SENTINEL);
    if (fd < 0) return 0;
    char first;
    ssize_t r = syscall_read(fd, &first, 1);
    syscall_close(fd);
    if (r <= 0) return 0;

    fd = syscall_open(GSH_SCRIPT_SENTINEL);
    if (fd < 0) return 0;
    char line[256];
    int li = 0;
    char ch;
    while (syscall_read(fd, &ch, 1) > 0) {
        if (ch == '\n') {
            line[li] = '\0';
            // Find a literal \t.
            int tab_pos = -1;
            for (int k = 0; k < li; k++) if (line[k] == '\t') { tab_pos = k; break; }
            if (tab_pos >= 0) {
                // Trim the line at the tab, run handle_tab on the
                // prefix, then write the resulting buf to the
                // completion output file.
                char prefix[256];
                strncpy_local(prefix, line, tab_pos + 1);
                int cursor = tab_pos;
                handle_tab(prefix, &cursor);
                script_emit_completion(prefix);
            } else {
                process_cmdline(line);
            }
            li = 0;
        } else if (li < (int)sizeof(line) - 1) {
            line[li++] = ch;
        }
    }
    syscall_close(fd);
    // Flush buffered tab-completion results so gsh_completion.tap can
    // read them.  All completions appended in one shot here.
    script_flush_completions();
    // Self-clean the sentinel so subsequent gsh spawns are interactive.
    int clean_fd = syscall_open(GSH_SCRIPT_SENTINEL);
    if (clean_fd >= 0) {
        (void)syscall_truncate(clean_fd);
        syscall_close(clean_fd);
    }
    syscall_exit(0);
    return 1;  // unreachable
}

// =========================================================================
//   Main loop
// =========================================================================

void _start(int argc, char **argv) {
    (void)argc; (void)argv;

    // Manifest cache + builtin seeds — needed for both interactive AND
    // script mode (gsh_completion.tap exercises tab completion against
    // the manifest).
    seed_builtins();
    load_manifest();

    // Script-mode sentinel takes precedence over interactive prompt.
    if (try_run_script_sentinel()) {
        // unreachable — script_run exits.
    }

    print("\n");
    print("gsh — GrahaOS Shell (Phase 28). Type `help` or `?<word>`.\n");
    print("Manifest: ");
    print_int(g_manifest_bytes);
    print(" bytes, ");
    print_int(g_term_count);
    print(" completion terms.\n");

    char line[HISTORY_LINE_MAX];
    for (;;) {
        print("gsh");
        if (g_cwd[0] && g_cwd[1]) { print(":"); print(g_cwd); }
        print("$ ");
        readline(line, sizeof(line));
        if (line[0]) history_add(line);
        process_cmdline(line);
    }
}
