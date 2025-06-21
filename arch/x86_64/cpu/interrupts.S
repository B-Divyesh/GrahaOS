; arch/x86_64/cpu/interrupts.S
bits 64
section .text
extern interrupt_handler

; Macro to create interrupt stubs that the IDT will point to.
; %1 = interrupt number, %2 = 1 if CPU pushes error code, 0 if not.
%macro isr_stub 2
isrstub%1:
    %if %2 == 0
        push 0          ; Push a dummy error code if the CPU doesn't.
    %endif
    push %1             ; Push the interrupt number.
    jmp isr_common      ; Jump to the common handler.
%endmacro

; Common handler for all interrupts.
isr_common:
    ; --- CRITICAL FIX: CORRECT PUSH ORDER ---
    ; Push GPRs in r15..rax order to match the C struct memory layout.
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

    ; Pass a pointer to the now-correct interrupt_frame to the C handler.
    mov rdi, rsp
    call interrupt_handler

    ; Restore all GPRs in the exact reverse order.
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

    ; Pop the interrupt number and error code.
    add rsp, 16

    ; Return from the interrupt.
    iretq

; Generate stubs for all 256 interrupts with correct error code flags
isr_stub 0, 0   ; Divide by Zero
isr_stub 1, 0   ; Debug
isr_stub 2, 0   ; Non-maskable Interrupt
isr_stub 3, 0   ; Breakpoint
isr_stub 4, 0   ; Overflow
isr_stub 5, 0   ; Bound Range Exceeded
isr_stub 6, 0   ; Invalid Opcode
isr_stub 7, 0   ; Device Not Available
isr_stub 8, 1   ; Double Fault (has error code)
isr_stub 9, 0   ; Coprocessor Segment Overrun
isr_stub 10, 1  ; Invalid TSS (has error code)
isr_stub 11, 1  ; Segment Not Present (has error code)
isr_stub 12, 1  ; Stack-Segment Fault (has error code)
isr_stub 13, 1  ; General Protection Fault (has error code)
isr_stub 14, 1  ; Page Fault (has error code)
isr_stub 15, 0  ; Reserved
isr_stub 16, 0  ; x87 Floating-Point Exception
isr_stub 17, 1  ; Alignment Check (has error code)
isr_stub 18, 0  ; Machine Check
isr_stub 19, 0  ; SIMD Floating-Point Exception
isr_stub 20, 0  ; Virtualization Exception
isr_stub 21, 1  ; Control Protection Exception (has error code)
; Generate remaining stubs (22-255) - these don't have error codes
%assign i 22
%rep (256-22)
    isr_stub i, 0
    %assign i i+1
%endrep

; --- CRITICAL FIX: CREATE THE MISSING ISR_STUB_TABLE ---
; This table is required by idt.c to initialize the IDT.
section .data
global isr_stub_table
isr_stub_table:
    dq isrstub0, isrstub1, isrstub2, isrstub3, isrstub4, isrstub5, isrstub6, isrstub7
    dq isrstub8, isrstub9, isrstub10, isrstub11, isrstub12, isrstub13, isrstub14, isrstub15
    dq isrstub16, isrstub17, isrstub18, isrstub19, isrstub20, isrstub21, isrstub22, isrstub23
    dq isrstub24, isrstub25, isrstub26, isrstub27, isrstub28, isrstub29, isrstub30, isrstub31
    dq isrstub32, isrstub33, isrstub34, isrstub35, isrstub36, isrstub37, isrstub38, isrstub39
    dq isrstub40, isrstub41, isrstub42, isrstub43, isrstub44, isrstub45, isrstub46, isrstub47
    dq isrstub48, isrstub49, isrstub50, isrstub51, isrstub52, isrstub53, isrstub54, isrstub55
    dq isrstub56, isrstub57, isrstub58, isrstub59, isrstub60, isrstub61, isrstub62, isrstub63
    dq isrstub64, isrstub65, isrstub66, isrstub67, isrstub68, isrstub69, isrstub70, isrstub71
    dq isrstub72, isrstub73, isrstub74, isrstub75, isrstub76, isrstub77, isrstub78, isrstub79
    dq isrstub80, isrstub81, isrstub82, isrstub83, isrstub84, isrstub85, isrstub86, isrstub87
    dq isrstub88, isrstub89, isrstub90, isrstub91, isrstub92, isrstub93, isrstub94, isrstub95
    dq isrstub96, isrstub97, isrstub98, isrstub99, isrstub100, isrstub101, isrstub102, isrstub103
    dq isrstub104, isrstub105, isrstub106, isrstub107, isrstub108, isrstub109, isrstub110, isrstub111
    dq isrstub112, isrstub113, isrstub114, isrstub115, isrstub116, isrstub117, isrstub118, isrstub119
    dq isrstub120, isrstub121, isrstub122, isrstub123, isrstub124, isrstub125, isrstub126, isrstub127
    dq isrstub128, isrstub129, isrstub130, isrstub131, isrstub132, isrstub133, isrstub134, isrstub135
    dq isrstub136, isrstub137, isrstub138, isrstub139, isrstub140, isrstub141, isrstub142, isrstub143
    dq isrstub144, isrstub145, isrstub146, isrstub147, isrstub148, isrstub149, isrstub150, isrstub151
    dq isrstub152, isrstub153, isrstub154, isrstub155, isrstub156, isrstub157, isrstub158, isrstub159
    dq isrstub160, isrstub161, isrstub162, isrstub163, isrstub164, isrstub165, isrstub166, isrstub167
    dq isrstub168, isrstub169, isrstub170, isrstub171, isrstub172, isrstub173, isrstub174, isrstub175
    dq isrstub176, isrstub177, isrstub178, isrstub179, isrstub180, isrstub181, isrstub182, isrstub183
    dq isrstub184, isrstub185, isrstub186, isrstub187, isrstub188, isrstub189, isrstub190, isrstub191
    dq isrstub192, isrstub193, isrstub194, isrstub195, isrstub196, isrstub197, isrstub198, isrstub199
    dq isrstub200, isrstub201, isrstub202, isrstub203, isrstub204, isrstub205, isrstub206, isrstub207
    dq isrstub208, isrstub209, isrstub210, isrstub211, isrstub212, isrstub213, isrstub214, isrstub215
    dq isrstub216, isrstub217, isrstub218, isrstub219, isrstub220, isrstub221, isrstub222, isrstub223
    dq isrstub224, isrstub225, isrstub226, isrstub227, isrstub228, isrstub229, isrstub230, isrstub231
    dq isrstub232, isrstub233, isrstub234, isrstub235, isrstub236, isrstub237, isrstub238, isrstub239
    dq isrstub240, isrstub241, isrstub242, isrstub243, isrstub244, isrstub245, isrstub246, isrstub247
    dq isrstub248, isrstub249, isrstub250, isrstub251, isrstub252, isrstub253, isrstub254, isrstub255