; arch/x86_64/cpu/interrupts.S
bits 64
section .text

global isr_stub_table
extern interrupt_handler

; Macro for interrupt stubs
%macro isr_stub 2
global isrstub%1
isrstub%1:
    %if %2 == 0
        push qword 0    ; Dummy error code
    %endif
    push qword %1       ; Interrupt number
    jmp isr_common
%endmacro

; Common interrupt handler
isr_common:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Save data segment
    mov ax, ds
    push rax
    
    ; Load kernel segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    
    ; Check if we have a valid stack
    mov rax, rsp
    mov rbx, 0xFFFF800000000000
    cmp rax, rbx
    jb .invalid_stack
    
    ; Pass pointer to interrupt frame
    mov rdi, rsp
    
    ; Ensure stack alignment for call
    mov rbp, rsp
    and rsp, ~0xF
    
    ; Clear direction flag
    cld
    
    ; Call C handler
    call interrupt_handler
    
    ; Restore stack pointer
    mov rsp, rbp
    
    ; Restore data segment
    pop rax
    mov ds, ax
    mov es, ax
    
    ; Restore all registers
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
    
    ; Remove error code and interrupt number
    add rsp, 16
    
    ; Return from interrupt
    iretq

.invalid_stack:
    ; Stack is corrupted - try to recover
    ; Load emergency stack if available
    mov rax, 0xFFFF800001FFF000  ; Emergency stack address
    mov rsp, rax
    
    ; Try to show error
    mov rdi, 0xDEADC0DE  ; Error code for stack corruption
    call interrupt_handler
    
    ; Halt
    cli
    hlt
    jmp $

; Generate all interrupt stubs
isr_stub 0, 0   ; Divide by zero
isr_stub 1, 0   ; Debug
isr_stub 2, 0   ; NMI
isr_stub 3, 0   ; Breakpoint
isr_stub 4, 0   ; Overflow
isr_stub 5, 0   ; Bound range exceeded
isr_stub 6, 0   ; Invalid opcode
isr_stub 7, 0   ; Device not available
isr_stub 8, 1   ; Double fault
isr_stub 9, 0   ; Coprocessor segment overrun
isr_stub 10, 1  ; Invalid TSS
isr_stub 11, 1  ; Segment not present
isr_stub 12, 1  ; Stack segment fault
isr_stub 13, 1  ; General protection fault
isr_stub 14, 1  ; Page fault
isr_stub 15, 0  ; Reserved
isr_stub 16, 0  ; x87 FPE
isr_stub 17, 1  ; Alignment check
isr_stub 18, 0  ; Machine check
isr_stub 19, 0  ; SIMD FPE
isr_stub 20, 0  ; Virtualization
isr_stub 21, 1  ; Control protection

; Continue for all 256 interrupts
%assign i 22
%rep 234
    isr_stub i, 0
    %assign i i+1
%endrep

; Export interrupt stub table
section .data
align 8
isr_stub_table:
%assign i 0
%rep 256
    dq isrstub%[i]
    %assign i i+1
%endrep