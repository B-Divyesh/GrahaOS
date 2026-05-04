// user/wasmd/src/loader.h
// Phase 26 Stage E.1 — minimal WebAssembly 1.0 module loader/validator.
//
// Per Stage B verdict NO-GO on Wasmtime: wasmd ships as a small C daemon
// that parses module headers, walks the import section, and cross-references
// each import against /etc/gcp.json (the GCP manifest) to reject modules
// declaring imports their caller's cap bundle does not authorise.
//
// In-tree v1 does NOT execute WASM bytecode. Instantiation returns a status
// + cap_kind_wasm_instance handle without running `_start`. A future Phase
// (27+) wires either wasm3 or a custom interpreter under the same loader.h
// API: loader_validate / loader_instantiate. Substrate-level tests (load,
// missing-cap rejection) pass against the validator alone.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define WASM_MAGIC          0x6D736100u   // "\0asm"
#define WASM_VERSION_1      0x00000001u

// Section IDs (https://webassembly.github.io/spec/core/binary/modules.html)
#define WASM_SEC_CUSTOM     0
#define WASM_SEC_TYPE       1
#define WASM_SEC_IMPORT     2
#define WASM_SEC_FUNCTION   3
#define WASM_SEC_TABLE      4
#define WASM_SEC_MEMORY     5
#define WASM_SEC_GLOBAL     6
#define WASM_SEC_EXPORT     7
#define WASM_SEC_START      8
#define WASM_SEC_ELEMENT    9
#define WASM_SEC_CODE       10
#define WASM_SEC_DATA       11

// Import kinds (https://webassembly.github.io/spec/core/binary/modules.html#import-section)
#define WASM_IMPORT_FUNC    0x00
#define WASM_IMPORT_TABLE   0x01
#define WASM_IMPORT_MEMORY  0x02
#define WASM_IMPORT_GLOBAL  0x03

// Loader return codes. Values match user/syscalls.h:WASM_* test enum.
#define WASM_OK                          0
#define WASM_E_BAD_MAGIC                -1
#define WASM_E_BAD_VERSION              -2
#define WASM_E_TRUNCATED                -3
#define WASM_E_BAD_LEB128               -4
#define WASM_E_TOO_MANY_IMPORTS         -5
#define WASM_E_INSTANTIATE_MISSING_CAP  -6  // import names a GCP fn outside cap bundle
#define WASM_E_NOT_IMPLEMENTED          -7  // execution path; substrate ships without

#define WASM_MAX_IMPORTS    64

typedef struct wasm_import {
    char     module_name[32];
    char     field_name[64];
    uint8_t  kind;     // WASM_IMPORT_*
    uint32_t type_idx; // type idx for funcs / refs for others
} wasm_import_t;

typedef struct wasm_module {
    const uint8_t *bytes;       // borrowed; caller owns
    size_t         len;
    uint32_t       version;
    uint32_t       n_imports;
    wasm_import_t  imports[WASM_MAX_IMPORTS];
} wasm_module_t;

// Parse and validate a WASM module. Populates `out` with imports. Returns
// WASM_OK or a negative WASM_E_* code.
int wasm_load(const uint8_t *bytes, size_t len, wasm_module_t *out);

// Cross-reference a parsed module's imports against an allowed list of
// GCP function names (kebab-case as emitted by gcp2wit.py). Returns
// WASM_OK or WASM_E_INSTANTIATE_MISSING_CAP. On error, *out_missing
// (if non-NULL) is filled with the offending import's field_name.
int wasm_validate_imports(const wasm_module_t *m,
                          const char *const *allowed_imports,
                          size_t n_allowed,
                          const char **out_missing);
