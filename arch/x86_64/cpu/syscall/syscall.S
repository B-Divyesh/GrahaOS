bits 64
section .text

global syscall_entry
extern syscall_dispatcher

syscall_entry:
    ; This is the entry point for the `syscall` instruction.
    ; The CPU saves RIP into RCX and RFLAGS into R11.
    ; We must save all other registers to create a valid interrupt_frame.

    ; We are already on the kernel stack (for now).

    ; Save general purpose registers in the order defined by interrupt_frame.
    push r15
    push r14
    push r13
    push r12
    push r11        ; User RFLAGS (saved by CPU)
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx        ; User RIP (saved by CPU)
    push rbx
    push rax        ; Contains syscall number

    ; Now, construct the part of the frame that the CPU would push for interrupts.
    ; We need to simulate what hardware interrupt would have pushed:
    push 0x10       ; SS (Kernel Data Segment)
    push rsp        ; RSP (current stack pointer)
    add qword [rsp], 17*8  ; Adjust RSP to point to user stack before we pushed registers
    push r11        ; RFLAGS (from the CPU)
    push 0x08       ; CS (Kernel Code Segment)
    push rcx        ; RIP (from the CPU)

    ; Syscalls don't have an error code. Push a dummy one.
    push 0          ; Error Code

    ; The syscall number is in the original RAX. Push it as the int_no.
    mov rax, [rsp + 21*8]   ; Get original RAX from the stack
    push rax        ; Interrupt Number (syscall number)

    ; Now the stack is a perfect `interrupt_frame`.
    ; Pass a pointer to it (current RSP) to the C dispatcher.
    mov rdi, rsp
    call syscall_dispatcher

    ; The return value is in RAX. We need to place it in the saved RAX
    ; on the stack so it gets restored correctly.
    mov [rsp + 15*8], rax   ; Update frame->rax with return value

    ; Clean up the interrupt frame header
    add rsp, 7*8    ; Skip int_no, err_code, rip, cs, rflags, rsp, ss

    ; Restore the registers in reverse order
    pop rax         ; Return value is now in RAX
    pop rbx
    pop rcx         ; User RIP for sysretq
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11         ; User RFLAGS for sysretq
    pop r12
    pop r13
    pop r14
    pop r15

    ; Return to the caller using sysretq
    ; RCX contains the return RIP, R11 contains the return RFLAGS
    sysretq
