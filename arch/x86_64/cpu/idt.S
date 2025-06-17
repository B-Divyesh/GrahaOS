bits 64

global idt_load

idt_load:
    ; Load IDT pointer from first argument (RDI)
    lidt [rdi]
    ret
