; arch/x86_64/cpu/syscall/syscall.S  
bits 64
section .text
global syscall_entry
extern syscall_dispatcher

; Debug variables
extern syscall_entry_reached
extern syscall_about_to_return  
extern syscall_frame_created
extern syscall_pre_dispatch
extern syscall_stack_switched

section .text

syscall_entry:
    mov qword [syscall_entry_reached], 1
    
    ; === PART 1: KERNEL CONTEXT SWITCH ===
    ; SWAPGS exchanges the user GS.base with the kernel GS.base (from MSR).
    ; The GS segment now points to our kernel's TSS structure.
    swapgs
    ; Load the kernel stack pointer for the current task from the TSS.
    ; The rsp0 field is at offset 4 in the tss struct.
    ; We are now running on a safe, dedicated kernel stack.
    mov rsp, gs:[4]
    
    ; Debug: Mark that we successfully switched to kernel stack
    mov qword [syscall_stack_switched], 1
    
    ; === PART 2: BUILD THE SYSCALL FRAME ===
    ; CRITICAL FIX: Push syscall info first to match C struct layout
    push rax            ; int_no (syscall number)
    push 0              ; err_code (dummy) removed now added debug to the end
    
    ; --- CRITICAL FIX: CORRECTED PUSH ORDER ---
    ; Push GPRs to match the C struct layout (struct expects r15 at lowest address)
    ; This creates memory layout: [rax][rbx]...[r15] which matches struct field order
    ;removed the duplicate rax here
    push rbx
    push rcx            ; User RIP (from syscall instruction)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11            ; User RFLAGS (from syscall instruction)
    push r12
    push r13
    push r14
    push r15
    
    mov qword [syscall_frame_created], 1    


    ; === PART 3: CALL C HANDLER ===
    ; The stack is already 16-byte aligned because the kernel stack top is page-aligned.
    ; Pass a pointer to the syscall_frame to C.
    mov qword [syscall_pre_dispatch], 1
    mov rdi, rsp        ; Pointer to syscall_frame - now correctly aligned!
    call syscall_dispatcher
    
    ; === PART 4: RESTORE AND RETURN ===
    mov qword [syscall_about_to_return], 1
    
    ; CRITICAL: Pop GPRs in exact reverse order of push sequence
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Clean up frame header
    add rsp, 16         ; Remove int_no and err_code
    
    ; === PART 5: FINAL SWAP AND RETURN ===
    ; Switch back to user GS base
    swapgs
    
    ; RAX has return value, RCX has user RIP, R11 has user RFLAGS.
    ; All other registers are restored to their syscall entry state.
    sysretq