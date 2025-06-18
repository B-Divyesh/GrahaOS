bits 64
section .text
global _start_user

_start_user:
    ; This is our first user-mode code.
    ; It will call SYS_TEST and then loop forever.
.loop:
    mov rax, 0  ; Syscall number for SYS_TEST
    syscall
    jmp .loop