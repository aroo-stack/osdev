MBALIGN  equ  1<<0
MEMINFO  equ  1<<1
FLAGS    equ  MBALIGN | MEMINFO
MAGIC    equ  0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main
_start:
    mov esp, stack_top
    ; Multiboot: EAX=0x2BADB002 magic, EBX=mbi ptr. Preserve via cdecl args.
    push ebx        ; second arg: mbi_addr
    push eax        ; first arg: magic
    call kernel_main
    add esp, 8
    cli
.hang:
    hlt
    jmp .hang
