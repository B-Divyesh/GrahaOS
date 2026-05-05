/*
 * user/wasmd/src/wasm3_smoke.c — FU27.WASM Stage D0 smoke test.
 *
 * Goal: prove vendor/wasm3 + libc shims linked together can parse a
 * non-trivial WebAssembly module without crashing or printing errors.
 * Run via `make smoke` (target added in user/Makefile after this file).
 *
 * Uses the same hand-rolled fixture from Phase 26's wasmtest.c
 * (`fixture_with_import` — 1 func import "gcp.compute-noop") so we
 * don't introduce a separate ground-truth.
 *
 * Expected output:
 *   wasm3 smoke: env ok
 *   wasm3 smoke: runtime ok
 *   wasm3 smoke: parse ok (1 imports)
 *   wasm3 smoke: load ok
 *   wasm3 smoke: PASS
 *
 * Exit code: 0 on success, 1 on failure (any wasm3 step returns non-NULL
 * M3Result error string).
 */

#include "wasm3.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* fixture_with_import: WebAssembly 1.0 module declaring one func import
 * of type () -> () named "gcp.compute-noop". Hand-rolled bytes from
 * user/tests/wasmtest.c (Phase 26 wasm loader gate). */
static const uint8_t k_fixture[] = {
    /* Magic + version */
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    /* Type section: 1 type, () -> () */
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    /* Import section: 1 import — module="gcp", field="compute-noop", kind=func, type_idx=0 */
    0x02, 0x16, 0x01,
    0x03, 'g', 'c', 'p',
    0x0C, 'c', 'o', 'm', 'p', 'u', 't', 'e', '-', 'n', 'o', 'o', 'p',
    0x00, /* kind = func */
    0x00, /* type_idx = 0 */
};

int main(void) {
    /* Step 1: environment */
    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        printf("wasm3 smoke: FAIL (m3_NewEnvironment)\n");
        return 1;
    }
    printf("wasm3 smoke: env ok\n");

    /* Step 2: runtime (8 KiB stack) */
    IM3Runtime rt = m3_NewRuntime(env, 8 * 1024, NULL);
    if (!rt) {
        printf("wasm3 smoke: FAIL (m3_NewRuntime)\n");
        m3_FreeEnvironment(env);
        return 1;
    }
    printf("wasm3 smoke: runtime ok\n");

    /* Step 3: parse module */
    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, k_fixture, sizeof(k_fixture));
    if (r) {
        printf("wasm3 smoke: FAIL (m3_ParseModule: %s)\n", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return 1;
    }
    printf("wasm3 smoke: parse ok (mod=%p)\n", (void *)mod);

    /* Step 4: load module into runtime (without linking imports — wasm3
       allows module load to succeed even if imports remain unsatisfied;
       call would fail but we don't call). */
    r = m3_LoadModule(rt, mod);
    if (r) {
        printf("wasm3 smoke: FAIL (m3_LoadModule: %s)\n", r);
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
        return 1;
    }
    printf("wasm3 smoke: load ok\n");

    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);

    printf("wasm3 smoke: PASS\n");
    return 0;
}
