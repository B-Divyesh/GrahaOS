; arch/x86_64/boot/ap_start.S
bits 64
section .text

global ap_trampoline
extern ap_main
extern g_kernel_pml4

; This is the entry point for Application Processors
; Limine starts APs in 64-bit long mode with:
; - RDI = pointer to limine_mp_info structure
; The limine_mp_info struct layout (from limine.h):
;   uint32_t processor_id;      ; offset 0
;   uint32_t lapic_id;          ; offset 4
;   uint64_t reserved;          ; offset 8
;   limine_goto_address goto;   ; offset 16
;   uint64_t extra_argument;    ; offset 24  <-- Our stack pointer is here!

ap_trampoline:
    ; Disable interrupts during initialization
    cli
    
    ; 1. Set up the stack from the MP info structure
    ; RDI contains pointer to limine_mp_info
    ; Stack pointer is at offset 24 (extra_argument)
    mov rsp, [rdi + 24]
    
    ; Align stack to 16 bytes (System V ABI requirement)
    and rsp, ~0xF
    
    ; Reserve red zone (128 bytes below RSP)
    sub rsp, 128
    
    ; 2. Load the kernel's page tables
    ; The BSP stored the PML4 physical address in g_kernel_pml4
    mov rax, [rel g_kernel_pml4]
    mov cr3, rax
    
    ; 3. Set up segment registers (should already be set by Limine, but be safe)
    mov ax, 0x10    ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; 4. Enable SSE/SSE2 (required for modern compilers)
    mov rax, cr0
    and eax, 0xFFFFFFFB      ; Clear EM bit (bit 2)
    or eax, 0x2              ; Set MP bit (bit 1)
    mov cr0, rax
    
    mov rax, cr4
    or eax, 0x600            ; Set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
    mov cr4, rax
    
    ; 5. Jump to C entry point
    ; RDI already contains limine_mp_info pointer (first argument)
    call ap_main
    
    ; 6. If ap_main returns (it shouldn't), halt forever
.hang:
    cli
    hlt
    jmp .hang   