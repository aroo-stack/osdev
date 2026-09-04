; sched.s - PIT preemptive scheduler, 100Hz, NASM
; Uses same flat selectors 0x08/0x10

global pit_handler
extern scheduler_tick
extern current_task
extern task_list
extern g_in_redraw
extern g_needs_redraw

; Task struct esp is at offset 0
%define TASK_ESP 0

pit_handler:
    cli
    push byte 0          ; dummy error code for IRQ
    push byte 32         ; vector 32 (PIT)
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; EOI to master PIC - must be early, before scheduler may switch
    mov al, 0x20
    out 0x20, al

    ; Save current task's ESP
    mov eax, [current_task]
    mov ebx, [task_list + eax*4]
    mov [ebx + TASK_ESP], esp

    ; Pick next task (round-robin) - updates current_task
    call scheduler_tick

    ; Load next task's ESP
    mov eax, [current_task]
    mov ebx, [task_list + eax*4]
    mov esp, [ebx + TASK_ESP]

    ; Restore
    pop ebx
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    popa
    add esp, 8           ; skip vector and error code
    sti
    iret
