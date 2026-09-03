; gdt_flush.s - NASM, called from C as gdt_flush(uint32_t gp_ptr)
; Loads GDTR, far-jumps to reload CS, then reloads data selectors.
global gdt_flush
section .text
gdt_flush:
    mov eax, [esp+4]     ; gp pointer arg
    lgdt [eax]

    jmp 0x08:.flush      ; far jump to selector 0x08 (code segment index 1)

.flush:
    mov ax, 0x10         ; selector 0x10 (data segment index 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
