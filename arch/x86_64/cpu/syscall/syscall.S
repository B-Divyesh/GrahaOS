bits 64
section .text

global syscall_entry
extern syscall_dispatcher

syscall_entry:
    ; We are in kernel mode, but we need to save the user-space state.
    ; The `syscall` instruction saves RIP -> RCX and RFLAGS -> R11.
    ; It does NOT save RSP. We must do that manually.
    ; We will use the kernel stack of the current task. For now, this is fine.

    ; Save registers to build the interrupt_frame on the stack
    push r15
    push r14
    push r13
    push r12
    push r11  ; User RFLAGS
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx  ; User RIP
    push rbx
    ; RAX is not pushed yet, it holds the syscall number.

    ; Now, create the rest of the interrupt_frame structure.
    ; We need to push a dummy error code and the interrupt number (which is not applicable for syscalls).
    ; Then we push the registers that the CPU would have pushed.
    mov rax, [rsp + 14*8] ; Get user RAX (syscall number) from the stack

    ; Create the full interrupt_frame on the stack
    push 0 ; dummy ss
    push 0 ; dummy rsp
    push r11 ; RFLAGS
    push 0x08 ; CS (Kernel Code Segment)
    push rcx ; RIP
    push 0 ; dummy error code
    push rax ; syscall number as "int_no"

    ; Now we have a structure on the stack that looks like interrupt_frame
    ; but the top half is our saved registers. Let's fix that.
    ; The stack pointer (RSP) now points to our interrupt_frame structure.
    ; We pass this as the first argument (in RDI) to our C handler.
    mov rdi, rsp
    call syscall_dispatcher

    ; The return value from C is in RAX. We will leave it there.

    ; Restore all registers from the stack frame.
    ; We skip the "CPU-pushed" part of the frame.
    add rsp, 7*8 ; Skip int_no, err_code, rip, cs, rflags, rsp, ss

    pop rbx
    pop rcx ; This is the user RIP, we need it for sysretq
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11 ; This is the user RFLAGS, we need it for sysretq
    pop r12
    pop r13
    pop r14
    pop r15

    ; Return to the caller. `sysretq` restores RIP from RCX and RFLAGS from R11.
    sysretq
