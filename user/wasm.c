// user/wasm.c — Phase 26 Stage G operator CLI: `wasm run <path>`.
//
// Loads a WebAssembly module from the GrahaFS filesystem, parses its
// header + import section, and reports validation status. Substrate v1
// does NOT execute bytecode (Stage B verdict deferred Wasmtime; a Phase
// 27 follow-up vendors wasm3 or a custom interpreter).
//
// Usage:
//   wasm run <path>                       — load + validate; print outcome.
//   wasm run <path> --cap CSV             — additionally cross-reference
//                                            the module's imports against
//                                            CSV and reject missing-cap.
//   wasm version                          — print wasmd substrate version.
//
// Exit codes:
//   0  success (load + validate OK)
//   1  unknown arg
//   2  load error (bad magic / version / truncated / too many imports)
//   3  missing-cap (one or more imports outside CSV-allowed set)

#include "syscalls.h"
#include "wasmd/src/loader.h"

#include <stdint.h>
#include <stddef.h>

extern int  printf(const char *fmt, ...);

#define WASM_MAX_FILE_BYTES  (1 * 1024 * 1024)   // 1 MiB cap; substrate scope
static uint8_t g_module_buf[WASM_MAX_FILE_BYTES];

static int s_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static long load_file(const char *path, uint8_t *buf, size_t bufsz) {
    int fd = syscall_open(path);
    if (fd < 0) {
        printf("wasm: cannot open '%s': %d\n", path, fd);
        return -1;
    }
    long total = 0;
    while ((size_t)total < bufsz) {
        ssize_t n = syscall_read(fd, buf + total, bufsz - (size_t)total);
        if (n <= 0) break;
        total += (long)n;
    }
    return total;
}

// Tiny CSV splitter: returns count of tokens (>= 0) written into out[].
// Mutates `csv` in place. Tokens are NUL-terminated within the input string.
static int split_csv(char *csv, const char **out, int max) {
    if (!csv || !*csv) return 0;
    int n = 0;
    char *p = csv;
    out[n++] = p;
    while (*p) {
        if (*p == ',') {
            *p = '\0';
            if (n >= max) break;
            out[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        printf("wasm run: missing <path>\n");
        return 1;
    }
    const char *path = argv[0];
    const char *allowed_csv = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (s_strcmp(argv[i], "--cap") == 0) {
            allowed_csv = argv[i + 1];
            i++;
        }
    }

    long n = load_file(path, g_module_buf, sizeof(g_module_buf));
    if (n <= 0) return 2;

    wasm_module_t m;
    int rc = wasm_load(g_module_buf, (size_t)n, &m);
    if (rc != WASM_OK) {
        printf("wasm: load failed rc=%d\n", rc);
        return 2;
    }

    printf("wasm: loaded ok (%ld bytes, version=%u, imports=%u)\n",
           n, (unsigned)m.version, (unsigned)m.n_imports);
    for (uint32_t i = 0; i < m.n_imports; i++) {
        printf("  import %u: %s.%s kind=%u\n",
               (unsigned)i,
               m.imports[i].module_name,
               m.imports[i].field_name,
               (unsigned)m.imports[i].kind);
    }

    if (allowed_csv) {
        // local copy so we can mutate
        static char csv_buf[1024];
        int j = 0;
        while (allowed_csv[j] && j < (int)sizeof(csv_buf) - 1) {
            csv_buf[j] = allowed_csv[j];
            j++;
        }
        csv_buf[j] = '\0';
        const char *toks[64] = {0};
        int n_toks = split_csv(csv_buf, toks, 64);

        const char *missing = NULL;
        rc = wasm_validate_imports(&m, toks, (size_t)n_toks, &missing);
        if (rc != WASM_OK) {
            printf("wasm: WASM_INSTANTIATE_MISSING_CAP missing=%s\n",
                   missing ? missing : "?");
            return 3;
        }
    }

    printf("wasm: validate ok (substrate v1 does not execute; instantiate stubbed)\n");
    return 0;
}

static int cmd_version(void) {
    printf("wasm: substrate v1 (Phase 26 Stage E; wasm3+C verdict)\n");
    printf("      loader: WebAssembly 1.0 header + import section\n");
    printf("      execution: NOT IMPLEMENTED in v1 (Phase 27+ scope)\n");
    return 0;
}

static int cmd_help(void) {
    printf("usage: wasm <verb> [args]\n");
    printf("verbs:\n");
    printf("  run <path> [--cap CSV]   load + validate a .wasm module\n");
    printf("  version                  print substrate version\n");
    printf("  help                     this message\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return cmd_help();
    }
    const char *verb = argv[1];
    if (s_strcmp(verb, "run") == 0)     return cmd_run(argc - 2, argv + 2);
    if (s_strcmp(verb, "version") == 0) return cmd_version();
    if (s_strcmp(verb, "help") == 0)    return cmd_help();
    printf("wasm: unknown verb '%s'\n", verb);
    return 1;
}

void _start(int argc, char **argv) {
    int rc = main(argc, argv);
    syscall_exit(rc);
}
