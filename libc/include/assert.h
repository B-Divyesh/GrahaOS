// libc/include/assert.h
//
// Minimal freestanding assert.h.  Phase 22 closeout (G1.3): added to satisfy
// vendored Mongoose's chacha-portable.c (referenced from libtls-mg.a build).
// On NDEBUG this collapses to (void)0; otherwise it logs + traps via abort().
#pragma once

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
extern void abort(void);
extern int  printf(const char *fmt, ...);
#define assert(expr) \
    ((expr) ? (void)0 \
            : (printf("assert failed: %s at %s:%d\n", #expr, __FILE__, __LINE__), \
               abort()))
#endif

// C11 static_assert.
#define static_assert _Static_assert
