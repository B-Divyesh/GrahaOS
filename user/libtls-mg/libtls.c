// user/libtls-mg/libtls.c — Phase 22 Stage D U18 placeholder.
//
// All public API surface lives in libtls_shim.c. This file exists so the
// archive has at least one object even if the extracted
// `mongoose_tls_core.c` is absent; the linker still pulls libtls-mg.a in
// whole via the weak-linkage contract with libhttp.

extern char __libtls_marker_symbol_placeholder;
char __libtls_marker_symbol_placeholder = 0;
