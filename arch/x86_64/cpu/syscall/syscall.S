; arch/x86_64/cpu/syscall/syscall.S  
bits 64
section .text
global syscall_entry
extern syscall_dispatcher

syscall_entry:
    ; Save user RSP in a non-volatile register before it's overwritten.
    mov r12, rsp

    ; Switch to kernel context.
    swapgs
    mov rsp, gs:[4]

    ; --- BUILD THE DEFINITIVE SYSCALL FRAME ---
    
    ; 1. Push the saved user RSP.
    push r12

    ; 2. Push syscall info.
    push rax ; int_no (syscall number)
    push 0   ; err_code (always 0 for syscalls)

    ; 3. Push GPRs in r15..rax order to match the C struct.
    push r15
    push r14
    push r13
    push r12 ; The original value of R12, not the user_rsp we saved
    push r11 ; User RFLAGS (saved by SYSCALL instruction)
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx ; User RIP (saved by SYSCALL instruction)
    push rbx
    push rax ; Syscall number (also available as int_no above)

    ; Call the C dispatcher.
    mov rdi, rsp
    call syscall_dispatcher

    ; --- RESTORE STATE AND RETURN ---
    
    ; 1. Restore GPRs in exact reverse order.
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; 2. Clean up the stack frame.
    add rsp, 16 ; Discard int_no and err_code
    pop rsp     ; Restore user RSP from the saved value

    ; 3. Return to userland.
    swapgs
    sysretq