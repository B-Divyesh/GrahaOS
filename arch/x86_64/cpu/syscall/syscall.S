bits 64
section .text

global syscall_entry
extern syscall_dispatcher

syscall_entry:
    ; We have entered from user mode via the `syscall` instruction.
    ; The CPU has saved user RIP -> RCX and user RFLAGS -> R11.
    ; We are on the kernel stack.
    ;
    ; We must preserve all registers that are not preserved by `sysretq`
    ; and that our C function might clobber. For simplicity and safety,
    ; we save all general-purpose registers.

    ; Push registers in an order that matches our new `syscall_frame_t` struct.
    push rax
    push rbx
    ; We don't push RCX and R11 here because they hold the return RIP and RFLAGS.
    ; We will push them later after saving the others.
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15

    ; Now, to complete the struct, we need to save the original user-mode
    ; values of RCX and R11. We swap them with temporary registers to do so.
    mov rax, rcx  ; Move user RIP to RAX temporarily
    push rax      ; Push user RIP (to match the `rcx` field in the struct)
    mov rax, r11  ; Move user RFLAGS to RAX temporarily
    push rax      ; Push user RFLAGS (to match the `r11` field in the struct)

    ; The stack now holds a complete `syscall_frame_t` struct.
    ; Pass a pointer to this struct (in RDI) to our C handler.
    mov rdi, rsp
    call syscall_dispatcher

    ; The C handler has returned. It may have modified the frame on the stack
    ; (e.g., to set the return value in `rax`).
    ; We now restore all registers from the stack frame.

    ; Restore R11 and RCX first, as they are needed by `sysretq`.
    pop r11       ; Pop return RFLAGS into R11
    pop rcx       ; Pop return RIP into RCX

    ; Pop the rest of the registers.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax       ; This now contains the return value from the C handler.

    ; Return to user mode. `sysretq` is the matched pair for `syscall`.
    ; It restores RIP from RCX and RFLAGS from R11.
    sysretq