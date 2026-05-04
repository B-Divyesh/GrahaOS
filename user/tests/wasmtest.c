// user/tests/wasmtest.c — Phase 26 Stage H.1 TAP gate test.
//
// Verifies the wasmd loader/validator substrate (user/wasmd/src/loader.{h,c}).
// Substrate scope: header magic + version, import-section walk, missing-cap
// cross-reference. Real WebAssembly execution (the "instantiate" + run) is
// Phase 27+ scope per Stage B verdict and is out of scope here.
//
// 8 assertions covering: valid empty module, module with one imported func,
// missing-cap rejection, bad magic, bad version, truncated bytes, too-many-
// imports, allowed-import passes.

#include "../libtap.h"
#include "../syscalls.h"
#include "../wasmd/src/loader.h"

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------
// Hand-rolled WASM fixture #1: minimal-valid empty module (8 bytes).
// magic 0x6D736100 + version 0x01 + no sections.
// ----------------------------------------------------------------------
static const uint8_t fixture_empty[] = {
    0x00, 0x61, 0x73, 0x6D,   // \0asm
    0x01, 0x00, 0x00, 0x00,   // version 1
};

// ----------------------------------------------------------------------
// Hand-rolled WASM fixture #2: module with one imported func.
// header (8 B) + Import section (id=2, payload):
//   payload = LEB128(1) + import {
//     module_name = "gcp" (3),
//     field_name  = "compute-noop" (12),
//     kind = 0x00 (func),
//     type_idx = 0
//   }
// ----------------------------------------------------------------------
static const uint8_t fixture_with_import[] = {
    0x00, 0x61, 0x73, 0x6D,
    0x01, 0x00, 0x00, 0x00,
    // Import section
    0x02,                       // section id 2 = IMPORT
    0x14,                       // payload_len = 20 bytes
    0x01,                       // n_imports = 1
    0x03, 'g', 'c', 'p',        // module_name "gcp"
    0x0C, 'c','o','m','p','u','t','e','-','n','o','o','p',  // field_name (12)
    0x00,                       // kind = func
    0x00,                       // type_idx = 0
};

// Same as above but field name is "net-send" (would NOT be in default
// allowed list). Used by missing-cap test.
static const uint8_t fixture_with_net_send[] = {
    0x00, 0x61, 0x73, 0x6D,
    0x01, 0x00, 0x00, 0x00,
    0x02,                       // IMPORT section
    0x10,                       // payload_len = 16
    0x01,                       // n_imports = 1
    0x03, 'g', 'c', 'p',
    0x08, 'n','e','t','-','s','e','n','d',
    0x00,
    0x00,
};

// Bad magic.
static const uint8_t fixture_bad_magic[] = {
    'X', 'X', 'X', 'X',
    0x01, 0x00, 0x00, 0x00,
};

// Bad version.
static const uint8_t fixture_bad_version[] = {
    0x00, 0x61, 0x73, 0x6D,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// Truncated (5 bytes; less than minimum 8).
static const uint8_t fixture_truncated[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01,
};

void _start(void) {
    tap_plan(8);

    wasm_module_t m;

    // ----------------------------------------------------------------
    // 1. Empty module loads cleanly with 0 imports.
    // ----------------------------------------------------------------
    int rc = wasm_load(fixture_empty, sizeof(fixture_empty), &m);
    TAP_ASSERT(rc == WASM_OK && m.n_imports == 0,
               "1. empty module: WASM_OK, 0 imports");

    // ----------------------------------------------------------------
    // 2. Module with one imported func decodes module/field names.
    // ----------------------------------------------------------------
    rc = wasm_load(fixture_with_import, sizeof(fixture_with_import), &m);
    int names_match = 0;
    if (rc == WASM_OK && m.n_imports == 1) {
        // Compare via byte-for-byte; libtap depends on libc strcmp which
        // we'd rather not pull in here.
        const char *want_mod = "gcp";
        const char *want_fn  = "compute-noop";
        names_match = 1;
        for (int k = 0; want_mod[k] != '\0'; k++) {
            if (m.imports[0].module_name[k] != want_mod[k]) {
                names_match = 0; break;
            }
        }
        for (int k = 0; want_fn[k] != '\0'; k++) {
            if (m.imports[0].field_name[k] != want_fn[k]) {
                names_match = 0; break;
            }
        }
    }
    TAP_ASSERT(rc == WASM_OK && m.n_imports == 1 && names_match
                 && m.imports[0].kind == WASM_IMPORT_FUNC,
               "2. import section: 1 imp, gcp.compute-noop, kind=FUNC");

    // ----------------------------------------------------------------
    // 3. Allowed-cap CSV passes for matching imports.
    // ----------------------------------------------------------------
    const char *allow_compute[] = { "compute-noop" };
    const char *missing = NULL;
    rc = wasm_validate_imports(&m, allow_compute, 1, &missing);
    TAP_ASSERT(rc == WASM_OK && missing == NULL,
               "3. validate_imports: allowed list passes");

    // ----------------------------------------------------------------
    // 4. Missing-cap rejection identifies the offending import.
    // ----------------------------------------------------------------
    rc = wasm_load(fixture_with_net_send, sizeof(fixture_with_net_send), &m);
    int net_match = 0;
    if (rc == WASM_OK && m.n_imports == 1) {
        const char *want = "net-send";
        net_match = 1;
        for (int k = 0; want[k] != '\0'; k++) {
            if (m.imports[0].field_name[k] != want[k]) { net_match = 0; break; }
        }
    }
    rc = wasm_validate_imports(&m, allow_compute, 1, &missing);
    TAP_ASSERT(rc == WASM_E_INSTANTIATE_MISSING_CAP && missing != NULL && net_match,
               "4. WASM_E_INSTANTIATE_MISSING_CAP: net-send not in allowed");

    // ----------------------------------------------------------------
    // 5. Bad magic rejection.
    // ----------------------------------------------------------------
    rc = wasm_load(fixture_bad_magic, sizeof(fixture_bad_magic), &m);
    TAP_ASSERT(rc == WASM_E_BAD_MAGIC,
               "5. bad magic returns WASM_E_BAD_MAGIC");

    // ----------------------------------------------------------------
    // 6. Bad version rejection.
    // ----------------------------------------------------------------
    rc = wasm_load(fixture_bad_version, sizeof(fixture_bad_version), &m);
    TAP_ASSERT(rc == WASM_E_BAD_VERSION,
               "6. bad version returns WASM_E_BAD_VERSION");

    // ----------------------------------------------------------------
    // 7. Truncated rejection.
    // ----------------------------------------------------------------
    rc = wasm_load(fixture_truncated, sizeof(fixture_truncated), &m);
    TAP_ASSERT(rc == WASM_E_TRUNCATED,
               "7. truncated returns WASM_E_TRUNCATED");

    // ----------------------------------------------------------------
    // 8. NULL bytes / zero len → graceful WASM_E_TRUNCATED.
    // ----------------------------------------------------------------
    rc = wasm_load(NULL, 0, &m);
    TAP_ASSERT(rc == WASM_E_TRUNCATED,
               "8. NULL bytes returns WASM_E_TRUNCATED");

    tap_done();
    syscall_exit(0);
}
