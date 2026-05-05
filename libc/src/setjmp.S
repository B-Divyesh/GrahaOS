/*
 * GrahaOS libc setjmp/longjmp — x86-64 SYSV ABI implementation.
 *
 * Layout (jmp_buf is unsigned long[8]):
 *   [0]  RBX
 *   [8]  RBP
 *   [16] R12
 *   [24] R13
 *   [32] R14
 *   [40] R15
 *   [48] caller RSP (RSP+8 at setjmp entry; i.e., the RSP value the caller
 *        had immediately BEFORE issuing `call setjmp` — already 16-byte
 *        aligned per SYSV ABI)
 *   [56] return-RIP (the value at [rsp] when setjmp is entered)
 *
 * No external references → naturally position-independent. Safe to link
 * into both PIE user binaries (-fpie) and the non-PIE libc.a archive.
 */

    .intel_syntax noprefix
    .text

    /* int setjmp(jmp_buf env) */
    .globl setjmp
    .type  setjmp, @function
setjmp:
    /* rdi = env */
    mov     [rdi +  0], rbx
    mov     [rdi +  8], rbp
    mov     [rdi + 16], r12
    mov     [rdi + 24], r13
    mov     [rdi + 32], r14
    mov     [rdi + 40], r15

    /* Caller's RSP before the call: rsp + 8 (popping the pushed return-RIP). */
    lea     rax, [rsp + 8]
    mov     [rdi + 48], rax

    /* Return-RIP sits at [rsp]. */
    mov     rax, [rsp]
    mov     [rdi + 56], rax

    /* return 0 */
    xor     eax, eax
    ret
    .size setjmp, .-setjmp

    /* __attribute__((noreturn)) void longjmp(jmp_buf env, int val) */
    .globl longjmp
    .type  longjmp, @function
longjmp:
    /* rdi = env, esi = val */

    /* Restore callee-saved registers. */
    mov     rbx, [rdi +  0]
    mov     rbp, [rdi +  8]
    mov     r12, [rdi + 16]
    mov     r13, [rdi + 24]
    mov     r14, [rdi + 32]
    mov     r15, [rdi + 40]

    /* Compute return value: max(val, 1) — C99 7.13.2.1 says longjmp(env, 0)
       must cause setjmp to return 1, not 0 (else the caller can't tell
       a longjmp return apart from the original save). */
    mov     eax, esi
    test    eax, eax
    jnz     1f
    mov     eax, 1
1:

    /* Reload return-RIP into rcx BEFORE clobbering RSP — the env pointer
       (rdi) survives, but we'd rather avoid a memory read after switching
       stacks. */
    mov     rcx, [rdi + 56]

    /* Restore the caller's pre-call RSP. */
    mov     rsp, [rdi + 48]

    /* Push return-RIP and ret — net effect: RSP unchanged from saved
       caller-pre-call value; RIP transfers to return-RIP; rax holds the
       setjmp return value. */
    push    rcx
    ret
    .size longjmp, .-longjmp

    /* No executable stack — match the rest of GrahaOS's asm files. */
    .section .note.GNU-stack,"",@progbits
