// kernel/panic.h
// Phase 13: unified kernel panic surface. Replaces
// kernel/sync/spinlock.c:kernel_panic (framebuffer-only) and the
// ad-hoc watchdog_panic / sched "PANIC: No runnable tasks!" sites.
//
// What a panic emits to serial — parse_oops.py depends on this layout:
//
//   ==OOPS== phase=13 build=<sha> cpu=<n> pid=<p> reason="<s>"
//   oops.magic=0xDEAD0005
//   oops.cr2=<hex> oops.cr3=<hex>
//   oops.rip=<hex> oops.rsp=<hex> oops.rflags=<hex>
//   regs.rax=<hex> rbx=<hex> rcx=<hex> rdx=<hex>
//   regs.rsi=<hex> rdi=<hex> rbp=<hex> r8=<hex>
//   regs.r9=<hex>  r10=<hex> r11=<hex> r12=<hex>
//   regs.r13=<hex> r14=<hex> r15=<hex>
//   frame 0: rip=<hex>
//   ...
//   frame N: rip=<hex>
//   ==KLOG BEGIN==
//   [seq=NN  T.TTT] LEVEL SUBSYS msg
//   ...
//   ==KLOG END==
//   ==OOPS END==
//
// After emission, kpanic calls kernel_shutdown(). Under `-no-reboot`
// that is a deterministic QEMU exit (triple-fault fallback).
#pragma once

#include <stdint.h>
#include <stdbool.h>

struct interrupt_frame;

#define OOPS_MAGIC 0xDEAD0005u

// Panic without a captured interrupt_frame. Registers and rip are
// sampled inline from the calling context. Useful for explicit
// kpanic("no runnable tasks") style sites.
void kpanic(const char *reason) __attribute__((noreturn));

// Panic from an exception handler that already has a populated
// interrupt_frame. This path prints the frame verbatim so the
// original faulting rip/rsp survive into the oops.
void kpanic_at(struct interrupt_frame *frame, const char *reason)
    __attribute__((noreturn));
