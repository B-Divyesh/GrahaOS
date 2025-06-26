; arch/x86_64/cpu/syscall/syscall.S - FIXED VERSION
bits 64
section .text
global syscall_entry
extern syscall_dispatcher

; Debug variables
extern syscall_entry_reached
extern syscall_about_to_return
extern syscall_stack_switched

section .text
syscall_entry:
    mov qword [syscall_entry_reached], 1
    
    ; Save user RSP
    mov r10, rsp
    
    ; Switch to kernel context
    swapgs
    mov rsp, [gs:4]
    
    test rsp, rsp
    jz panic
    
    mov qword [syscall_stack_switched], 1
    
    ; Build syscall_frame
    push r10            ; user_rsp
    push 0              ; err_code
    push rax            ; int_no
    
    ; Push all GPRs
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    
    ; Call dispatcher
    mov rdi, rsp
    call syscall_dispatcher
    
    mov qword [syscall_about_to_return], 1
    
    ; Restore registers
    pop rax
    pop rbx
    pop rcx             ; Return RIP
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11             ; RFLAGS
    pop r12
    pop r13
    pop r14
    pop r15
    
    add rsp, 16
    pop rsp
    
    ; Return to user - NO CANONICAL CHECK FOR NOW
    swapgs
    db 0x48, 0x0f, 0x07  ; sysretq opcode

panic:
    cli
.hang:
    hlt
    jmp .hang