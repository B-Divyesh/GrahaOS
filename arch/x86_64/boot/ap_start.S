; arch/x86_64/boot/ap_start.S
bits 64
section .text

global ap_trampoline
extern ap_main
extern g_kernel_pml4

; Entry point for Application Processors
; Limine starts APs in 64-bit long mode with:
; - RDI = pointer to limine_mp_info structure

ap_trampoline:
    ; Disable interrupts immediately
    cli
    
    ; Save the info pointer
    push rdi
    
    ; Load kernel's page tables
    mov rax, [rel g_kernel_pml4]
    test rax, rax
    jz .halt_ap  ; If PML4 is not set, halt
    mov cr3, rax
    
    ; Set up proper segment registers
    mov ax, 0x10    ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Restore info pointer
    pop rdi
    
    ; Get the stack pointer from the info structure
    ; Stack pointer is at offset 24 (extra_argument)
    mov rsp, [rdi + 24]
    test rsp, rsp
    jz .halt_ap  ; If no stack, halt
    
    ; Validate stack is in kernel space
    mov rax, rsp
    mov rbx, 0xFFFF800000000000
    cmp rax, rbx
    jb .halt_ap  ; Stack not in kernel space
    
    ; Align stack to 16 bytes
    and rsp, ~0xF
    
    ; Reserve space for red zone
    sub rsp, 128
    
    ; Push a canary value for debugging
    mov rax, 0xDEADBEEFDEADBEEF
    push rax
    
    ; Enable SSE/SSE2 (required for modern compilers)
    mov rax, cr0
    and eax, 0xFFFFFFFB  ; Clear EM bit
    or eax, 0x2          ; Set MP bit
    mov cr0, rax
    
    mov rax, cr4
    or eax, 0x600        ; Set OSFXSR and OSXMMEXCPT
    mov cr4, rax
    
    ; Clear the direction flag
    cld
    
    ; Call C entry point
    ; RDI already contains limine_mp_info pointer
    call ap_main
    
    ; If ap_main returns (it shouldn't), halt
.halt_ap:
    cli
    hlt
    jmp .halt_ap