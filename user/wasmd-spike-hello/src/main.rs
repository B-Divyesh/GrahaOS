// Phase 26 Stage B.1 — Day 1 spike: minimal Rust no_std hello world for GrahaOS.
//
// Goal: prove that stable Rust 1.94 produces a static ELF64 x86_64-unknown-none
// binary that GrahaOS sched_create_user_process can load and run, using inline-asm
// to invoke the GCP syscall ABI (syscall instruction, MSR_LSTAR entry).
//
// Syscall numbers per user/syscalls.h: SYS_PUTC=1001, SYS_EXIT=1008.

#![no_std]
#![no_main]

use core::arch::asm;
use core::panic::PanicInfo;

// GrahaOS GCP syscall ABI: rax = syscall number, rdi/rsi/rdx/r10/r8/r9 = args.
// Returns in rax. Same as Linux x86_64 syscall ABI.

#[inline(always)]
unsafe fn syscall1(n: u64, a0: u64) -> i64 {
    let ret: i64;
    unsafe {
        asm!(
            "syscall",
            in("rax") n,
            in("rdi") a0,
            lateout("rax") ret,
            out("rcx") _,
            out("r11") _,
            options(nostack, preserves_flags),
        );
    }
    ret
}

#[inline(always)]
fn sys_putc(c: u8) {
    unsafe { syscall1(1001, c as u64) };
}

#[inline(always)]
fn sys_exit(code: i32) -> ! {
    unsafe { syscall1(1008, code as u64) };
    loop {
        unsafe { asm!("hlt", options(nomem, nostack)) };
    }
}

fn print(s: &str) {
    for &b in s.as_bytes() {
        sys_putc(b);
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    print("hello from rust spike (phase 26 stage B.1)\n");
    sys_exit(0);
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    print("rust panic\n");
    sys_exit(1);
}
