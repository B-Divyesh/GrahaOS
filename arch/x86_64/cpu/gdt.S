bits 64
global gdt_load, tss_load

gdt_load:
    ; Load GDT pointer from first argument (RDI)
    lgdt [rdi]
    
    ; Reload data segment registers with new kernel data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Perform far jump to reload CS with kernel code selector (0x08)
    ; We can't directly load CS, so we use a far return
    push 0x08              ; Push new CS selector
    lea rax, [rel .reload_cs]  ; Load address of next instruction
    push rax               ; Push return address
    retfq                  ; Far return to reload CS
.reload_cs:
    ret

; --- NEW: Function to load Task Register ---
tss_load:
    ; Load the Task Register. The argument is the TSS selector.
    mov ax, di
    ltr ax
    ret