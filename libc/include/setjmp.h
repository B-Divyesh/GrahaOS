#ifndef _SETJMP_H
#define _SETJMP_H

/*
 * GrahaOS libc setjmp/longjmp — x86-64 SYSV ABI.
 *
 * Authored for FU27.WASM Stage D0: wasm3's interpreter unwinds traps via
 * setjmp/longjmp (mIM_TRAP path); without this shim, vendored wasm3 cannot
 * link in our -nostdlib userspace.
 *
 * jmp_buf layout — 8 x uint64_t = 64 bytes total:
 *   regs[0] RBX
 *   regs[1] RBP
 *   regs[2] R12
 *   regs[3] R13
 *   regs[4] R14
 *   regs[5] R15
 *   regs[6] caller RSP (just before the `call setjmp` instruction)
 *   regs[7] return-RIP (instruction after `call setjmp`)
 *
 * Semantics:
 *   setjmp(env)     — save state; return 0 on the saving call.
 *   longjmp(env, v) — jump back to the setjmp site; setjmp returns v.
 *                     If v == 0, returns 1 (max(v, 1) coercion per C99 7.13.2.1).
 *
 * No signal-mask save (sigjmp_buf): GrahaOS doesn't have signals, so plain
 * setjmp/longjmp covers wasm3's needs and any future userland exception
 * unwind.
 */

typedef unsigned long __jmp_buf_reg_t;
typedef __jmp_buf_reg_t jmp_buf[8];

int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);

#endif /* _SETJMP_H */
